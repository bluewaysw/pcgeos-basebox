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
#include "hostfs_lfn.h"
#include "hostfs_volume.h"

#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
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
static bool fs_lfn           = false; // long file names granted (v2)

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
		std::string long_name  = {}; // GEOS name, in LFN mode
		uintmax_t size         = 0;
		stdfs::file_time_type mtime = {};
		bool is_dir            = false;
	};
	uint8_t drive         = 0;
	std::string dos_dir   = {}; // "DIR\SUB", no drive, no leading '\'
	std::string long_dir  = {}; // the same in GEOS names, in LFN mode
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

/**
 * Name GEOS uses for a new file: its GEOS long name, else the DOS name
 * (in LFN mode the name the LFN calls show).
 */
std::string geos_name_of(const std::string& full, const DosWatch::Seen& seen)
{
	const std::string& fallback = fs_lfn && !seen.long_name.empty() ? seen.long_name
	                                                                 : seen.short_name;
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
	return fallback;
}

/** Name the change IDs are computed over: long in LFN mode, else short. */
const std::string& id_name_of(const DosWatch::Seen& seen)
{
	return fs_lfn && !seen.long_name.empty() ? seen.long_name : seen.short_name;
}

// Long file name helpers, defined with the LFN calls below
std::string lfn_long_dir(uint8_t drive, const std::string& short_dir);
std::unordered_map<std::string, std::string> lfn_long_names(localDrive& ld,
                                                            const std::string& host_dir,
                                                            const std::string& cache_dir);

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
	const uint32_t did = dir_id(fs_lfn ? w.long_dir : w.dos_dir);
	const auto long_names = fs_lfn ? lfn_long_names(*ld, w.host_dir, w.cache_dir)
	                              : std::unordered_map<std::string, std::string>{};
	std::string sep_dir = w.host_dir;
	if (!sep_dir.empty() && sep_dir.back() != CROSS_FILESPLIT) {
		sep_dir.push_back(CROSS_FILESPLIT);
	}
	const std::string& cache_dir = w.cache_dir;
	std::unordered_map<std::string, DosWatch::Seen> now;
	// Neither the name conversion nor stepping through the directory may
	// throw: this runs from the tick handler. On Windows the name is the
	// ANSI form DOSBox's DOS core uses; names it can't represent (and so
	// DOS can't reach) are skipped, as in the LFN listing.
	for (const stdfs::directory_iterator end; !ec && it != end; it.increment(ec)) {
		const auto& e          = *it;
		const std::string name = HostLfn::native_name(e.path().filename());
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
			if (const auto ln = long_names.find(name); ln != long_names.end()) {
				seen.long_name = ln->second;
			}
			if (report && !seen.short_name.empty()) {
				fs_events.push_back({FCNT_CREATE, w.drive, did, geos_name_of(full, seen)});
			}
		} else {
			seen.short_name = prev->second.short_name;
			seen.long_name  = prev->second.long_name;
			const bool changed = prev->second.size != seen.size ||
			                     prev->second.mtime != seen.mtime;
			const std::string rel = w.dos_dir.empty() ? seen.short_name
			                                          : w.dos_dir + "\\" + seen.short_name;
			if (report && changed && !seen.is_dir && !seen.short_name.empty() &&
			    !dos_file_open(w.drive, rel)) {
				fs_events.push_back(
				        {FCNT_CONTENTS, w.drive, dos_id(did, id_name_of(seen)), {}});
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
				        {FCNT_DELETE, w.drive, dos_id(did, id_name_of(seen)), {}});
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
	if (fs_lfn) {
		w.long_dir = lfn_long_dir(drive, dir);
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
 * Long file names (INT 21h AX=71xxh), see geoshost_fs.h
 *
 * Paths are resolved component by component against the host directory
 * (HostLfn::list_dir: GEOS names, full host names and DOS short names all
 * match) and turned into DOS short paths, so the actual work is done by
 * the DOS core as for every other call: handles, the current directory
 * and the directory cache stay DOSBox's. Only creating a long name is
 * done on the host directly, then the new entry is added to the cache.
 *
 * Drives that aren't directory mounts (images, CD-ROM images, Z:) get
 * the calls with plain 8.3 semantics through the DOS core.
 * ---------------------------------------------------------------- */

namespace {

constexpr uint16_t LfnNotLocal = 0xffff; // lfn_resolve: use the 8.3 path

struct LfnSearch {
	std::vector<HostLfn::Entry> entries = {};
	size_t next                         = 0;
	uint64_t used                       = 0;
};
std::map<uint16_t, LfnSearch> lfn_searches;
uint16_t lfn_next_handle = 1;
uint64_t lfn_clock       = 0;

void lfn_reset()
{
	lfn_searches.clear();
	lfn_next_handle = 1;
}

bool read_guest_lfn(const uint16_t seg, const uint16_t off, std::string& out)
{
	GuestSpan span;
	if (!resolve(seg, off, Access::Read, span)) {
		return false;
	}
	const uint32_t max = std::min<uint32_t>(span.avail, HostLfn::MaxPath);
	out.clear();
	for (uint32_t i = 0; i < max; ++i) {
		const auto c = static_cast<char>(mem_readb(span.base + i));
		if (c == '\0') {
			return true;
		}
		out.push_back(c);
	}
	return false;
}

bool write_guest(const uint16_t seg, const uint16_t off, const void* data, const uint32_t len)
{
	PhysPt dest = 0;
	if (!resolve_len(seg, off, len, Access::Write, dest)) {
		return false;
	}
	MEM_BlockWrite(dest, data, len);
	return true;
}

bool write_guest_string(const uint16_t seg, const uint16_t off, const std::string& s)
{
	if (s.size() >= HostLfn::MaxPath) {
		return false;
	}
	return write_guest(seg, off, s.c_str(), static_cast<uint32_t>(s.size() + 1));
}

Outcome lfn_fail(Regs& r, const uint16_t err)
{
	DOS_SetError(err);
	r.fail();
	return Outcome::Handled;
}

/** A directory on a directory mount, in all its name forms. */
struct LfnDir {
	uint8_t drive = 0;
	std::shared_ptr<localDrive> ld = {};
	std::vector<std::string> short_comps  = {};
	std::vector<std::string> long_comps   = {};
	std::vector<std::string> native_comps = {};
	stdfs::path host  = {}; // host directory
	std::string cache = {}; // basedir + DOS short names + separator
	std::vector<HostLfn::Entry> entries = {};
};

std::string join_comps(const std::vector<std::string>& comps)
{
	std::string s;
	for (const auto& c : comps) {
		if (!s.empty()) {
			s.push_back('\\');
		}
		s += c;
	}
	return s;
}

/** "C:\DIR\SUB\name" in DOS short names. */
std::string lfn_short_path(const LfnDir& d, const std::string& name = {})
{
	std::string s = std::string(1, static_cast<char>('A' + d.drive)) + ":\\";
	s += join_comps(d.short_comps);
	if (!name.empty()) {
		if (!d.short_comps.empty()) {
			s.push_back('\\');
		}
		s += name;
	}
	return s;
}

void lfn_rebuild(LfnDir& d)
{
	const std::string base = d.ld->GetBasedir();
	d.host  = HostLfn::from_native(base);
	d.cache = base;
	if (!d.cache.empty() && d.cache.back() != CROSS_FILESPLIT) {
		d.cache.push_back(CROSS_FILESPLIT);
	}
	for (size_t i = 0; i < d.native_comps.size(); ++i) {
		d.host /= HostLfn::from_native(d.native_comps[i]);
		d.cache += d.short_comps[i];
		d.cache.push_back(CROSS_FILESPLIT);
	}
	d.entries.clear();
}

bool lfn_root(const uint8_t drive, LfnDir& d)
{
	d       = {};
	d.drive = drive;
	d.ld    = local_drive(drive);
	if (!d.ld) {
		return false;
	}
	lfn_rebuild(d);
	return true;
}

HostLfn::ShortNameFn lfn_short_names(localDrive& ld, const std::string& cache)
{
	return [&ld, cache](const std::string& native, const bool is_dir) {
		const std::string cpath = cache + native;
		std::string s           = short_name_of(ld, cpath, native);
		if (s.empty()) {
			// Not in the directory cache yet (created on the host). As
			// in scan_watch: AddEntry only creates file entries.
			if (is_dir) {
				ld.dirCache.CacheOut(cpath.c_str(), true);
			} else {
				ld.dirCache.AddEntry(cpath.c_str(), true);
			}
			s = short_name_of(ld, cpath, native);
		}
		return s;
	};
}

void lfn_list(LfnDir& d)
{
	d.entries = HostLfn::list_dir(d.host, lfn_short_names(*d.ld, d.cache));
}

void lfn_descend(LfnDir& d, const HostLfn::Entry& e)
{
	d.short_comps.push_back(e.short_name);
	d.long_comps.push_back(e.geos_name);
	d.native_comps.push_back(e.native_name);
	lfn_rebuild(d);
}

bool lfn_ascend(LfnDir& d)
{
	if (d.short_comps.empty()) {
		return false;
	}
	d.short_comps.pop_back();
	d.long_comps.pop_back();
	d.native_comps.pop_back();
	lfn_rebuild(d);
	return true;
}

std::vector<std::string> split_short(const std::string& s)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start < s.size()) {
		const auto end = s.find('\\', start);
		const auto stop = end == std::string::npos ? s.size() : end;
		if (stop > start) {
			out.push_back(s.substr(start, stop - start));
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return out;
}

/** A path resolved to its directory plus its last component. */
struct LfnTarget {
	LfnDir dir           = {};
	std::string last     = {}; // as given (GEOS characters); empty: the directory
	bool exists          = false;
	HostLfn::Entry entry = {}; // if exists
};

/**
 * Resolve a GEOS path. Returns 0, a DOS error, or LfnNotLocal if the drive
 * is not a directory mount.
 */
uint16_t lfn_resolve(const std::string& path, LfnTarget& t)
{
	HostLfn::SplitPath sp;
	if (!HostLfn::split_path(path, sp)) {
		return DOSERR_PATH_NOT_FOUND;
	}
	const uint8_t drive = sp.drive >= 0 ? static_cast<uint8_t>(sp.drive)
	                                    : DOS_GetDefaultDrive();
	if (drive >= DOS_DRIVES || !Drives[drive]) {
		return DOSERR_INVALID_DRIVE;
	}
	if (!lfn_root(drive, t.dir)) {
		return LfnNotLocal;
	}
	std::vector<std::string> comps;
	if (!sp.absolute) {
		comps = split_short(Drives[drive]->curdir); // DOS keeps it short
	}
	comps.insert(comps.end(), sp.comps.begin(), sp.comps.end());

	std::string last;
	if (!comps.empty()) {
		last = comps.back();
		comps.pop_back();
	}
	for (const auto& c : comps) {
		if (c == ".") {
			continue;
		}
		if (c == "..") {
			if (!lfn_ascend(t.dir)) {
				return DOSERR_PATH_NOT_FOUND;
			}
			continue;
		}
		lfn_list(t.dir);
		const HostLfn::Entry* e = HostLfn::find_entry(t.dir.entries, c);
		if (!e || !e->is_dir) {
			return DOSERR_PATH_NOT_FOUND;
		}
		const HostLfn::Entry copy = *e;
		lfn_descend(t.dir, copy);
	}
	if (last == ".") {
		last.clear();
	} else if (last == "..") {
		if (!lfn_ascend(t.dir)) {
			return DOSERR_PATH_NOT_FOUND;
		}
		last.clear();
	}
	t.last = last;
	lfn_list(t.dir);
	if (!last.empty() && !HostLfn::has_wildcards(last)) {
		if (const HostLfn::Entry* e = HostLfn::find_entry(t.dir.entries, last)) {
			t.exists = true;
			t.entry  = *e;
		}
	}
	return 0;
}

/** DOS short path of an existing target (or of its directory). */
std::string lfn_target_short(const LfnTarget& t)
{
	return lfn_short_path(t.dir, t.exists ? t.entry.short_name : std::string());
}

std::string drive_prefix(const uint8_t drive)
{
	return std::string(1, static_cast<char>('A' + drive)) + ":\\";
}

/** Host path for a new GEOS name in t.dir, or empty if it can't be made. */
stdfs::path lfn_new_host_path(const LfnTarget& t, std::string& native)
{
	if (!HostLfn::valid_new_name(t.last) || t.dir.ld->IsReadOnly()) {
		return {};
	}
	const stdfs::path name = HostLfn::from_utf8(HostFs::encode_geos_name(t.last));
	native                 = HostLfn::native_name(name);
	if (native.empty()) {
		return {}; // DOSBox's DOS core couldn't reach it (Windows)
	}
	return t.dir.host / name;
}

/** Search results of a non-mount drive, through the DOS core (8.3 names). */
bool lfn_classic_list(const std::string& spec, const uint8_t allowed,
                      std::vector<HostLfn::Entry>& out)
{
	ensure_scratch_dta();
	const RealPt saved = dos.dta();
	dos.dta(scratch_dta);
	bool found = DOS_FindFirst(spec.c_str(), FatAttributeFlags(allowed));
	while (found) {
		DOS_DTA dta(dos.dta());
		DOS_DTA::Result res;
		dta.GetResult(res);
		HostLfn::Entry e;
		e.geos_name = e.short_name = e.native_name = res.name;
		e.is_dir = res.attr.directory;
		e.attrs  = res.attr._data;
		e.size   = res.size;
		e.date   = res.date;
		e.time   = res.time;
		out.push_back(std::move(e));
		found = DOS_FindNext();
	}
	dos.dta(saved);
	return !out.empty();
}

uint16_t lfn_new_handle()
{
	if (lfn_searches.size() >= HOST_FS_LFN_MAX_SEARCHES) {
		auto oldest = lfn_searches.begin();
		for (auto it = lfn_searches.begin(); it != lfn_searches.end(); ++it) {
			if (it->second.used < oldest->second.used) {
				oldest = it;
			}
		}
		lfn_searches.erase(oldest);
	}
	while (lfn_next_handle == 0 || lfn_searches.count(lfn_next_handle)) {
		++lfn_next_handle;
	}
	return lfn_next_handle++;
}

/** Hand out the next entry of a search into ES:DI. */
Outcome lfn_find_out(Regs& r, LfnSearch& s)
{
	if (s.next >= s.entries.size()) {
		return lfn_fail(r, DOSERR_NO_MORE_FILES);
	}
	uint8_t fd[HostLfn::FindDataSize];
	HostLfn::pack_find_data(s.entries[s.next], fd);
	if (!write_guest(r.es, r.di, fd, sizeof(fd))) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	++s.next;
	s.used = ++lfn_clock;
	r.cx   = 0; // no Unicode conversion problems
	r.ok();
	return Outcome::Handled;
}

Outcome lfn_find_first(Regs& r)
{
	std::string spec;
	if (r.si != 1) {
		return lfn_fail(r, DOSERR_FUNCTION_NUMBER_INVALID); // DOS times only
	}
	if (!read_guest_lfn(r.ds, r.dx, spec)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	const uint8_t allowed  = r.cl();
	const uint8_t required = static_cast<uint8_t>(r.cx >> 8);
	LfnSearch s;
	LfnTarget t;
	const uint16_t err = lfn_resolve(spec, t);
	if (err == LfnNotLocal) {
		std::vector<HostLfn::Entry> all;
		lfn_classic_list(spec, allowed, all);
		for (auto& e : all) {
			if (HostLfn::attrs_match(e.attrs, allowed, required)) {
				s.entries.push_back(std::move(e));
			}
		}
	} else if (err) {
		return lfn_fail(r, err);
	} else {
		const std::string pattern = t.last.empty() ? "*" : t.last;
		if (!t.dir.short_comps.empty()) {
			for (const char* dots : {".", ".."}) {
				HostLfn::Entry e;
				e.geos_name = e.short_name = e.native_name = dots;
				e.is_dir = true;
				e.attrs  = HostLfn::A_SUBDIR;
				if (HostLfn::match_pattern(pattern, dots) &&
				    HostLfn::attrs_match(e.attrs, allowed, required)) {
					s.entries.push_back(std::move(e));
				}
			}
		}
		for (const auto& e : t.dir.entries) {
			if (HostLfn::attrs_match(e.attrs, allowed, required) &&
			    HostLfn::entry_matches(e, pattern)) {
				s.entries.push_back(e);
			}
		}
		watch_dos(lfn_short_path(t.dir, "X").c_str(), false);
	}
	if (s.entries.empty()) {
		return lfn_fail(r, HostLfn::has_wildcards(spec) ? DOSERR_NO_MORE_FILES
		                                                : DOSERR_FILE_NOT_FOUND);
	}
	const uint16_t handle = lfn_new_handle();
	auto& stored          = lfn_searches[handle];
	stored                = std::move(s);
	const Outcome o       = lfn_find_out(r, stored);
	if (!r.cf) {
		r.ax = handle;
	} else {
		lfn_searches.erase(handle);
	}
	return o;
}

Outcome lfn_create_open(Regs& r)
{
	std::string name;
	if (!read_guest_lfn(r.ds, r.si, name)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	const uint16_t mode   = r.bx;
	const auto attrs      = FatAttributeFlags(r.cl());
	const uint16_t action = r.dx;
	uint16_t handle = 0, status = 0;
	LfnTarget t;
	const uint16_t err = lfn_resolve(name, t);
	if (err == LfnNotLocal) {
		r.result(DOS_OpenFileExtended(name.c_str(), mode, attrs, action, &handle, &status));
		if (!r.cf) {
			r.ax = handle;
			r.cx = status;
		}
		return Outcome::Handled;
	}
	if (err) {
		return lfn_fail(r, err);
	}
	if (t.last.empty() || (t.exists && t.entry.is_dir)) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	if (HostLfn::has_wildcards(t.last)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	std::string short_path;
	if (t.exists) {
		short_path = lfn_target_short(t);
		r.result(DOS_OpenFileExtended(short_path.c_str(), mode, attrs, action, &handle, &status));
	} else {
		if (!(action & 0x10)) {
			return lfn_fail(r, DOSERR_FILE_NOT_FOUND);
		}
		std::string native;
		const stdfs::path host = lfn_new_host_path(t, native);
		if (host.empty()) {
			return lfn_fail(r, HostLfn::valid_new_name(t.last) ? DOSERR_ACCESS_DENIED
			                                                   : DOSERR_PATH_NOT_FOUND);
		}
		{
			std::ofstream f(host, std::ios::binary);
			if (!f) {
				return lfn_fail(r, DOSERR_ACCESS_DENIED);
			}
		}
		const std::string cpath = t.dir.cache + native;
		t.dir.ld->dirCache.AddEntry(cpath.c_str(), true);
		const std::string alias = short_name_of(*t.dir.ld, cpath, native);
		std::error_code ec;
		if (alias.empty()) {
			stdfs::remove(host, ec);
			return lfn_fail(r, DOSERR_ACCESS_DENIED);
		}
		short_path = lfn_short_path(t.dir, alias);
		// replace + create: applies the attributes, and the file is new
		r.result(DOS_OpenFileExtended(short_path.c_str(), mode, attrs, 0x12, &handle, &status));
		if (r.cf) {
			t.dir.ld->dirCache.DeleteEntry(cpath.c_str());
			stdfs::remove(host, ec);
			return Outcome::Handled;
		}
		status = 2; // created
	}
	if (!r.cf) {
		r.ax = handle;
		r.cx = status;
		watch_dos(short_path.c_str(), false);
		if (status != 1) {
			resync_dos(short_path.c_str(), false);
		}
	}
	return Outcome::Handled;
}

Outcome lfn_rename(Regs& r)
{
	std::string from, to;
	if (!read_guest_lfn(r.ds, r.dx, from) || !read_guest_lfn(r.es, r.di, to)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	LfnTarget a, b;
	const uint16_t ea = lfn_resolve(from, a);
	const uint16_t eb = lfn_resolve(to, b);
	if (ea == LfnNotLocal || eb == LfnNotLocal) {
		if (ea != eb) {
			return lfn_fail(r, DOSERR_NOT_SAME_DEVICE);
		}
		r.result(DOS_Rename(from.c_str(), to.c_str()));
		return Outcome::Handled;
	}
	if (ea) {
		return lfn_fail(r, ea);
	}
	if (eb) {
		return lfn_fail(r, eb);
	}
	if (!a.exists) {
		return lfn_fail(r, DOSERR_FILE_NOT_FOUND);
	}
	if (a.dir.drive != b.dir.drive) {
		return lfn_fail(r, DOSERR_NOT_SAME_DEVICE);
	}
	const stdfs::path old_host = a.dir.host / HostLfn::from_native(a.entry.native_name);
	if (b.exists) {
		const bool same = b.dir.host == a.dir.host && b.entry.native_name == a.entry.native_name;
		if (!same) {
			return lfn_fail(r, DOSERR_ACCESS_DENIED);
		}
		if (b.last == a.entry.geos_name) {
			r.ok(); // nothing to do
			return Outcome::Handled;
		}
	}
	std::string native;
	const stdfs::path new_host = lfn_new_host_path(b, native);
	if (new_host.empty()) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	const std::string rel = join_comps(a.dir.short_comps) +
	                        (a.dir.short_comps.empty() ? "" : "\\") + a.entry.short_name;
	if (!a.entry.is_dir && dos_file_open(a.dir.drive, rel)) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	std::error_code ec;
	stdfs::rename(old_host, new_host, ec);
	if (ec) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	auto& cache = a.dir.ld->dirCache;
	const std::string old_cpath = a.dir.cache + a.entry.native_name;
	const std::string new_cpath = b.dir.cache + native;
	if (a.entry.is_dir) {
		cache.CacheOut(old_cpath.c_str(), true);
		cache.CacheOut(new_cpath.c_str(), true);
	} else {
		cache.DeleteEntry(old_cpath.c_str());
		cache.AddEntry(new_cpath.c_str(), true);
	}
	resync_dos(lfn_short_path(a.dir, "X").c_str(), false);
	resync_dos(lfn_short_path(b.dir, "X").c_str(), false);
	r.ok();
	return Outcome::Handled;
}

Outcome lfn_full_path(Regs& r)
{
	std::string src;
	const uint8_t how = r.cl();
	if (how > 2) {
		return lfn_fail(r, DOSERR_FUNCTION_NUMBER_INVALID);
	}
	if (!read_guest_lfn(r.ds, r.si, src)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	LfnTarget t;
	const uint16_t err = lfn_resolve(src, t);
	std::string out;
	if (err == LfnNotLocal) {
		char buf[DOS_PATHLENGTH] = {};
		if (!DOS_Canonicalize(src.c_str(), buf)) {
			r.fail();
			return Outcome::Handled;
		}
		out = buf;
	} else if (err) {
		return lfn_fail(r, err);
	} else {
		const bool need = how != 0 && !t.last.empty();
		if (need && !t.exists) {
			return lfn_fail(r, DOSERR_FILE_NOT_FOUND);
		}
		std::vector<std::string> comps = how == 1 ? t.dir.short_comps : t.dir.long_comps;
		if (!t.last.empty()) {
			comps.push_back(!t.exists ? t.last
			                          : (how == 1 ? t.entry.short_name : t.entry.geos_name));
		}
		out = drive_prefix(t.dir.drive) + join_comps(comps);
	}
	if (!write_guest_string(r.es, r.di, out)) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	r.ok();
	return Outcome::Handled;
}

Outcome lfn_current_dir(Regs& r)
{
	char buf[DOS_PATHLENGTH] = {};
	if (!DOS_GetCurrentDir(r.dl(), buf)) {
		r.fail();
		return Outcome::Handled;
	}
	const uint8_t drive = r.dl() ? static_cast<uint8_t>(r.dl() - 1) : DOS_GetDefaultDrive();
	std::string out     = buf;
	LfnDir d;
	if (lfn_root(drive, d)) {
		out = lfn_long_dir(drive, buf);
	}
	if (!write_guest_string(r.ds, r.si, out)) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	r.ax = 0x0100;
	r.ok();
	return Outcome::Handled;
}

Outcome lfn_volume_info(Regs& r)
{
	std::string root;
	if (!read_guest_lfn(r.ds, r.dx, root)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	HostLfn::SplitPath sp;
	if (!HostLfn::split_path(root, sp) || sp.drive < 0 || !local_drive(sp.drive)) {
		return lfn_fail(r, DOSERR_INVALID_DRIVE);
	}
	static const char fs_name[] = "BASEBOX";
	if (r.cx >= sizeof(fs_name) && !write_guest(r.es, r.di, fs_name, sizeof(fs_name))) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	r.bx = HOST_FS_LFN_VOLUME_FLAGS;
	r.cx = HOST_FS_LFN_MAX_NAME;
	r.dx = HOST_FS_LFN_MAX_PATH;
	r.ok();
	return Outcome::Handled;
}

/** Path calls that act on one existing or new name: 39, 3A, 3B, 41, 43. */
Outcome lfn_path_call(Regs& r)
{
	std::string path;
	if (!read_guest_lfn(r.ds, r.dx, path)) {
		return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
	}
	const uint8_t fn = r.al();
	const uint8_t bl = static_cast<uint8_t>(r.bx & 0xff);
	if ((fn == 0x41 && r.si != 0) || (fn == 0x43 && bl > 1)) {
		return lfn_fail(r, DOSERR_FUNCTION_NUMBER_INVALID);
	}
	LfnTarget t;
	const uint16_t err = lfn_resolve(path, t);
	if (err == LfnNotLocal) {
		// 8.3 semantics through the DOS core
		const char* p = path.c_str();
		switch (fn) {
		case 0x39: r.result(DOS_MakeDir(p)); break;
		case 0x3a: r.result(DOS_RemoveDir(p)); break;
		case 0x3b: r.result(DOS_ChangeDir(p)); break;
		case 0x41: r.result(DOS_UnlinkFile(p)); break;
		default: {
			FatAttributeFlags a = {};
			if (bl == 0) {
				r.result(DOS_GetFileAttr(p, &a));
				if (!r.cf) r.cx = a._data;
			} else {
				r.result(DOS_SetFileAttr(p, FatAttributeFlags(r.cl())));
			}
		}
		}
		return Outcome::Handled;
	}
	if (err) {
		return lfn_fail(r, err);
	}
	if (fn == 0x39) { // MKDIR
		if (t.last.empty() || t.exists) {
			return lfn_fail(r, DOSERR_ACCESS_DENIED);
		}
		std::string native;
		const stdfs::path host = lfn_new_host_path(t, native);
		if (host.empty()) {
			return lfn_fail(r, HostLfn::valid_new_name(t.last) ? DOSERR_ACCESS_DENIED
			                                                   : DOSERR_PATH_NOT_FOUND);
		}
		std::error_code ec;
		if (!stdfs::create_directory(host, ec)) {
			return lfn_fail(r, DOSERR_ACCESS_DENIED);
		}
		t.dir.ld->dirCache.CacheOut((t.dir.cache + native).c_str(), true);
		resync_dos(lfn_short_path(t.dir, "X").c_str(), false);
		r.ok();
		return Outcome::Handled;
	}
	if (fn == 0x3b) { // CHDIR
		if (!t.last.empty()) {
			if (!t.exists || !t.entry.is_dir) {
				return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
			}
			lfn_descend(t.dir, t.entry);
		}
		const std::string dir = lfn_short_path(t.dir);
		r.result(DOS_ChangeDir(dir.c_str()));
		if (!r.cf) {
			watch_dos(dir.c_str(), true);
		}
		return Outcome::Handled;
	}
	if (t.last.empty() && fn != 0x43) {
		return lfn_fail(r, DOSERR_ACCESS_DENIED);
	}
	if (!t.last.empty() && !t.exists) {
		return lfn_fail(r, fn == 0x3a ? DOSERR_PATH_NOT_FOUND : DOSERR_FILE_NOT_FOUND);
	}
	const std::string target = lfn_target_short(t);
	switch (fn) {
	case 0x3a: // RMDIR
		if (!t.entry.is_dir) {
			return lfn_fail(r, DOSERR_PATH_NOT_FOUND);
		}
		r.result(DOS_RemoveDir(target.c_str()));
		break;
	case 0x41: // UNLINK
		if (t.entry.is_dir) {
			return lfn_fail(r, DOSERR_ACCESS_DENIED);
		}
		r.result(DOS_UnlinkFile(target.c_str()));
		break;
	default: { // 0x43 attributes
		FatAttributeFlags a = {};
		if (bl == 0) {
			if (t.last.empty()) {
				a = FatAttributeFlags(HostLfn::A_SUBDIR);
				r.ok();
			} else {
				r.result(DOS_GetFileAttr(target.c_str(), &a));
			}
			if (!r.cf) r.cx = a._data;
			return Outcome::Handled;
		}
		r.result(DOS_SetFileAttr(target.c_str(), FatAttributeFlags(r.cl())));
	}
	}
	if (!r.cf) {
		resync_dos(target.c_str(), false);
	}
	return Outcome::Handled;
}

/** INT 21h AX=71xxh. Never NotHandled: the regular path has no LFN. */
Outcome execute_lfn(Regs& r)
{
	switch (r.al()) {
	case 0x39:
	case 0x3a:
	case 0x3b:
	case 0x41:
	case 0x43: return lfn_path_call(r);
	case 0x47: return lfn_current_dir(r);
	case 0x4e: return lfn_find_first(r);
	case 0x4f: {
		const auto it = lfn_searches.find(r.bx);
		if (r.si != 1) {
			return lfn_fail(r, DOSERR_FUNCTION_NUMBER_INVALID);
		}
		if (it == lfn_searches.end()) {
			return lfn_fail(r, DOSERR_INVALID_HANDLE);
		}
		return lfn_find_out(r, it->second);
	}
	case 0xa1:
		if (!lfn_searches.erase(r.bx)) {
			return lfn_fail(r, DOSERR_INVALID_HANDLE);
		}
		r.ok();
		return Outcome::Handled;
	case 0x56: return lfn_rename(r);
	case 0x60: return lfn_full_path(r);
	case 0x6c: return lfn_create_open(r);
	case 0xa0: return lfn_volume_info(r);
	default:
		// As MS-DOS 7: AX = 7100h. The guest takes the extended error.
		lfn_fail(r, DOSERR_FUNCTION_NUMBER_INVALID);
		r.ax = 0x7100;
		return Outcome::Handled;
	}
}

std::string lfn_long_dir(const uint8_t drive, const std::string& short_dir)
{
	LfnDir d;
	if (!lfn_root(drive, d)) {
		return short_dir;
	}
	for (const auto& c : split_short(short_dir)) {
		lfn_list(d);
		const HostLfn::Entry* e = HostLfn::find_entry(d.entries, c);
		if (!e || !e->is_dir) {
			return short_dir;
		}
		const HostLfn::Entry copy = *e;
		lfn_descend(d, copy);
	}
	return join_comps(d.long_comps);
}

std::unordered_map<std::string, std::string> lfn_long_names(localDrive& ld,
                                                            const std::string& host_dir,
                                                            const std::string& cache_dir)
{
	std::unordered_map<std::string, std::string> names;
	for (const auto& e : HostLfn::list_dir(HostLfn::from_native(host_dir),
	                                       lfn_short_names(ld, cache_dir))) {
		names[e.native_name] = e.geos_name;
	}
	return names;
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

	case 0x71: // long file names, only if granted (bit 71h in the map)
		if (!fs_lfn) {
			return Outcome::NotHandled;
		}
		return execute_lfn(r);

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
	fs_lfn = (options & HIF_FS_OPT_LFN) != 0;
	if (fs_lfn) {
		bits[HOST_FS_LFN_FUNCTION >> 3] |= static_cast<uint8_t>(1u << (HOST_FS_LFN_FUNCTION & 7));
	}
	lfn_reset();
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
	LOG_INFO("GEOSHOST: FS fast path enabled (psp=%04x, %s mode%s)",
	         psp_seg,
	         selector_mode() ? "protected" : "real",
	         fs_lfn ? ", long file names" : "");
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
	if (fs_lfn && local_drive(static_cast<uint8_t>(drive))) flags |= HFDF_LFN;

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
