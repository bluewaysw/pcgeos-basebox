// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file hostfs_volume.h
 * @brief Live, writable host directory exposed as a GEOS volume.
 *
 * This is the semantic core of the GEOS host file system (hostfs.geo):
 * everything is done in GEOS terms (GEOS names, FileError codes, GEOS
 * file headers, extended attributes), but against the host directory as
 * it is right now. There are no snapshots: every lookup reads the
 * directory (with a per-directory cache that is dropped as soon as the
 * directory's modification time changes, or when we modify it).
 *
 * Naming
 *  - A host file that starts with a GEOS file header (as written by the
 *    DOS IFS drivers) is a GEOS file: its GEOS name and extended
 *    attributes come from the header, its data starts after it.
 *  - @DIRNAME.000 supplies a directory's legacy GEOS name and attributes
 *    and is never listed.
 *  - Every other name is the host name, decoded: "%XX" stands for the
 *    byte XX. Names GEOS creates are encoded the same way when they
 *    contain characters the host can't store (or non-ASCII GEOS
 *    characters), so they round-trip exactly.
 *  - GEOS names are limited to 32 characters; longer host names are cut
 *    and get "~" plus a hash of the full host name, keeping the extension.
 *    Host names that are valid 8.3 names are their own short name, all
 *    others get "X~HHHHHH.EXT". Names depend only on the file itself, so
 *    they stay the same when other files come and go.
 *  - Host names starting with '.' are hidden.
 *
 * Identity
 *  - Every file and directory has a 32-bit ID derived from its path
 *    relative to the root (case-insensitive FNV-1a, seeded per volume).
 *    It is stable across restarts and is what GEOS uses for current
 *    directories, file-change notification and open-file checks.
 *
 * This file has no DOSBox dependencies so it can be unit-tested.
 */

#ifndef DOSBOX_HOSTFS_VOLUME_H
#define DOSBOX_HOSTFS_VOLUME_H

#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HostFs {

namespace fs = std::filesystem;

// FileError (Include/file.def)
enum Error : uint16_t {
	OK                       = 0,
	ERROR_UNSUPPORTED_FUNCTION = 1,
	ERROR_FILE_NOT_FOUND     = 2,
	ERROR_PATH_NOT_FOUND     = 3,
	ERROR_TOO_MANY_OPEN_FILES = 4,
	ERROR_ACCESS_DENIED      = 5,
	ERROR_IS_CURRENT_DIRECTORY = 16,
	ERROR_NO_MORE_FILES      = 18,
	ERROR_WRITE_PROTECTED    = 19,
	ERROR_WRITE_FAULT        = 29,
	ERROR_READ_FAULT         = 30,
	ERROR_GENERAL_FAILURE    = 31,
	ERROR_SHORT_READ_WRITE   = 128,
	ERROR_INVALID_NAME       = 129,
	ERROR_FILE_EXISTS        = 130,
	ERROR_FILE_IN_USE        = 132,
	ERROR_DISK_UNAVAILABLE   = 134,
	ERROR_DIRECTORY_NOT_EMPTY = 138,
	ERROR_ATTR_NOT_SUPPORTED = 139,
	ERROR_ATTR_NOT_FOUND     = 140,
	ERROR_ATTR_SIZE_MISMATCH = 141,
	ERROR_ATTR_CANNOT_BE_SET = 142,
	ERROR_PATH_TOO_LONG      = 144,
	ERROR_NOT_A_LINK         = 149,
};

// FileAttrs
constexpr uint8_t FA_RDONLY = 0x01;
constexpr uint8_t FA_HIDDEN = 0x02;
constexpr uint8_t FA_SYSTEM = 0x04;
constexpr uint8_t FA_SUBDIR = 0x10;
constexpr uint8_t FA_ARCHIVE = 0x20;

// GeosFileType
constexpr uint16_t GFT_NOT_GEOS_FILE = 0;
constexpr uint16_t GFT_EXECUTABLE    = 1;
constexpr uint16_t GFT_VM            = 2;
constexpr uint16_t GFT_DATA          = 3;
constexpr uint16_t GFT_DIRECTORY     = 4;

// FileExtendedAttribute
enum Fea : uint16_t {
	FEA_MODIFICATION = 0,
	FEA_FILE_ATTR,
	FEA_SIZE,
	FEA_FILE_TYPE,
	FEA_FLAGS,
	FEA_RELEASE,
	FEA_PROTOCOL,
	FEA_TOKEN,
	FEA_CREATOR,
	FEA_USER_NOTES,
	FEA_NOTICE,
	FEA_CREATION,
	FEA_PASSWORD,
	FEA_CUSTOM,
	FEA_NAME,
	FEA_GEODE_ATTR,
	FEA_PATH_INFO,
	FEA_FILE_ID,
	FEA_DESKTOP_INFO,
	FEA_DRIVE_STATUS,
	FEA_DISK,
	FEA_DOS_NAME,
	FEA_OWNER,
	FEA_RIGHTS,
	FEA_TARGET_FILE_ID,
	FEA_MULTIPLE    = 0xfffe,
	FEA_END_OF_LIST = 0xffff,
};

// FullFileAccessFlags
constexpr uint8_t FFAF_RAW  = 0x80;
constexpr uint8_t FFAF_MODE = 0x03; // FA_READ_ONLY 0, FA_WRITE_ONLY 1, FA_READ_WRITE 2

// FileCreateFlags (in the high byte of cx for FSAOF_CREATE)
constexpr uint8_t FCF_NATIVE                = 0x80;
constexpr uint8_t FCF_NATIVE_WITH_EXT_ATTRS = 0x40;

// FilePosMode
constexpr uint8_t FILE_POS_START    = 0;
constexpr uint8_t FILE_POS_RELATIVE = 1;
constexpr uint8_t FILE_POS_END      = 2;

constexpr size_t LongNameLength = 32;
constexpr size_t DosNameLength  = 11;
constexpr uint32_t HeaderSize   = 256; // GeosFileHeader

/** Context the kernel knows and the host doesn't (for some attributes). */
struct KernelContext {
	uint16_t disk_handle  = 0; // FEA_DISK
	uint16_t path_info    = 0; // FEA_PATH_INFO (DirPathInfo)
	uint16_t drive_status = 0; // FEA_DRIVE_STATUS (DriveExtendedStatus)
};

/** One file or directory as GEOS sees it. */
struct Item {
	fs::path host        = {};
	std::string host_name = {};
	std::string rel      = {}; // path relative to the root, '/' separated
	bool is_dir          = false;
	bool has_header      = false; // GEOS file, or directory with @DIRNAME.000
	uint8_t header[HeaderSize] = {};
	uint8_t attrs        = 0; // FileAttrs
	uint32_t size        = 0; // data size (without header)
	uint16_t mdate = 0, mtime = 0;
	uint16_t geode_attrs = 0;
	uint32_t id          = 0;
	char long_name[LongNameLength + 1] = {};
	char dos_name[DosNameLength]       = {};

	uint16_t file_type() const;
};

/** Result of resolving a GEOS path. */
struct Lookup {
	bool exists        = false; // final component found
	uint32_t parent_id = 0;
	fs::path parent    = {};    // host directory containing the final component
	std::string parent_rel = {};
	std::string last   = {};    // final component as given (GEOS name)
	Item item          = {};    // valid if exists
};

/** A change on the host that GEOS didn't make (see Volume::poll). */
struct ChangeEvent {
	enum Type : uint8_t { Created, Deleted, Contents } type = Created;
	uint32_t id      = 0;  // Created: containing directory; else the item
	std::string name = {}; // Created: GEOS name of the new item
};

/** Value of one extended attribute, in GEOS layout. */
struct AttrValue {
	std::vector<uint8_t> bytes;
};

class Volume {
public:
	/**
	 * @param root  host directory to serve
	 * @param name  GEOS drive/volume name
	 * @param seed  mixed into all IDs, so volumes don't share IDs
	 */
	Volume(fs::path root, std::string name, uint32_t seed = 0);
	~Volume();

	Volume(const Volume&)            = delete;
	Volume& operator=(const Volume&) = delete;

	bool ok() const { return ok_; }
	const std::string& name() const { return name_; }
	uint32_t root_id() const { return root_id_; }

	/** Free/total bytes, clamped to 31 bits as GEOS stores them signed. */
	void space(uint32_t& free_bytes, uint32_t& total_bytes) const;

	// --- paths ---------------------------------------------------------

	/**
	 * Resolve a GEOS path (components separated by '\', absolute if it
	 * starts with '\', else relative to directory @p base_id).
	 * Components match GEOS names, host names and 8.3 short names; with
	 * @p final_alias false the last component must be a real name (used
	 * when creating, so a new name never collides with a short name).
	 */
	Error lookup(uint32_t base_id, std::string_view path, Lookup& out,
	             bool final_alias = true);

	/** Directory ID -> item (for current directories). */
	Error dir_item(uint32_t id, Item& out);

	/** Current entries of a directory (fresh as of now). */
	Error list(uint32_t dir_id, std::vector<Item>& out);

	Error make_dir(uint32_t base_id, std::string_view path, Item& created);
	Error remove_dir(uint32_t base_id, std::string_view path, uint32_t& id);
	Error remove_file(uint32_t base_id, std::string_view path, uint32_t& id);
	Error rename(uint32_t base_id, std::string_view path,
	             std::string_view new_name, Item& renamed);
	Error move(uint32_t base_id, std::string_view path,
	           uint32_t dest_base_id, std::string_view dest_path, Item& moved);
	Error get_attrs(uint32_t base_id, std::string_view path, uint8_t& attrs);
	Error set_attrs(uint32_t base_id, std::string_view path, uint8_t attrs);

	// --- extended attributes -------------------------------------------

	Error get_ext_attr(const Item& item, uint16_t attr, const KernelContext& ctx,
	                   AttrValue& out) const;
	/** Set one attribute on a path or (via @p handle != 0) an open file. */
	Error set_ext_attr(uint32_t base_id, std::string_view path, uint16_t handle,
	                   uint16_t attr, const uint8_t* value, uint16_t size);
	/** Attributes reported by GET_ALL (never FEA_NAME). */
	std::vector<uint16_t> all_ext_attrs(const Item& item) const;

	// --- open files ----------------------------------------------------

	Error open(uint32_t base_id, std::string_view path, uint8_t access,
	           uint16_t& handle, Item& item);
	Error create(uint32_t base_id, std::string_view path, uint8_t attrs,
	             uint8_t create_flags, uint8_t access, uint16_t& handle, Item& item);
	Error read(uint16_t handle, uint8_t* buf, uint16_t count, uint16_t& got);
	Error write(uint16_t handle, const uint8_t* buf, uint16_t count, uint16_t& put);
	Error seek(uint16_t handle, uint8_t mode, int32_t offset, uint32_t& pos);
	Error truncate(uint16_t handle, uint32_t size);
	Error commit(uint16_t handle);
	Error file_size(uint16_t handle, uint32_t& size);
	Error get_date_time(uint16_t handle, uint16_t& date, uint16_t& time);
	Error set_date_time(uint16_t handle, uint16_t date, uint16_t time);
	Error add_ref(uint16_t handle);
	Error is_dirty(uint16_t handle, bool& dirty);
	/** Drops one reference; @p closed tells whether the file is now closed. */
	Error close(uint16_t handle, bool& closed, bool& was_dirty, uint32_t& id);
	Error handle_item(uint16_t handle, Item& item);
	bool is_open(const fs::path& host) const;

	// --- change detection ----------------------------------------------

	/**
	 * Directories GEOS reads are watched (the most recently used
	 * MaxWatchedDirs). poll() compares them with their last known state
	 * and queues what changed on the host: new and deleted entries, and
	 * files whose size or time changed (unless GEOS has them open).
	 * Changes GEOS makes itself through this class are not reported, as
	 * GEOS notifies about those already.
	 * @return the next queued change, or false if there is none
	 */
	bool next_change(ChangeEvent& event);
	void poll();
	bool has_changes() const { return !changes_.empty(); }

	static constexpr size_t MaxWatchedDirs   = 32;
	static constexpr size_t MaxCachedHeaders = 50000;
	/** Longest a directory listing is reused without reading it again. */
	static constexpr int ListingMaxAgeMs = 2000;

	/** Number of host files opened to read a GEOS header (for tests). */
	size_t header_reads() const { return header_reads_; }

	// --- enumeration ---------------------------------------------------

	Error enum_begin(uint32_t dir_id, uint16_t& enum_handle);
	/** Returns ERROR_NO_MORE_FILES at the end (and frees the enum). */
	Error enum_next(uint16_t enum_handle, Item& item);
	void enum_end(uint16_t enum_handle);

private:
	struct Listing;
	struct OpenFile;
	struct Watch;

	const Listing* get_listing(const fs::path& dir, const std::string& rel, Error& err,
	                           bool record_watch = true, bool fresh = false);
	void refresh_item(Item& item);
	void invalidate(const fs::path& dir);
	void watch(const fs::path& dir, const std::string& rel, const std::vector<Item>& items);
	bool cached_header(const fs::path& file, uintmax_t size, fs::file_time_type mtime,
	                   uint8_t (&header)[HeaderSize], uint16_t* geode_attrs);
	Error resolve_dir(uint32_t id, fs::path& host, std::string& rel);
	uint32_t id_for(const std::string& rel) const;
	void remember_dir(uint32_t id, const std::string& rel);
	Error write_header(const fs::path& host, const uint8_t* header);
	Error update_header_field(Item& item, size_t offset, const uint8_t* value, size_t size);
	OpenFile* file(uint16_t handle);
	Error make_host_name(const fs::path& parent, const std::string& parent_rel,
	                     std::string_view geos_name, std::string& host_name);
	Error open_host(const Item& item, uint8_t access, bool create_new,
	                uint16_t& handle);

	fs::path root_;
	std::string name_;
	bool ok_          = false;
	uint32_t seed_    = 0;
	uint32_t root_id_ = 0;

	std::unordered_map<std::string, std::unique_ptr<Listing>> listings_; // by host path
	std::unordered_map<uint32_t, std::string> dir_ids_;                  // id -> rel
	std::vector<std::unique_ptr<OpenFile>> files_;                       // index = handle - 1
	std::map<uint16_t, std::vector<Item>> enums_;
	uint16_t next_enum_ = 1;

	// GEOS headers by host path, valid while size and time are unchanged,
	// so listing a directory doesn't open every file in it each time
	struct HeaderCacheEntry {
		uintmax_t size                 = 0;
		fs::file_time_type mtime       = {};
		bool has_header                = false;
		uint8_t header[HeaderSize]     = {};
		uint16_t geode_attrs           = 0;
	};
	std::unordered_map<std::string, HeaderCacheEntry> header_cache_;
	size_t header_reads_ = 0;

	std::unordered_map<std::string, std::unique_ptr<Watch>> watches_; // by host path
	uint64_t watch_clock_ = 0;
	std::deque<ChangeEvent> changes_;
};

// Helpers, exposed for tests
std::string decode_host_name(const std::string& host_name);
std::string encode_geos_name(std::string_view geos_name);
bool to_dos_name(const std::string& name, char (&out)[DosNameLength]);
uint16_t to_file_date(int year, int month, int day);
uint16_t to_file_time(int hour, int minute, int second);
uint32_t path_id(const std::string& rel, uint32_t seed = 0);
/** Assign GEOS and 8.3 names to items sorted by host name. */
void assign_names(std::vector<Item>& items);

/**
 * Parse one line of the [hostfs] drives list in GEOS.INI:
 * "NAME=HOST PATH". Spaces around both parts are ignored.
 * @return false if the line is not of that form
 */
bool parse_mount_line(std::string_view line, std::string& name, std::string& path);

/**
 * Whether @p dir may be served, given a ';'-separated list of allowed
 * roots (empty list: everything is allowed). @p dir must be the given
 * root or inside it, after resolving "..", links and case (Windows).
 */
bool path_allowed(const fs::path& dir, std::string_view allow_list);

} // namespace HostFs

#endif // DOSBOX_HOSTFS_VOLUME_H
