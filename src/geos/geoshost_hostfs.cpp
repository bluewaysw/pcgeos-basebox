// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file geoshost_hostfs.cpp
 * @brief GEOS host interface, host file system sub-API (see header).
 */

#include "dosbox.h"

#if C_GEOSHOST

#include "geoshost_hostfs.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "geoshost_mem.h"
#include "hostfs_volume.h"

using namespace GeosGuestMem;
using namespace HostFs;

namespace {

constexpr uint16_t HIF_OK        = 0;
constexpr uint16_t HIF_NOT_FOUND = 1;
constexpr uint16_t HIF_FAILED    = 7;

// FileChangeNotificationType
constexpr uint16_t FCNT_CREATE     = 0;
constexpr uint16_t FCNT_RENAME     = 1;
constexpr uint16_t FCNT_DELETE     = 3;
constexpr uint16_t FCNT_CONTENTS   = 4;
constexpr uint16_t FCNT_ATTRIBUTES = 5;

constexpr size_t FeadSize   = 12; // FileExtAttrDesc
constexpr size_t MaxPath    = 512;
constexpr size_t NotifyName = 36;

std::vector<std::unique_ptr<Volume>> volumes;
Volume* volume = nullptr; // the volume of the request being executed
std::vector<uint8_t> io(0x10000);

// File handles given to GEOS are global, so handle operations (and
// DR_FS_COMPARE_FILES, which only gets handles) find their volume.
struct GlobalHandle {
	bool used      = false;
	uint8_t vol    = 0;
	uint16_t local = 0;
};
std::vector<GlobalHandle> handles;

uint16_t add_handle(const uint8_t vol, const uint16_t local)
{
	for (size_t i = 0; i < handles.size(); ++i) {
		if (!handles[i].used) {
			handles[i] = {true, vol, local};
			return static_cast<uint16_t>(i + 1);
		}
	}
	handles.push_back({true, vol, local});
	return static_cast<uint16_t>(handles.size());
}

const GlobalHandle* find_handle(const uint16_t h)
{
	return (h && h <= handles.size() && handles[h - 1].used) ? &handles[h - 1] : nullptr;
}

/* ----------------------------------------------------------------
 * Frame
 * ---------------------------------------------------------------- */

struct Frame {
	uint16_t func = 0, result = 0;
	uint16_t ax = 0, bx = 0, cx = 0, dx = 0, si = 0, di = 0, ds = 0, es = 0;
	uint32_t dir_id = 0;
	uint16_t handle = 0, disk = 0, path_info = 0, drive_status = 0;
	uint32_t aux = 0, aux_id = 0;
	uint16_t notify_type = 0;
	uint32_t notify_id   = 0;
	char notify_name[NotifyName] = {};
	uint16_t notify2_type = 0;
	uint32_t notify2_id   = 0;
	uint16_t volume       = 0;

	uint8_t al() const { return static_cast<uint8_t>(ax); }
	uint8_t cl() const { return static_cast<uint8_t>(cx); }
	uint8_t ch() const { return static_cast<uint8_t>(cx >> 8); }

	KernelContext ctx() const { return {disk, path_info, drive_status}; }

	void notify(const uint16_t type, const uint32_t id, const char* name = nullptr)
	{
		if (!notify_type) {
			notify_type = type + 1;
			notify_id   = id;
			if (name) {
				std::strncpy(notify_name, name, NotifyName - 1);
			}
		} else {
			notify2_type = type + 1;
			notify2_id   = id;
		}
	}
	void id_out(const uint32_t id)
	{
		cx = static_cast<uint16_t>(id >> 16);
		dx = static_cast<uint16_t>(id);
	}
};

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return rd16(p) | (static_cast<uint32_t>(rd16(p + 2)) << 16); }
void wr16(uint8_t* p, const uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void wr32(uint8_t* p, const uint32_t v) { wr16(p, static_cast<uint16_t>(v)); wr16(p + 2, static_cast<uint16_t>(v >> 16)); }

void decode(const uint8_t* b, Frame& f)
{
	f.func   = rd16(b + 0);
	f.ax     = rd16(b + 4);
	f.bx     = rd16(b + 6);
	f.cx     = rd16(b + 8);
	f.dx     = rd16(b + 10);
	f.si     = rd16(b + 12);
	f.di     = rd16(b + 14);
	f.ds     = rd16(b + 16);
	f.es     = rd16(b + 18);
	f.dir_id = rd32(b + 20);
	f.handle = rd16(b + 24);
	f.disk   = rd16(b + 26);
	f.path_info    = rd16(b + 28);
	f.drive_status = rd16(b + 30);
	f.aux    = rd32(b + 32);
	f.aux_id = rd32(b + 36);
	f.volume = rd16(b + 88);
}

void encode(const Frame& f, uint8_t* b)
{
	wr16(b + 2, f.result);
	wr16(b + 4, f.ax);
	wr16(b + 6, f.bx);
	wr16(b + 8, f.cx);
	wr16(b + 10, f.dx);
	wr16(b + 12, f.si);
	wr16(b + 14, f.di);
	wr16(b + 40, f.notify_type);
	wr32(b + 42, f.notify_id);
	std::memcpy(b + 46, f.notify_name, NotifyName);
	wr16(b + 82, f.notify2_type);
	wr32(b + 84, f.notify2_id);
}

/* ----------------------------------------------------------------
 * Guest memory
 * ---------------------------------------------------------------- */

bool read_string(const uint16_t seg, const uint16_t off, std::string& out)
{
	GuestSpan span;
	if (!resolve(seg, off, Access::Read, span)) {
		return false;
	}
	out.clear();
	const uint32_t max = std::min<uint32_t>(span.avail, MaxPath);
	for (uint32_t i = 0; i < max; ++i) {
		const char c = static_cast<char>(mem_readb(span.base + i));
		if (!c) {
			return true;
		}
		out.push_back(c);
	}
	return false;
}

bool read_bytes(const uint16_t seg, const uint16_t off, uint8_t* buf, const uint32_t n)
{
	PhysPt lin = 0;
	if (n && !resolve_len(seg, off, n, Access::Read, lin)) {
		return false;
	}
	if (n) {
		MEM_BlockRead(lin, buf, n);
	}
	return true;
}

bool write_bytes(const uint16_t seg, const uint16_t off, const uint8_t* buf, const uint32_t n)
{
	PhysPt lin = 0;
	if (n && !resolve_len(seg, off, n, Access::Write, lin)) {
		return false;
	}
	if (n) {
		MEM_BlockWrite(lin, buf, n);
	}
	return true;
}

bool is_text_attr(const uint16_t attr)
{
	return attr == FEA_NAME || attr == FEA_USER_NOTES || attr == FEA_NOTICE ||
	       attr == FEA_PASSWORD || attr == FEA_DOS_NAME;
}

/** Store one attribute value into a guest buffer of @p size bytes. */
Error store_value(const AttrValue& v, const uint16_t attr, const uint16_t seg,
                  const uint16_t off, const uint16_t size)
{
	if (v.bytes.size() > size && !is_text_attr(attr)) {
		return ERROR_ATTR_SIZE_MISMATCH;
	}
	std::vector<uint8_t> buf(size, 0);
	std::memcpy(buf.data(), v.bytes.data(), std::min<size_t>(size, v.bytes.size()));
	if (is_text_attr(attr) && size && v.bytes.size() >= size) {
		buf[size - 1] = 0; // truncated text stays NUL-terminated
	}
	return write_bytes(seg, off, buf.data(), size) ? OK : ERROR_GENERAL_FAILURE;
}

/** FileExtAttrDesc array access */
struct Fead {
	uint16_t attr = 0, value_off = 0, value_seg = 0, size = 0;
};

bool read_fead(const uint16_t seg, const uint16_t off, Fead& d)
{
	uint8_t b[FeadSize];
	if (!read_bytes(seg, off, b, FeadSize)) {
		return false;
	}
	d.attr      = rd16(b);
	d.value_off = rd16(b + 2);
	d.value_seg = rd16(b + 4);
	d.size      = rd16(b + 6);
	return true;
}

/* ----------------------------------------------------------------
 * Extended attributes on an item (path or handle)
 * ---------------------------------------------------------------- */

Error get_ext(const Item& item, Frame& f)
{
	const uint16_t attr = f.ax;
	if (attr != FEA_MULTIPLE) {
		AttrValue v;
		if (const auto e = volume->get_ext_attr(item, attr, f.ctx(), v); e != OK) {
			return e;
		}
		return store_value(v, attr, f.es, f.di, f.cx);
	}
	Error result = OK;
	for (uint16_t i = 0; i < f.cx; ++i) {
		Fead d;
		if (!read_fead(f.es, static_cast<uint16_t>(f.di + i * FeadSize), d)) {
			return ERROR_GENERAL_FAILURE;
		}
		AttrValue v;
		Error e = volume->get_ext_attr(item, d.attr, f.ctx(), v);
		if (e == OK) {
			e = store_value(v, d.attr, d.value_seg, d.value_off, d.size);
		}
		if (e != OK && result == OK) {
			result = e; // report the first problem, fill the rest
		}
	}
	return result;
}

Error set_ext(const uint32_t base, const std::string& path, const uint16_t handle,
              Frame& f, bool& changed)
{
	changed = false;
	auto set_one = [&](const uint16_t attr, const uint16_t seg, const uint16_t off,
	                   const uint16_t size) -> Error {
		std::vector<uint8_t> value(size);
		if (!read_bytes(seg, off, value.data(), size)) {
			return ERROR_GENERAL_FAILURE;
		}
		const Error e = volume->set_ext_attr(base, path, handle, attr, value.data(), size);
		changed |= e == OK;
		return e;
	};
	if (f.ax != FEA_MULTIPLE) {
		return set_one(f.ax, f.es, f.di, f.cx);
	}
	Error result = OK;
	for (uint16_t i = 0; i < f.cx; ++i) {
		Fead d;
		if (!read_fead(f.es, static_cast<uint16_t>(f.di + i * FeadSize), d)) {
			return ERROR_GENERAL_FAILURE;
		}
		const Error e = set_one(d.attr, d.value_seg, d.value_off, d.size);
		if (e != OK && result == OK) {
			result = e;
		}
	}
	return result;
}

/** GET_ALL: size phase (ax = bytes, cx = count) and fill phase (es = block). */
Error get_all(const Item& item, Frame& f, const bool fill)
{
	const auto attrs = volume->all_ext_attrs(item);
	std::vector<AttrValue> values(attrs.size());
	size_t bytes = attrs.size() * FeadSize;
	for (size_t i = 0; i < attrs.size(); ++i) {
		volume->get_ext_attr(item, attrs[i], f.ctx(), values[i]);
		bytes += values[i].bytes.size();
	}
	if (bytes > 0xfff0) {
		return ERROR_GENERAL_FAILURE;
	}
	f.cx = static_cast<uint16_t>(attrs.size());
	f.ax = static_cast<uint16_t>(bytes);
	if (!fill) {
		return OK;
	}
	std::vector<uint8_t> block(bytes, 0);
	size_t value_off = attrs.size() * FeadSize;
	for (size_t i = 0; i < attrs.size(); ++i) {
		uint8_t* d = block.data() + i * FeadSize;
		wr16(d, attrs[i]);
		wr16(d + 2, static_cast<uint16_t>(value_off));
		wr16(d + 4, f.es);
		wr16(d + 6, static_cast<uint16_t>(values[i].bytes.size()));
		std::memcpy(block.data() + value_off, values[i].bytes.data(), values[i].bytes.size());
		value_off += values[i].bytes.size();
	}
	return write_bytes(f.es, 0, block.data(), static_cast<uint32_t>(bytes))
	             ? OK
	             : ERROR_GENERAL_FAILURE;
}

/* ----------------------------------------------------------------
 * Enumeration: fill the kernel's FileEnumCallbackData for one item
 * ---------------------------------------------------------------- */

Error fill_enum(const Item& item, Frame& f)
{
	for (uint16_t off = 0;; off = static_cast<uint16_t>(off + FeadSize)) {
		Fead d;
		if (!read_fead(f.ds, off, d)) {
			return ERROR_GENERAL_FAILURE;
		}
		if (d.attr == FEA_END_OF_LIST) {
			return OK;
		}
		AttrValue v;
		bool present = volume->get_ext_attr(item, d.attr, f.ctx(), v) == OK &&
		               store_value(v, d.attr, f.ds, d.value_off, d.size) == OK;
		// FEAD_value.segment = ds if stored, 0 if the file has no such attr
		uint8_t seg[2];
		wr16(seg, present ? f.ds : 0);
		if (!write_bytes(f.ds, static_cast<uint16_t>(off + 4), seg, 2)) {
			return ERROR_GENERAL_FAILURE;
		}
	}
}

/* ----------------------------------------------------------------
 * Dispatch
 * ---------------------------------------------------------------- */

Error run(Frame& f)
{
	std::string path;
	const bool path_func = f.func < HFF_READ;
	if (path_func && !read_string(f.ds, f.dx, path)) {
		return ERROR_PATH_NOT_FOUND;
	}
	const uint32_t base = f.dir_id ? f.dir_id : volume->root_id();

	switch (f.func) {
	case HFF_CUR_PATH_SET: {
		Lookup lk;
		if (const auto e = volume->lookup(base, path, lk); e != OK) {
			return e;
		}
		if (!lk.exists || !lk.item.is_dir) {
			return ERROR_PATH_NOT_FOUND;
		}
		f.id_out(lk.item.id);
		return OK;
	}
	case HFF_OPEN: {
		uint16_t h = 0;
		Item it;
		if (const auto e = volume->open(base, path, f.al(), h, it); e != OK) {
			return e;
		}
		f.ax = h;
		f.id_out(it.id);
		return OK;
	}
	case HFF_CREATE: {
		uint16_t h = 0;
		Item it;
		Lookup parent;
		if (const auto e = volume->create(base, path, f.cl(), f.ch(), f.al(), h, it); e != OK) {
			return e;
		}
		volume->lookup(base, path, parent);
		f.ax = h;
		f.id_out(it.id);
		f.notify(FCNT_CREATE, parent.parent_id, it.long_name);
		return OK;
	}
	case HFF_CREATE_DIR: {
		Item it;
		if (const auto e = volume->make_dir(base, path, it); e != OK) {
			return e;
		}
		Lookup lk;
		volume->lookup(base, path, lk);
		f.notify(FCNT_CREATE, lk.parent_id, it.long_name);
		return OK;
	}
	case HFF_DELETE_DIR:
	case HFF_DELETE_FILE: {
		uint32_t id = 0;
		const auto e = f.func == HFF_DELETE_DIR ? volume->remove_dir(base, path, id)
		                                        : volume->remove_file(base, path, id);
		if (e == OK) {
			f.notify(FCNT_DELETE, id);
		}
		return e;
	}
	case HFF_RENAME: {
		std::string new_name;
		if (!read_string(f.bx, f.cx, new_name)) {
			return ERROR_INVALID_NAME;
		}
		Lookup before;
		volume->lookup(base, path, before);
		Item it;
		if (const auto e = volume->rename(base, path, new_name, it); e != OK) {
			return e;
		}
		f.notify(FCNT_RENAME, before.item.id, it.long_name);
		return OK;
	}
	case HFF_MOVE: {
		std::string dest;
		if (!read_string(static_cast<uint16_t>(f.aux >> 16), static_cast<uint16_t>(f.aux), dest)) {
			return ERROR_PATH_NOT_FOUND;
		}
		const uint32_t dest_base = f.aux_id ? f.aux_id : volume->root_id();
		Lookup before;
		volume->lookup(base, path, before);
		Item it;
		if (const auto e = volume->move(base, path, dest_base, dest, it); e != OK) {
			return e;
		}
		Lookup after;
		volume->lookup(dest_base, dest, after);
		f.notify(FCNT_CREATE, after.parent_id, it.long_name);
		f.notify(FCNT_DELETE, before.item.id);
		return OK;
	}
	case HFF_GET_ATTRS: {
		uint8_t a = 0;
		if (const auto e = volume->get_attrs(base, path, a); e != OK) {
			return e;
		}
		f.cx = a;
		return OK;
	}
	case HFF_SET_ATTRS: {
		Lookup lk;
		volume->lookup(base, path, lk);
		const auto e = volume->set_attrs(base, path, f.cl());
		if (e == OK) {
			f.notify(FCNT_ATTRIBUTES, lk.item.id);
		}
		return e;
	}
	case HFF_PATH_GET_EXT:
	case HFF_PATH_GET_ALL_SIZE:
	case HFF_PATH_GET_ALL_FILL: {
		Lookup lk;
		if (const auto e = volume->lookup(base, path, lk); e != OK) {
			return e;
		}
		if (!lk.exists) {
			return ERROR_FILE_NOT_FOUND;
		}
		if (f.func == HFF_PATH_GET_EXT) {
			return get_ext(lk.item, f);
		}
		return get_all(lk.item, f, f.func == HFF_PATH_GET_ALL_FILL);
	}
	case HFF_PATH_SET_EXT: {
		Lookup lk;
		volume->lookup(base, path, lk);
		bool changed = false;
		const auto e = set_ext(base, path, 0, f, changed);
		if (changed) {
			f.notify(FCNT_ATTRIBUTES, lk.item.id);
		}
		return e;
	}

	case HFF_READ: {
		uint16_t got = 0;
		PhysPt lin = 0;
		if (f.cx && !resolve_len(f.ds, f.dx, f.cx, Access::Write, lin)) {
			return ERROR_GENERAL_FAILURE;
		}
		if (const auto e = volume->read(f.handle, io.data(), f.cx, got); e != OK) {
			return e;
		}
		if (got) {
			MEM_BlockWrite(lin, io.data(), got);
		}
		f.ax = got;
		return OK;
	}
	case HFF_WRITE: {
		if (!read_bytes(f.ds, f.dx, io.data(), f.cx)) {
			return ERROR_GENERAL_FAILURE;
		}
		uint16_t put = 0;
		const auto e = volume->write(f.handle, io.data(), f.cx, put);
		f.ax = put;
		return e;
	}
	case HFF_POSITION: {
		uint32_t pos = 0;
		const auto off = static_cast<int32_t>((static_cast<uint32_t>(f.cx) << 16) | f.dx);
		if (const auto e = volume->seek(f.handle, f.al(), off, pos); e != OK) {
			return e;
		}
		f.dx = static_cast<uint16_t>(pos >> 16);
		f.ax = static_cast<uint16_t>(pos);
		return OK;
	}
	case HFF_TRUNCATE:
		return volume->truncate(f.handle, (static_cast<uint32_t>(f.cx) << 16) | f.dx);
	case HFF_COMMIT: {
		bool dirty = false;
		volume->is_dirty(f.handle, dirty);
		const auto e = volume->commit(f.handle);
		Item it;
		if (e == OK && dirty && volume->handle_item(f.handle, it) == OK) {
			f.notify(FCNT_CONTENTS, it.id);
		}
		return e;
	}
	case HFF_GET_DATE_TIME: return volume->get_date_time(f.handle, f.dx, f.cx);
	case HFF_SET_DATE_TIME: return volume->set_date_time(f.handle, f.dx, f.cx);
	case HFF_FILE_SIZE: {
		uint32_t size = 0;
		if (const auto e = volume->file_size(f.handle, size); e != OK) {
			return e;
		}
		f.dx = static_cast<uint16_t>(size >> 16);
		f.ax = static_cast<uint16_t>(size);
		return OK;
	}
	case HFF_ADD_REFERENCE: return volume->add_ref(f.handle);
	case HFF_CHECK_DIRTY: {
		bool dirty = false;
		const auto e = volume->is_dirty(f.handle, dirty);
		f.ax = dirty ? 1 : 0;
		return e;
	}
	case HFF_CLOSE: {
		bool closed = false, dirty = false;
		uint32_t id = 0;
		if (const auto e = volume->close(f.handle, closed, dirty, id); e != OK) {
			return e;
		}
		f.ax = closed ? 1 : 0;
		f.id_out(id);
		if (closed && dirty) {
			f.notify(FCNT_CONTENTS, id);
		}
		return OK;
	}
	case HFF_GET_FILE_ID: {
		Item it;
		if (const auto e = volume->handle_item(f.handle, it); e != OK) {
			return e;
		}
		f.id_out(it.id);
		return OK;
	}
	case HFF_CHECK_NATIVE: {
		Item it;
		if (const auto e = volume->handle_item(f.handle, it); e != OK) {
			return e;
		}
		const bool native        = !it.has_header;
		const bool want_native   = (f.ch() & FCF_NATIVE) != 0;
		f.ax = native == want_native ? 1 : 0;
		return OK;
	}
	case HFF_HANDLE_GET_EXT:
	case HFF_HANDLE_GET_ALL_SIZE:
	case HFF_HANDLE_GET_ALL_FILL: {
		Item it;
		if (const auto e = volume->handle_item(f.handle, it); e != OK) {
			return e;
		}
		if (f.func == HFF_HANDLE_GET_EXT) {
			return get_ext(it, f);
		}
		return get_all(it, f, f.func == HFF_HANDLE_GET_ALL_FILL);
	}
	case HFF_HANDLE_SET_EXT: {
		bool changed = false;
		const auto e = set_ext(0, "", f.handle, f, changed);
		Item it;
		if (changed && volume->handle_item(f.handle, it) == OK) {
			f.notify(FCNT_ATTRIBUTES, it.id);
		}
		return e;
	}

	case HFF_ENUM_BEGIN: {
		uint16_t h = 0;
		if (const auto e = volume->enum_begin(base, h); e != OK) {
			return e;
		}
		f.ax = h;
		return OK;
	}
	case HFF_ENUM_NEXT: {
		Item it;
		if (const auto e = volume->enum_next(f.ax, it); e != OK) {
			return e;
		}
		return fill_enum(it, f);
	}
	case HFF_ENUM_END: volume->enum_end(f.ax); return OK;

	case HFF_POLL_CHANGES: {
		ChangeEvent e;
		if (!volume->next_change(e)) {
			return ERROR_NO_MORE_FILES;
		}
		switch (e.type) {
		case ChangeEvent::Created: f.notify(FCNT_CREATE, e.id, e.name.c_str()); break;
		case ChangeEvent::Deleted: f.notify(FCNT_DELETE, e.id); break;
		case ChangeEvent::Contents: f.notify(FCNT_CONTENTS, e.id); break;
		}
		return OK;
	}
	case HFF_DISK_INFO: {
		uint32_t free_bytes = 0, total = 0;
		volume->space(free_bytes, total);
		f.dx = static_cast<uint16_t>(free_bytes >> 16);
		f.ax = static_cast<uint16_t>(free_bytes);
		f.cx = static_cast<uint16_t>(total >> 16);
		f.bx = static_cast<uint16_t>(total);
		return OK;
	}
	default: return ERROR_UNSUPPORTED_FUNCTION;
	}
}

void cmd_init(uint16_t resp[6])
{
	volumes.clear();
	handles.clear();
	volume  = nullptr;
	resp[0] = HIF_OK;
	resp[1] = HOSTFS_MAGIC;
	resp[2] = HIF_API_HOSTFS_VERSION;
}

void cmd_mount(const uint16_t cmd[6], uint16_t resp[6])
{
	std::string line, name, path;
	if (!read_string(cmd[2], cmd[3], line) || !parse_mount_line(line, name, path)) {
		LOG_WARNING("GEOSHOST: [hostfs] drives: can't parse '%s' "
		            "(expected NAME=HOST PATH)",
		            line.c_str());
		resp[0] = HIF_NOT_FOUND;
		return;
	}
	if (volumes.size() >= HOSTFS_MAX_VOLUMES) {
		LOG_WARNING("GEOSHOST: [hostfs] drives: more than %zu drives, ignoring '%s'",
		            HOSTFS_MAX_VOLUMES,
		            name.c_str());
		resp[0] = HIF_FAILED;
		return;
	}
	const char* allow = std::getenv("BASEBOX_GEOS_HOSTFS_ALLOW");
	const std::filesystem::path dir(path);
	if (!path_allowed(dir, allow ? allow : "")) {
		LOG_WARNING("GEOSHOST: [hostfs] drive %s: %s is not in BASEBOX_GEOS_HOSTFS_ALLOW",
		            name.c_str(),
		            path.c_str());
		resp[0] = HIF_NOT_FOUND;
		return;
	}
	auto v = std::make_unique<Volume>(dir, name, path_id(name));
	if (!v->ok()) {
		LOG_WARNING("GEOSHOST: [hostfs] drive %s: %s is not a directory",
		            name.c_str(),
		            path.c_str());
		resp[0] = HIF_NOT_FOUND;
		return;
	}
	// Hand the drive name back for FSDInitDrive
	const uint16_t size = cmd[1];
	std::string n = name;
	n.resize(std::min<size_t>(n.size(), size ? size - 1u : 0u));
	n.push_back('\0');
	if (!size || !write_bytes(cmd[4], cmd[5], reinterpret_cast<const uint8_t*>(n.data()),
	                          static_cast<uint32_t>(n.size()))) {
		resp[0] = HIF_FAILED;
		return;
	}
	LOG_INFO("GEOSHOST: host drive %s serving %s (live, read-write)",
	         name.c_str(),
	         path.c_str());
	resp[0] = HIF_OK;
	resp[1] = static_cast<uint16_t>(volumes.size());
	resp[3] = static_cast<uint16_t>(v->root_id() >> 16);
	resp[4] = static_cast<uint16_t>(v->root_id());
	volumes.push_back(std::move(v));
}

void cmd_call(const uint16_t cmd[6], uint16_t resp[6])
{
	PhysPt lin = 0;
	if (cmd[4] < HOSTFS_FRAME_SIZE ||
	    !resolve_len(cmd[2], cmd[3], HOSTFS_FRAME_SIZE, Access::Write, lin)) {
		resp[0] = HIF_FAILED;
		return;
	}
	uint8_t raw[HOSTFS_FRAME_SIZE];
	MEM_BlockRead(lin, raw, HOSTFS_FRAME_SIZE);
	Frame f;
	decode(raw, f);

	// Route the request to its volume
	const bool handle_op = f.func >= HFF_READ && f.func < HFF_ENUM_BEGIN;
	const uint16_t global = f.handle;
	uint8_t vol = 0;
	if (handle_op) {
		const GlobalHandle* gh = find_handle(global);
		vol      = gh ? gh->vol : 0xff;
		f.handle = gh ? gh->local : 0;
	} else {
		vol = static_cast<uint8_t>(f.volume < volumes.size() ? f.volume : 0xff);
	}
	if (vol >= volumes.size()) {
		f.result = handle_op ? ERROR_GENERAL_FAILURE : ERROR_DISK_UNAVAILABLE;
	} else {
		volume   = volumes[vol].get();
		f.result = run(f);
		volume   = nullptr;
		if (f.result == OK && (f.func == HFF_OPEN || f.func == HFF_CREATE)) {
			f.ax = add_handle(vol, f.ax);
		}
		if (f.result == OK && f.func == HFF_CLOSE && f.ax) {
			handles[global - 1].used = false;
		}
	}
	encode(f, raw);
	MEM_BlockWrite(lin, raw, HOSTFS_FRAME_SIZE);
	resp[0] = HIF_OK;
}

} // namespace

bool GEOSHOST_HOSTFS_Tick()
{
	// Scanning reads the watched directories on the host, so only do it
	// about once a second (ticks are emulated milliseconds).
	static unsigned ticks = 0;
	if (volumes.empty() || ++ticks < 1000) {
		return false;
	}
	ticks = 0;
	// Report every scan that queued something (see GEOSHOST_FS_Tick)
	bool queued_new = false;
	for (auto& v : volumes) {
		const size_t before = v->pending_changes();
		v->poll();
		queued_new |= v->pending_changes() > before;
	}
	return queued_new;
}

void GEOSHOST_HOSTFS_HandleCommand(const uint16_t cmd[6], uint16_t resp[6])
{
	for (int i = 0; i < 6; ++i) {
		resp[i] = 0;
	}
	switch (cmd[0]) {
	case HOSTFS_INIT: cmd_init(resp); break;
	case HOSTFS_MOUNT: cmd_mount(cmd, resp); break;
	case HOSTFS_CALL: cmd_call(cmd, resp); break;
	default: resp[0] = HIF_FAILED; break;
	}
}

#endif // C_GEOSHOST
