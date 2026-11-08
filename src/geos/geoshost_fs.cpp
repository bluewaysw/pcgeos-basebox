// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file geoshost_fs.cpp
 * @brief GEOS host interface, file system sub-API (DOS-compatible fast path).
 *
 * See geoshost_fs.h for the protocol. Design rules:
 *
 *  - Everything goes through DOSBox's DOS core (DOS_* functions), never
 *    directly to the host file system. Handles live in the PSP's JFT and
 *    the global Files[] table exactly as with INT 21h, so fast and regular
 *    calls can be mixed freely, and MOUNT/IMGMOUNT/overlay drives keep
 *    working without any special casing.
 *  - Anything we are not 100% sure about returns HIF_FS_NOT_EXECUTED, so
 *    the guest falls back to a real INT 21h.
 *  - Set DTA (1Ah) is never executed here, only observed: it always goes
 *    on to real DOS so the DOS/DPMI DTA stays correct for everything we
 *    don't handle (FCB searches, finds issued outside the driver, ...).
 *  - Guest memory is validated against the current CPU mode before use.
 */

#include "dosbox.h"

#if C_GEOSHOST

#include "geoshost_fs.h"
#include "geoshost_mem.h"

#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cpu/cpu.h"
#include "cpu/registers.h"
#include "dos/dos.h"
#include "dos/drives.h"
#include "hardware/memory.h"

// Result codes shared with geoshost.cpp
constexpr uint16_t HIF_OK        = 0;
constexpr uint16_t HIF_NOT_FOUND = 1;
constexpr uint16_t HIF_FAILED    = 7;

constexpr uint16_t FrameWords   = HOST_FS_FRAME_SIZE / 2;
constexpr int DtaSize           = sizeof(sDTA); // 43 bytes, DOS find DTA
constexpr int MaxPath           = 256;
constexpr uint16_t FuncMapBytes = 32;

static_assert(sizeof(sDTA) == 0x2b, "DOS find DTA must be 43 bytes");

/* ----------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------- */

static bool fs_disabled_by_env = false;
static bool fs_env_checked     = false;

static bool fs_enabled       = false; // HIF_FS_ENABLE succeeded
static uint16_t fs_guest_psp = 0;     // PSP the guest registered
static bool fs_notify_drives = false; // guest wants HIF_NOTIFY_DRIVE_CHANGE
static bool fs_notify_files  = false; // guest polls HIF_FS_POLL_CHANGES

// DTA observed from the guest's last AH=1Ah. "Armed" only until the next
// request we see: the GEOS driver sets the DTA right before every
// FindFirst/FindNext under the BIOS lock, so a find directly following a
// 1Ah may use it. Anything else (a find without a preceding 1Ah, or any
// call in between) takes the regular path, where DOS/the DPMI host use
// their own, always up-to-date DTA. This rules out writing results
// through a stale pointer into a stack frame that no longer exists.
static bool fast_dta_valid   = false;
static uint16_t fast_dta_seg = 0;
static uint16_t fast_dta_off = 0;

// Private conventional-memory DTA used to run DOS_FindFirst/Next, which
// operate on a real-mode DTA pointer.
static RealPt scratch_dta = 0;

// Drive table snapshot for change detection
struct DriveSnapshot {
	const DOS_Drive* drive = nullptr;
	DosDriveType type      = DosDriveType::Unknown;
	std::string info       = {}; // host path / image name
};
static std::array<DriveSnapshot, DOS_DRIVES> drive_snapshot = {};
static uint16_t drive_generation                           = 1;

// Host bounce buffer for reads/writes (DOS transfers are <= 64 KB)
static std::array<uint8_t, 0x10000> io_buffer = {};

/* ----------------------------------------------------------------
 * Guest memory access
 * ---------------------------------------------------------------- */

using namespace GeosGuestMem;

/** Copy a NUL-terminated string from the guest (bounded by the segment). */
static bool read_guest_string(const uint16_t seg, const uint16_t off,
                              char (&out)[MaxPath])
{
	GuestSpan span;
	if (!resolve(seg, off, Access::Read, span)) {
		return false;
	}
	const uint32_t max = std::min<uint32_t>(span.avail, MaxPath);
	for (uint32_t i = 0; i < max; ++i) {
		out[i] = static_cast<char>(mem_readb(span.base + i));
		if (out[i] == '\0') {
			return true;
		}
	}
	return false; // unterminated or too long: let real DOS deal with it
}

/* ----------------------------------------------------------------
 * Frame and register image
 * ---------------------------------------------------------------- */

// Word order of the guest HostFsFrame (must match os2HostFs.asm)
enum FrameSlot : uint16_t {
	F_AX = 0,
	F_BX,
	F_CX,
	F_DX,
	F_SI,
	F_DI,
	F_DS,
	F_ES,
	F_FLAGS,  // out: bit 0 = carry
	F_EXTERR, // out: extended error code (valid if carry)
};

struct Regs {
	uint16_t ax = 0, bx = 0, cx = 0, dx = 0, si = 0, di = 0, ds = 0, es = 0;
	bool cf = false;

	uint8_t ah() const { return static_cast<uint8_t>(ax >> 8); }
	uint8_t al() const { return static_cast<uint8_t>(ax & 0xff); }
	uint8_t cl() const { return static_cast<uint8_t>(cx & 0xff); }
	uint8_t dl() const { return static_cast<uint8_t>(dx & 0xff); }
	void set_al(const uint8_t v) { ax = static_cast<uint16_t>((ax & 0xff00) | v); }

	void ok() { cf = false; }
	void fail()
	{
		ax = dos.errorcode;
		cf = true;
	}
	void result(const bool success)
	{
		success ? ok() : fail();
	}
};

enum class Outcome { Handled, NotHandled };

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/** True if the JFT entry refers to an open, non-device file. */
static bool is_disk_file_handle(const uint16_t entry)
{
	const uint8_t handle = RealHandle(entry);
	if (handle >= DOS_FILES || !Files[handle]) {
		return false;
	}
	return (Files[handle]->GetInformation() & 0x0080) == 0;
}

static void ensure_scratch_dta()
{
	if (!scratch_dta) {
		// 3 paragraphs = 48 bytes >= 43-byte find DTA
		scratch_dta = RealMake(DOS_GetMemory(3), 0);
	}
}

namespace {
void watch_dos(const char* name, bool is_dir); // host change detection, below
} // namespace

/**
 * Run a FindFirst/FindNext against the fast-path DTA.
 * The guest DTA is copied into a private real-mode DTA (it carries the
 * search state for FindNext), the DOS core runs on it, and the result is
 * copied back. dos.dta() is restored afterwards.
 */
static Outcome do_find(Regs& r, const bool first)
{
	if (!fast_dta_valid) {
		// No 1Ah right before this find: let DOS do it all.
		return Outcome::NotHandled;
	}
	fast_dta_valid = false; // consumed
	PhysPt guest_dta = 0;
	if (!resolve_len(fast_dta_seg, fast_dta_off, DtaSize, Access::Write, guest_dta)) {
		// Real DOS has the same DTA (1Ah was passed through), so the
		// regular path behaves exactly as without the fast path.
		return Outcome::NotHandled;
	}

	char name[MaxPath];
	if (first && !read_guest_string(r.ds, r.dx, name)) {
		return Outcome::NotHandled;
	}
	if (first) {
		watch_dos(name, false);
	}

	ensure_scratch_dta();
	uint8_t dta_bytes[DtaSize];
	MEM_BlockRead(guest_dta, dta_bytes, DtaSize);
	MEM_BlockWrite(RealToPhysical(scratch_dta), dta_bytes, DtaSize);

	const RealPt saved_dta = dos.dta();
	dos.dta(scratch_dta);
	const bool success = first ? DOS_FindFirst(name, FatAttributeFlags(r.cl()))
	                           : DOS_FindNext();
	dos.dta(saved_dta);

	MEM_BlockRead(RealToPhysical(scratch_dta), dta_bytes, DtaSize);
	MEM_BlockWrite(guest_dta, dta_bytes, DtaSize);

	if (success) {
		r.ax = 0; // undocumented, matches DOSBox's INT 21h
		r.ok();
	} else {
		r.fail();
	}
	return Outcome::Handled;
}

/* ----------------------------------------------------------------
 * Host change detection for the DOS IFS driver
 * ---------------------------------------------------------------- */

namespace {

namespace stdfs = std::filesystem;

constexpr size_t MaxDosWatches   = 32;
constexpr uint16_t FCNT_CREATE   = 0;
constexpr uint16_t FCNT_DELETE   = 3;
constexpr uint16_t FCNT_CONTENTS = 4;

struct DosWatch {
	struct Seen {
		std::string short_name = {}; // as DOS shows it ("README.TXT")
		uintmax_t size         = 0;
		stdfs::file_time_type mtime = {};
		bool is_dir            = false;
	};
	uint8_t drive         = 0;
	std::string dos_dir   = {}; // "DIR\SUB", no drive, no leading '\'
	std::string host_dir  = {}; // real host path, for listing the directory
	std::string cache_dir = {}; // basedir + DOS names + separator: the form
	                            // DOSBox's directory cache resolves (its
	                            // FindDirInfo matches path components against
	                            // DOS short names, case-sensitively, and
	                            // silently stops at the parent otherwise)
	uint64_t used        = 0;
	std::unordered_map<std::string, Seen> entries = {}; // by host name
};

struct FsEvent {
	uint16_t type    = 0;
	uint8_t drive    = 0;
	uint32_t id      = 0;
	std::string name = {};
};

std::map<std::string, DosWatch> dos_watches; // by "D:" + dos_dir

uint64_t dos_watch_clock = 0;
std::deque<FsEvent> fs_events;

/** DOSFileChangeCalculateIDLow: value*33 + char, backslashes skipped. */
uint32_t dos_id(uint32_t value, const std::string& s)
{
	for (const char c : s) {
		if (c != '\\') {
			value = value * 33 + static_cast<uint8_t>(c);
		}
	}
	return value;
}

uint32_t dir_id(const std::string& dos_dir)
{
	return dos_id(0x31fe, dos_dir); // DOSFileChangeCalculateID's start value
}

std::shared_ptr<localDrive> local_drive(const uint8_t drive)
{
	return drive < DOS_DRIVES ? std::dynamic_pointer_cast<localDrive>(Drives[drive])
	                          : nullptr;
}

bool is_83(const std::string& n)
{
	const auto dot = n.find('.');
	const std::string base = n.substr(0, dot);
	const std::string ext  = dot == std::string::npos ? "" : n.substr(dot + 1);
	if (base.empty() || base.size() > 8 || ext.size() > 3 ||
	    ext.find('.') != std::string::npos) {
		return false;
	}
	for (const char c : base + ext) {
		if (c <= ' ' || std::strchr("\"*+,/:;<=>?[\\]|", c)) {
			return false;
		}
	}
	return true;
}

std::string short_name_of(localDrive& ld, const std::string& full, const std::string& name)
{
	char buf[DOS_NAMELENGTH_ASCII] = {};
	if (ld.dirCache.GetShortName(full.c_str(), buf)) {
		return buf;
	}
	if (is_83(name)) {
		std::string u = name;
		for (auto& c : u) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return u;
	}
	return {};
}

/** Name GEOS uses for a new file: its GEOS long name, else the DOS name. */
std::string geos_name_of(const std::string& full, const DosWatch::Seen& seen)
{
	if (!seen.is_dir && seen.size >= 256) {
		if (std::FILE* f = std::fopen(full.c_str(), "rb")) {
			uint8_t h[40] = {};
			const size_t got = std::fread(h, 1, sizeof(h), f);
			std::fclose(f);
			if (got == sizeof(h) && h[0] == ('G' | 0x80) && h[1] == 'E' &&
			    h[2] == ('A' | 0x80) && h[3] == 'S') {
				const char* ln = reinterpret_cast<const char*>(h + 4);
				const std::string n(ln, strnlen(ln, 32));
				if (!n.empty()) {
					return n;
				}
			}
		}
	}
	return seen.short_name;
}

bool dos_file_open(const uint8_t drive, const std::string& rel)
{
	for (const auto& f : Files) {
		if (f && f->GetDrive() == drive && strcasecmp(f->GetName(), rel.c_str()) == 0) {
			return true;
		}
	}
	return false;
}

/**
 * Compare a watched directory with the host. With @p report, differences
 * are applied to DOSBox's directory cache and queued as notifications;
 * without, the current state is just taken as known.
 */
void scan_watch(DosWatch& w, const bool report)
{
	const auto ld = local_drive(w.drive);
	if (!ld) {
		return;
	}
	std::error_code ec;
	stdfs::directory_iterator it(w.host_dir, ec);
	if (ec) {
		return;
	}
	const uint32_t did = dir_id(w.dos_dir);
	std::string sep_dir = w.host_dir;
	if (!sep_dir.empty() && sep_dir.back() != CROSS_FILESPLIT) {
		sep_dir.push_back(CROSS_FILESPLIT);
	}
	const std::string& cache_dir = w.cache_dir;
	std::unordered_map<std::string, DosWatch::Seen> now;
	for (const auto& e : it) {
		const std::string name = e.path().filename().string();
		if (name.empty() || name == "." || name == "..") {
			continue;
		}
		DosWatch::Seen seen;
		std::error_code eec;
		seen.is_dir = e.is_directory(eec);
		if (!seen.is_dir) {
			seen.size = e.file_size(eec);
		}
		seen.mtime = e.last_write_time(eec);
		const std::string full  = sep_dir + name;   // host file
		const std::string cpath = cache_dir + name; // same, for the cache

		const auto prev = w.entries.find(name);
		if (prev == w.entries.end()) {
			if (report) {
				// Make it visible to DOS. AddEntry only creates file
				// entries; for a directory do what DOSBox's own MKDIR
				// does and drop the parent's cache.
				if (seen.is_dir) {
					ld->dirCache.CacheOut(cpath.c_str(), true);
				} else {
					ld->dirCache.AddEntry(cpath.c_str(), true);
				}
			}
			seen.short_name = short_name_of(*ld, cpath, name);
			if (report && !seen.short_name.empty()) {
				fs_events.push_back({FCNT_CREATE, w.drive, did, geos_name_of(full, seen)});
			}
		} else {
			seen.short_name = prev->second.short_name;
			const bool changed = prev->second.size != seen.size ||
			                     prev->second.mtime != seen.mtime;
			const std::string rel = w.dos_dir.empty() ? seen.short_name
			                                          : w.dos_dir + "\\" + seen.short_name;
			if (report && changed && !seen.is_dir && !seen.short_name.empty() &&
			    !dos_file_open(w.drive, rel)) {
				fs_events.push_back(
				        {FCNT_CONTENTS, w.drive, dos_id(did, seen.short_name), {}});
			}
		}
		now[name] = seen;
	}
	for (const auto& [name, seen] : w.entries) {
		if (now.count(name)) {
			continue;
		}
		if (report) {
			if (seen.is_dir) {
				ld->dirCache.CacheOut((cache_dir + name).c_str(), true);
			} else {
				ld->dirCache.DeleteEntry((cache_dir + name).c_str());
			}
			if (!seen.short_name.empty()) {
				fs_events.push_back(
				        {FCNT_DELETE, w.drive, dos_id(did, seen.short_name), {}});
			}
		}
	}
	w.entries = std::move(now);
}

/** Directory of a DOS name (or the name itself if @p is_dir), canonical. */
bool dos_dir_of(const char* name, const bool is_dir, uint8_t& drive, std::string& dir)
{
	char full[DOS_PATHLENGTH] = {};
	uint8_t d = 0;
	if (!DOS_MakeName(name, full, &d)) {
		return false;
	}
	std::string f(full);
	if (!is_dir) {
		const auto p = f.rfind('\\');
		f = p == std::string::npos ? "" : f.substr(0, p);
	}
	drive = d;
	dir   = f;
	return true;
}

/** GEOS uses this directory: watch it (most recently used MaxDosWatches). */
void watch_dos(const char* name, const bool is_dir)
{
	if (!fs_notify_files) {
		return;
	}
	uint8_t drive = 0;
	std::string dir;
	if (!dos_dir_of(name, is_dir, drive, dir)) {
		return;
	}
	const auto ld = local_drive(drive);
	if (!ld) {
		return; // images, CD-ROMs, Z: don't change behind our back
	}
	const std::string key = std::string(1, static_cast<char>('A' + drive)) + ":" + dir;
	auto it = dos_watches.find(key);
	if (it != dos_watches.end()) {
		it->second.used = ++dos_watch_clock;
		return;
	}
	if (dos_watches.size() >= MaxDosWatches) {
		auto oldest = dos_watches.begin();
		for (auto w = dos_watches.begin(); w != dos_watches.end(); ++w) {
			if (w->second.used < oldest->second.used) {
				oldest = w;
			}
		}
		dos_watches.erase(oldest);
	}
	DosWatch w;
	w.drive    = drive;
	w.dos_dir  = dir;
	w.host_dir  = ld->MapDosToHostFilename(dir.c_str());
	w.cache_dir = ld->GetBasedir();
	if (!w.cache_dir.empty() && w.cache_dir.back() != CROSS_FILESPLIT) {
		w.cache_dir.push_back(CROSS_FILESPLIT);
	}
	for (const char c : dir) {
		w.cache_dir.push_back(c == '\\' ? CROSS_FILESPLIT : c);
	}
	if (!dir.empty()) {
		w.cache_dir.push_back(CROSS_FILESPLIT);
	}
	w.used = ++dos_watch_clock;
	scan_watch(w, false);
	dos_watches.emplace(key, std::move(w));
}

/** GEOS changed this directory itself: take its state as known. */
void resync_dos(const char* name, const bool is_dir)
{
	if (!fs_notify_files) {
		return;
	}
	uint8_t drive = 0;
	std::string dir;
	if (!dos_dir_of(name, is_dir, drive, dir)) {
		return;
	}
	const auto it = dos_watches.find(std::string(1, static_cast<char>('A' + drive)) + ":" + dir);
	if (it != dos_watches.end()) {
		scan_watch(it->second, false);
	}
}

} // namespace

/* ----------------------------------------------------------------
 * INT 21h function executor
 *
 * Semantics mirror DOS_21Handler in src/dos/dos.cpp for each function,
 * minus the emulated disk I/O delays and cycle accounting.
 * ---------------------------------------------------------------- */

static Outcome execute_int21(Regs& r)
{
	char name1[MaxPath];
	char name2[MaxPath];

	switch (r.ah()) {
	case 0x0e: // Select default drive
		DOS_SetDefaultDrive(r.dl());
		r.set_al(DOS_DRIVES);
		return Outcome::Handled;

	case 0x19: // Get default drive
		r.set_al(DOS_GetDefaultDrive());
		return Outcome::Handled;

	case 0x1a: // Set DTA: observe only, real DOS must see it too
		fast_dta_seg   = r.ds;
		fast_dta_off   = r.dx;
		fast_dta_valid = true;
		return Outcome::NotHandled;

	case 0x36: { // Get free disk space (no carry convention)
		uint16_t bytes = 0, clusters = 0, free = 0;
		uint8_t sectors = 0;
		if (DOS_GetFreeDiskSpace(r.dl(), &bytes, &sectors, &clusters, &free)) {
			r.ax = sectors;
			r.bx = free;
			r.cx = bytes;
			r.dx = clusters;
		} else {
			r.ax = 0xffff;
		}
		return Outcome::Handled;
	}

	case 0x39: // MKDIR
	case 0x3a: // RMDIR
	case 0x3b: // CHDIR
	case 0x41: // UNLINK
		if (!read_guest_string(r.ds, r.dx, name1)) {
			return Outcome::NotHandled;
		}
		switch (r.ah()) {
		case 0x39:
			r.result(DOS_MakeDir(name1));
			if (!r.cf) r.ax = 0x05;
			break;
		case 0x3a:
			r.result(DOS_RemoveDir(name1));
			if (!r.cf) r.ax = 0x05;
			break;
		case 0x3b:
			r.result(DOS_ChangeDir(name1));
			if (!r.cf) r.ax = 0x00;
			break;
		default: r.result(DOS_UnlinkFile(name1)); break;
		}
		if (!r.cf) {
			if (r.ah() == 0x3b) {
				watch_dos(name1, true);
			} else {
				resync_dos(name1, false);
			}
		}
		return Outcome::Handled;

	case 0x3c: // CREATE
	case 0x5b: // CREATE NEW
		if (!read_guest_string(r.ds, r.dx, name1)) {
			return Outcome::NotHandled;
		}
		if (r.ah() == 0x5b && DOS_FileExists(name1)) {
			DOS_SetError(DOSERR_FILE_ALREADY_EXISTS);
			r.fail();
		} else {
			uint16_t handle = 0;
			r.result(DOS_CreateFile(name1, FatAttributeFlags(r.cl()), &handle));
			if (!r.cf) {
				r.ax = handle;
				watch_dos(name1, false);
				resync_dos(name1, false);
			}
		}
		return Outcome::Handled;

	case 0x3d: { // OPEN
		if (!read_guest_string(r.ds, r.dx, name1)) {
			return Outcome::NotHandled;
		}
		uint16_t handle = 0;
		r.result(DOS_OpenFile(name1, r.al(), &handle));
		if (!r.cf) {
			r.ax = handle;
			watch_dos(name1, false);
		}
		return Outcome::Handled;
	}

	case 0x3e: { // CLOSE
		uint8_t refcnt = 0;
		r.result(DOS_CloseFile(r.bx, false, &refcnt));
		if (!r.cf) r.set_al(refcnt);
		return Outcome::Handled;
	}

	case 0x3f: { // READ
		if (!is_disk_file_handle(r.bx)) {
			return Outcome::NotHandled; // devices (CON!) via real DOS
		}
		PhysPt dest = 0;
		if (!resolve_len(r.ds, r.dx, r.cx, Access::Write, dest)) {
			return Outcome::NotHandled;
		}
		uint16_t amount = r.cx;
		if (DOS_ReadFile(r.bx, io_buffer.data(), &amount)) {
			MEM_BlockWrite(dest, io_buffer.data(), amount);
			r.ax = amount;
			r.ok();
		} else {
			r.fail();
		}
		return Outcome::Handled;
	}

	case 0x40: { // WRITE (cx = 0 truncates)
		if (!is_disk_file_handle(r.bx)) {
			return Outcome::NotHandled;
		}
		PhysPt src = 0;
		if (r.cx && !resolve_len(r.ds, r.dx, r.cx, Access::Read, src)) {
			return Outcome::NotHandled;
		}
		uint16_t amount = r.cx;
		if (amount) {
			MEM_BlockRead(src, io_buffer.data(), amount);
		}
		if (DOS_WriteFile(r.bx, io_buffer.data(), &amount)) {
			r.ax = amount;
			r.ok();
		} else {
			r.fail();
		}
		return Outcome::Handled;
	}

	case 0x42: { // LSEEK
		uint32_t pos = (static_cast<uint32_t>(r.cx) << 16) | r.dx;
		if (DOS_SeekFile(r.bx, &pos, r.al())) {
			r.dx = static_cast<uint16_t>(pos >> 16);
			r.ax = static_cast<uint16_t>(pos & 0xffff);
			r.ok();
		} else {
			r.fail();
		}
		return Outcome::Handled;
	}

	case 0x43: { // Get/Set attributes
		if (r.al() > 1 || !read_guest_string(r.ds, r.dx, name1)) {
			return Outcome::NotHandled;
		}
		if (r.al() == 0) {
			FatAttributeFlags attr = r.cl();
			if (DOS_GetFileAttr(name1, &attr)) {
				r.cx = attr._data;
				r.ax = attr._data;
				r.ok();
			} else {
				r.fail();
			}
		} else {
			r.result(DOS_SetFileAttr(name1, FatAttributeFlags(r.cl())));
			if (!r.cf) r.ax = 0x202;
		}
		return Outcome::Handled;
	}

	case 0x44: { // IOCTL: only 00h (get device information)
		if (r.al() != 0x00) {
			return Outcome::NotHandled;
		}
		const uint8_t handle = RealHandle(r.bx);
		if (handle >= DOS_FILES || !Files[handle]) {
			DOS_SetError(DOSERR_INVALID_HANDLE);
			r.fail();
			return Outcome::Handled;
		}
		const uint16_t info = Files[handle]->GetInformation();
		if (info & 0x8000) {
			r.dx = info & ~EXT_DEVICE_BIT;
		} else {
			uint8_t hdrive = Files[handle]->GetDrive();
			if (hdrive == 0xff) {
				hdrive = 2;
			}
			r.dx = (info & 0xffe0) | hdrive;
		}
		r.ax = r.dx;
		r.ok();
		return Outcome::Handled;
	}

	case 0x45: { // DUP
		uint16_t newhandle = 0;
		r.result(DOS_DuplicateEntry(r.bx, &newhandle));
		if (!r.cf) r.ax = newhandle;
		return Outcome::Handled;
	}

	case 0x47: { // Get current directory into ds:si (64 bytes)
		if (!DOS_GetCurrentDir(r.dl(), name1)) {
			r.fail();
			return Outcome::Handled;
		}
		const auto len = static_cast<uint32_t>(strnlen(name1, MaxPath - 1) + 1);
		PhysPt dest = 0;
		if (!resolve_len(r.ds, r.si, len, Access::Write, dest)) {
			return Outcome::NotHandled;
		}
		MEM_BlockWrite(dest, name1, len);
		r.ax = 0x0100;
		r.ok();
		return Outcome::Handled;
	}

	case 0x4e: return do_find(r, true);
	case 0x4f: return do_find(r, false);

	case 0x56: // RENAME ds:dx -> es:di
		if (!read_guest_string(r.ds, r.dx, name1) ||
		    !read_guest_string(r.es, r.di, name2)) {
			return Outcome::NotHandled;
		}
		r.result(DOS_Rename(name1, name2));
		if (!r.cf) {
			resync_dos(name1, false);
			resync_dos(name2, false);
		}
		return Outcome::Handled;

	case 0x57: // Get/Set file date and time
		if (r.al() == 0x00) {
			r.result(DOS_GetFileDate(r.bx, &r.cx, &r.dx));
		} else if (r.al() == 0x01) {
			r.result(DOS_SetFileDate(r.bx, r.cx, r.dx));
		} else {
			return Outcome::NotHandled;
		}
		return Outcome::Handled;

	case 0x5c: { // Lock/unlock region
		const uint32_t pos = (static_cast<uint32_t>(r.cx) << 16) | r.dx;
		const uint32_t len = (static_cast<uint32_t>(r.si) << 16) | r.di;
		bool success       = false;
		if (r.al() == 0) {
			success = DOS_LockFile(r.bx, pos, len);
		} else if (r.al() == 1) {
			success = DOS_UnlockFile(r.bx, pos, len);
		} else {
			return Outcome::NotHandled;
		}
		r.result(success);
		if (!r.cf) r.ax = 0;
		return Outcome::Handled;
	}

	case 0x68: // Commit file
		r.result(DOS_FlushFile(r.bx));
		if (!r.cf) r.ax = static_cast<uint16_t>((0x68 << 8) | r.al());
		return Outcome::Handled;

	case 0x6c: { // Extended open/create, name at ds:si
		if (!read_guest_string(r.ds, r.si, name1)) {
			return Outcome::NotHandled;
		}
		uint16_t handle = 0, status = 0;
		r.result(DOS_OpenFileExtended(name1, r.bx, FatAttributeFlags(r.cl()),
		                              r.dx, &handle, &status));
		if (!r.cf) {
			r.ax = handle;
			r.cx = status;
			watch_dos(name1, false);
			if (status != 1) { // created or replaced
				resync_dos(name1, false);
			}
		}
		return Outcome::Handled;
	}

	default: return Outcome::NotHandled;
	}
}

/** AH values execute_int21() may handle (reported to the guest). */
static constexpr uint8_t HandledFunctions[] = {
        0x0e, 0x19, 0x1a, 0x36, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x4e, 0x4f, 0x56, 0x57,
        0x5b, 0x5c, 0x68, 0x6c};

/* ----------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------- */

static bool fs_allowed()
{
	if (!fs_env_checked) {
		fs_env_checked   = true;
		const char* env  = std::getenv("BASEBOX_GEOS_FSAPI");
		fs_disabled_by_env = env && (env[0] == '0' || env[0] == 'n' ||
		                             env[0] == 'N' || env[0] == 'f' ||
		                             env[0] == 'F');
		if (fs_disabled_by_env) {
			LOG_INFO("GEOSHOST: FS fast path disabled by BASEBOX_GEOS_FSAPI");
		}
	}
	return !fs_disabled_by_env;
}

static void cmd_enable(const uint16_t cmd[6], uint16_t resp[6])
{
	const uint16_t psp      = cmd[1];
	const uint16_t map_seg  = cmd[2];
	const uint16_t map_off  = cmd[3];
	const uint16_t map_size = cmd[4];
	const uint16_t options  = cmd[5];

	PhysPt map = 0;
	if (!fs_allowed() || map_size < FuncMapBytes ||
	    !resolve_len(map_seg, map_off, FuncMapBytes, Access::Write, map)) {
		resp[0] = HIF_FAILED;
		return;
	}

	// The guest passes its PSP as it sees it. Under DPMI that is a
	// selector (INT 21h/62h is translated by the DPMI host), so convert
	// it back to the real-mode segment DOS itself uses in dos.psp().
	uint16_t psp_seg = psp;
	if (selector_mode()) {
		GuestSpan span;
		if (!resolve(psp, 0, Access::Read, span) || (span.base & 0xf) ||
		    span.base >= 0x100000) {
			LOG_WARNING("GEOSHOST: FS enable: PSP selector %04x not mappable", psp);
			resp[0] = HIF_FAILED;
			return;
		}
		psp_seg = static_cast<uint16_t>(span.base >> 4);
	}
	if (psp_seg != dos.psp()) {
		LOG_WARNING("GEOSHOST: FS enable: PSP %04x is not the current DOS PSP %04x",
		            psp_seg,
		            dos.psp());
		resp[0] = HIF_FAILED;
		return;
	}

	uint8_t bits[FuncMapBytes] = {};
	for (const auto fn : HandledFunctions) {
		bits[fn >> 3] |= static_cast<uint8_t>(1u << (fn & 7));
	}
	MEM_BlockWrite(map, bits, FuncMapBytes);

	fs_enabled       = true;
	fs_guest_psp     = psp_seg;
	fs_notify_drives = (options & HIF_FS_OPT_DRIVE_NOTIFY) != 0;
	fs_notify_files  = (options & HIF_FS_OPT_FILE_NOTIFY) != 0;
	dos_watches.clear();
	fs_events.clear();
	fast_dta_valid   = false;

	resp[0] = HIF_OK;
	resp[1] = HIF_FS_MAGIC;
	resp[2] = HIF_API_FS_VERSION;
	resp[3] = drive_generation;
	LOG_INFO("GEOSHOST: FS fast path enabled (psp=%04x, %s mode)",
	         psp_seg,
	         selector_mode() ? "protected" : "real");
}

static void cmd_int21(const uint16_t cmd[6], uint16_t resp[6])
{
	// ax only carries the HostIf status; the outcome is in bx (see
	// geoshost_fs.h)
	resp[0] = HIF_OK;
	resp[1] = HIF_FS_MAGIC;
	resp[2] = HIF_FS_NOT_EXECUTED;

	// Only serve the process that enabled us; anything else (e.g. a DOS
	// program started from GEOS that somehow reaches this port) takes the
	// regular path.
	if (!fs_enabled || dos.psp() != fs_guest_psp) {
		return;
	}
	PhysPt frame = 0;
	if (cmd[4] < HOST_FS_FRAME_SIZE ||
	    !resolve_len(cmd[2], cmd[3], HOST_FS_FRAME_SIZE, Access::Write, frame)) {
		return;
	}

	uint16_t w[FrameWords];
	for (uint16_t i = 0; i < FrameWords; ++i) {
		w[i] = mem_readw(frame + 2 * i);
	}
	Regs r;
	r.ax = w[F_AX];
	r.bx = w[F_BX];
	r.cx = w[F_CX];
	r.dx = w[F_DX];
	r.si = w[F_SI];
	r.di = w[F_DI];
	r.ds = w[F_DS];
	r.es = w[F_ES];

	const uint8_t fn = r.ah();
	if (fn != 0x1a && fn != 0x4e && fn != 0x4f) {
		fast_dta_valid = false;
	}

	if (execute_int21(r) == Outcome::NotHandled) {
		return; // frame untouched
	}

	w[F_AX]     = r.ax;
	w[F_BX]     = r.bx;
	w[F_CX]     = r.cx;
	w[F_DX]     = r.dx;
	w[F_SI]     = r.si;
	w[F_DI]     = r.di;
	w[F_FLAGS]  = r.cf ? 1 : 0;
	w[F_EXTERR] = r.cf ? dos.errorcode : 0;
	for (uint16_t i = 0; i < FrameWords; ++i) {
		mem_writew(frame + 2 * i, w[i]);
	}
	resp[2] = HIF_FS_EXECUTED;
}

static void cmd_drive_info(const uint16_t cmd[6], uint16_t resp[6])
{
	const uint16_t drive = cmd[1];
	if (drive >= DOS_DRIVES || !Drives[drive]) {
		resp[0] = HIF_NOT_FOUND;
		return;
	}
	auto& d = *Drives[drive];

	uint16_t flags = HFDF_PRESENT;
	if (d.IsReadOnly()) flags |= HFDF_READ_ONLY;
	if (d.IsRemovable()) flags |= HFDF_REMOVABLE;
	if (d.IsRemote()) flags |= HFDF_REMOTE;
	if (drive == DOS_GetDefaultDrive()) flags |= HFDF_DEFAULT;

	uint16_t bytes_sector    = 512;
	uint8_t sectors_cluster  = 0;
	uint16_t total_clusters  = 0;
	uint16_t free_clusters   = 0;
	if (d.AllocationInfo(&bytes_sector, &sectors_cluster, &total_clusters,
	                     &free_clusters) == false ||
	    bytes_sector == 0) {
		bytes_sector = 512;
	}

	// Two directory mounts of the same host path are the same disk.
	uint16_t alias = 0xffff;
	if (d.GetType() == DosDriveType::Local || d.GetType() == DosDriveType::Cdrom) {
		const std::string mine = d.GetInfo();
		for (uint16_t i = 0; i < drive; ++i) {
			if (Drives[i] && Drives[i]->GetType() == d.GetType() &&
			    mine == Drives[i]->GetInfo()) {
				alias = i;
				break;
			}
		}
	}

	resp[0] = HIF_OK;
	resp[1] = flags;
	resp[2] = static_cast<uint16_t>(d.GetType());
	resp[3] = bytes_sector;
	resp[4] = alias;
	resp[5] = d.GetMediaByte();
}

static void cmd_poll_changes(const uint16_t cmd[6], uint16_t resp[6])
{
	if (fs_events.empty()) {
		resp[0] = HIF_NOT_FOUND;
		return;
	}
	const FsEvent e = fs_events.front();
	fs_events.pop_front();
	const uint16_t size = cmd[4];
	PhysPt name_buf = 0;
	if (size && resolve_len(cmd[2], cmd[3], size, Access::Write, name_buf)) {
		const auto n = std::min<size_t>(e.name.size(), size - 1u);
		MEM_BlockWrite(name_buf, e.name.data(), n);
		mem_writeb(name_buf + n, 0);
	}
	resp[0] = HIF_OK;
	resp[1] = e.type;
	resp[2] = e.drive;
	resp[3] = static_cast<uint16_t>(e.id >> 16);
	resp[4] = static_cast<uint16_t>(e.id);
}

bool GEOSHOST_FS_Tick()
{
	static unsigned ticks = 0;
	if (!fs_enabled || !fs_notify_files || dos_watches.empty() || ++ticks < 1000) {
		return false;
	}
	ticks = 0;
	const size_t queued_before = fs_events.size();
	std::vector<std::string> gone;
	for (auto& [key, w] : dos_watches) {
		std::error_code ec;
		if (!std::filesystem::is_directory(w.host_dir, ec)) {
			gone.push_back(key); // the parent's watch reports the deletion
			continue;
		}
		scan_watch(w, true);
	}
	for (const auto& key : gone) {
		dos_watches.erase(key);
	}
	// Report every scan that queued something: a single "became pending"
	// event could be missed (e.g. before the driver registered its
	// handler), and the queue would then never be announced again.
	const bool queued_new = fs_events.size() > queued_before;
	return queued_new;
}

void GEOSHOST_FS_HandleCommand(const uint16_t cmd[6], uint16_t resp[6])
{
	for (int i = 0; i < 6; ++i) {
		resp[i] = 0;
	}
	switch (cmd[0]) {
	case HIF_FS_ENABLE: cmd_enable(cmd, resp); break;
	case HIF_FS_INT21: cmd_int21(cmd, resp); break;
	case HIF_FS_GET_DRIVE_INFO: cmd_drive_info(cmd, resp); break;
	case HIF_FS_POLL_CHANGES: cmd_poll_changes(cmd, resp); break;
	case HIF_FS_GET_DRIVE_GENERATION:
		resp[0] = HIF_OK;
		resp[1] = drive_generation;
		break;
	default: resp[0] = HIF_FAILED; break;
	}
}

bool GEOSHOST_FS_PollDriveChange(uint16_t& generation)
{
	bool changed = false;
	for (size_t i = 0; i < DOS_DRIVES; ++i) {
		const DOS_Drive* d = Drives[i].get();
		const auto type    = d ? d->GetType() : DosDriveType::Unknown;
		auto& snap         = drive_snapshot[i];
		// The info string catches unmount + remount of a different path
		// that happens to reuse the same allocation.
		if (snap.drive != d || snap.type != type ||
		    (d && snap.info != d->GetInfo())) {
			snap.drive = d;
			snap.type  = type;
			snap.info  = d ? d->GetInfo() : "";
			changed    = true;
		}
	}
	if (changed) {
		if (++drive_generation == 0) {
			drive_generation = 1; // 0 is reserved for "unknown"
		}
	}
	generation = drive_generation;
	// Only report changes to a guest that asked for notifications: the
	// current HostIf library broadcasts unknown notification types as
	// socket state changes.
	return changed && fs_enabled && fs_notify_drives;
}

#endif // C_GEOSHOST
