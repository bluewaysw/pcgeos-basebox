// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file hostfs_volume.cpp
 * @brief Live, writable host directory exposed as a GEOS volume.
 */

#include "hostfs_volume.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <set>
#include <system_error>

namespace HostFs {

namespace {

// GeosFileHeader layout (Include/Internal/fileStr.def)
constexpr size_t GFH_longName  = 4;   // FileLongName, 36 bytes
constexpr size_t GFH_type      = 40;  // word
constexpr size_t GFH_flags     = 42;  // word
constexpr size_t GFH_release   = 44;  // 8
constexpr size_t GFH_protocol  = 52;  // 4
constexpr size_t GFH_token     = 56;  // 6
constexpr size_t GFH_creator   = 62;  // 6
constexpr size_t GFH_userNotes = 68;  // 100
constexpr size_t GFH_notice    = 168; // 32
constexpr size_t GFH_created   = 200; // 4: date, time
constexpr size_t GFH_password  = 204; // 8
constexpr size_t GFH_desktop   = 212; // 16

constexpr char DirNameFile[] = "@DIRNAME.000";

uint16_t get16(const uint8_t* p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void put16(uint8_t* p, const uint16_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

void put16(std::vector<uint8_t>& v, const uint16_t x)
{
	v.push_back(static_cast<uint8_t>(x));
	v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t>& v, const uint32_t x)
{
	for (int i = 0; i < 4; ++i) {
		v.push_back(static_cast<uint8_t>(x >> (8 * i)));
	}
}

bool is_geos_header(const uint8_t* h)
{
	return h[0] == ('G' | 0x80) && h[1] == 'E' && h[2] == ('A' | 0x80) &&
	       h[3] == 'S';
}

std::string upper(std::string s)
{
	for (auto& c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::toupper(static_cast<unsigned char>(a[i])) !=
		    std::toupper(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

bool is_dos_char(const char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) ||
	       (c != '\0' && std::strchr("!#$%&'()-@^_`{}~", c) != nullptr);
}

std::string utf8_name(const fs::path& p)
{
	const auto u8 = p.filename().u8string();
	return std::string(u8.begin(), u8.end());
}

fs::path from_utf8(const std::string& s)
{
	return fs::path(std::u8string(s.begin(), s.end()));
}

std::FILE* open_file(const fs::path& p, const char* mode)
{
#ifdef _WIN32
	std::wstring wmode(mode, mode + std::strlen(mode));
	return _wfopen(p.c_str(), wmode.c_str());
#else
	return std::fopen(p.c_str(), mode);
#endif
}

bool seek_file(std::FILE* f, const int64_t pos)
{
#ifdef _WIN32
	return _fseeki64(f, pos, SEEK_SET) == 0;
#else
	return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

bool read_header(const fs::path& p, uint8_t (&h)[HeaderSize], uint16_t* geode_attrs)
{
	std::FILE* f = open_file(p, "rb");
	if (!f) {
		return false;
	}
	uint8_t buf[HeaderSize + 2] = {};
	const size_t got = std::fread(buf, 1, sizeof(buf), f);
	std::fclose(f);
	if (got < HeaderSize || !is_geos_header(buf)) {
		return false;
	}
	std::memcpy(h, buf, HeaderSize);
	if (geode_attrs) {
		// ExecutableFileHeader.EFH_attributes is the first data word
		*geode_attrs = (get16(buf + GFH_type) == GFT_EXECUTABLE &&
		                got >= HeaderSize + 2)
		                     ? get16(buf + HeaderSize)
		                     : 0;
	}
	return true;
}

void to_date_time(const fs::file_time_type ft, uint16_t& date, uint16_t& time)
{
	const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
	const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
	std::tm t{};
#ifdef _WIN32
	const bool ok = localtime_s(&t, &tt) == 0;
#else
	const bool ok = localtime_r(&tt, &t) != nullptr;
#endif
	if (!ok) {
		date = to_file_date(1980, 1, 1);
		time = 0;
		return;
	}
	date = to_file_date(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
	time = to_file_time(t.tm_hour, t.tm_min, t.tm_sec);
}

fs::file_time_type from_date_time(const uint16_t date, const uint16_t time)
{
	std::tm t{};
	t.tm_year  = ((date >> 9) & 0x7f) + 80;
	t.tm_mon   = ((date >> 5) & 0x0f) - 1;
	t.tm_mday  = date & 0x1f;
	t.tm_hour  = time >> 11;
	t.tm_min   = (time >> 5) & 0x3f;
	t.tm_sec   = (time & 0x1f) * 2;
	t.tm_isdst = -1;
	const auto sys = std::chrono::system_clock::from_time_t(std::mktime(&t));
	return std::chrono::clock_cast<fs::file_time_type::clock>(sys);
}

void now_date_time(uint16_t& date, uint16_t& time)
{
	to_date_time(std::chrono::clock_cast<fs::file_time_type::clock>(
	                     std::chrono::system_clock::now()),
	             date,
	             time);
}

std::string join_rel(const std::string& parent, const std::string& name)
{
	return parent.empty() ? name : parent + "/" + name;
}

/** GEOS names reachable through the GEOS path syntax only. */
std::string geos_long_name(const Item& it)
{
	if (it.has_header) {
		const char* n = reinterpret_cast<const char*>(it.header + GFH_longName);
		const std::string s(n, strnlen(n, LongNameLength));
		if (!s.empty()) {
			return s;
		}
	}
	return decode_host_name(it.host_name);
}

} // namespace

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

std::string decode_host_name(const std::string& host_name)
{
	auto hex = [](const char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		return -1;
	};
	std::string out;
	for (size_t i = 0; i < host_name.size();) {
		const auto c = static_cast<unsigned char>(host_name[i]);
		if (c == '%' && i + 2 < host_name.size() && hex(host_name[i + 1]) >= 0 &&
		    hex(host_name[i + 2]) >= 0) {
			const auto b = static_cast<char>(hex(host_name[i + 1]) * 16 +
			                                 hex(host_name[i + 2]));
			// a path separator can't be part of a GEOS name
			out.push_back((b == '\\' || b == '\0') ? '_' : b);
			i += 3;
			continue;
		}
		if (c >= 0x80) {
			// Unicode beyond ASCII: no GEOS mapping yet
			out.push_back('_');
			++i;
			while (i < host_name.size() &&
			       (static_cast<unsigned char>(host_name[i]) & 0xc0) == 0x80) {
				++i;
			}
			continue;
		}
		out.push_back(c < 0x20 ? '_' : static_cast<char>(c));
		++i;
	}
	return out;
}

std::string encode_geos_name(std::string_view geos_name)
{
	static const char hexdig[] = "0123456789ABCDEF";
	std::string out;
	for (size_t i = 0; i < geos_name.size(); ++i) {
		const auto c = static_cast<unsigned char>(geos_name[i]);
		const bool last = i + 1 == geos_name.size();
		const bool escape = c < 0x20 || c >= 0x7f ||
		                    std::strchr("\\/:*?\"<>|%", c) != nullptr ||
		                    (i == 0 && c == '.') || // would be hidden
		                    (last && (c == '.' || c == ' ')); // Windows
		if (escape) {
			out.push_back('%');
			out.push_back(hexdig[c >> 4]);
			out.push_back(hexdig[c & 0xf]);
		} else {
			out.push_back(static_cast<char>(c));
		}
	}
	return out;
}

bool to_dos_name(const std::string& name, char (&out)[DosNameLength])
{
	std::memset(out, ' ', DosNameLength);
	const auto dot = name.find('.');
	const std::string base = name.substr(0, dot);
	const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
	if (base.empty() || base.size() > 8 || ext.size() > 3 ||
	    ext.find('.') != std::string::npos ||
	    (dot != std::string::npos && ext.empty())) {
		return false;
	}
	for (const char c : base + ext) {
		if (!is_dos_char(c)) {
			return false;
		}
	}
	const auto ub = upper(base);
	const auto ue = upper(ext);
	std::memcpy(out, ub.data(), ub.size());
	std::memcpy(out + 8, ue.data(), ue.size());
	return true;
}

uint16_t to_file_date(const int year, const int month, const int day)
{
	const int y = std::clamp(year - 1980, 0, 127);
	return static_cast<uint16_t>((y << 9) | ((month & 0xf) << 5) | (day & 0x1f));
}

uint16_t to_file_time(const int hour, const int minute, const int second)
{
	return static_cast<uint16_t>(((hour & 0x1f) << 11) | ((minute & 0x3f) << 5) |
	                             ((second / 2) & 0x1f));
}

uint32_t path_id(const std::string& rel, const uint32_t seed)
{
	uint32_t h = 0x811c9dc5u ^ seed;
	for (const char c : rel) {
		h ^= static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(c)));
		h *= 0x01000193u;
	}
	return h ? h : 1;
}

bool parse_mount_line(std::string_view line, std::string& name, std::string& path)
{
	auto trim = [](std::string_view v) {
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) {
			v.remove_prefix(1);
		}
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) {
			v.remove_suffix(1);
		}
		return std::string(v);
	};
	const auto eq = line.find('=');
	if (eq == std::string_view::npos) {
		return false;
	}
	name = trim(line.substr(0, eq));
	path = trim(line.substr(eq + 1));
	// Allow quoting of paths with leading/trailing spaces
	if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
		path = path.substr(1, path.size() - 2);
	}
	return !name.empty() && name.size() <= LongNameLength && !path.empty();
}

bool path_allowed(const fs::path& dir, std::string_view allow_list)
{
	if (allow_list.empty()) {
		return true;
	}
	std::error_code ec;
	const fs::path real = fs::weakly_canonical(dir, ec);
	if (ec) {
		return false;
	}
	auto norm = [](const fs::path& p) {
#ifdef _WIN32
		return upper(p.generic_string());
#else
		return p.generic_string();
#endif
	};
	const std::string target = norm(real);
	size_t start = 0;
	while (start <= allow_list.size()) {
		size_t end = allow_list.find(';', start);
		if (end == std::string_view::npos) {
			end = allow_list.size();
		}
		const std::string_view entry = allow_list.substr(start, end - start);
		start = end + 1;
		if (entry.empty()) {
			continue;
		}
		std::error_code aec;
		std::string root = norm(fs::weakly_canonical(fs::path(std::string(entry)), aec));
		if (aec) {
			continue;
		}
		while (root.size() > 1 && root.back() == '/') {
			root.pop_back();
		}
		if (target == root || (target.size() > root.size() &&
		                       target.compare(0, root.size(), root) == 0 &&
		                       (target[root.size()] == '/' || root.back() == '/'))) {
			return true;
		}
	}
	return false;
}

uint16_t Item::file_type() const
{
	if (is_dir) {
		return GFT_DIRECTORY;
	}
	return has_header ? get16(header + GFH_type) : GFT_NOT_GEOS_FILE;
}

/* ----------------------------------------------------------------
 * Volume
 * ---------------------------------------------------------------- */

struct Volume::Listing {
	std::vector<Item> items;
	fs::file_time_type dir_mtime = {};
	std::chrono::steady_clock::time_point built = {};
};

struct Volume::Watch {
	struct Seen {
		bool is_dir    = false;
		uint32_t size  = 0;
		uint16_t mdate = 0, mtime = 0;
		uint32_t id    = 0;
	};
	std::string rel = {};
	fs::path dir    = {};
	uint64_t used   = 0; // watch_clock_ at last use
	std::unordered_map<std::string, Seen> entries; // by host name
};

struct Volume::OpenFile {
	std::FILE* f   = nullptr;
	fs::path host  = {};
	std::string rel = {};
	uint32_t id    = 0;
	uint32_t skip  = 0; // GEOS header bytes hidden from the data
	uint32_t pos   = 0; // data position
	bool writable  = false;
	bool dirty     = false;
	int refs       = 1;
};

Volume::Volume(fs::path root, std::string name, const uint32_t seed)
        : root_(std::move(root)),
          name_(std::move(name)),
          seed_(seed)
{
	if (name_.size() > LongNameLength) {
		name_.resize(LongNameLength);
	}
	std::error_code ec;
	ok_      = fs::is_directory(root_, ec);
	root_id_ = path_id("", seed_);
	dir_ids_[root_id_] = "";
}

Volume::~Volume()
{
	for (auto& f : files_) {
		if (f && f->f) {
			std::fclose(f->f);
		}
	}
}

void Volume::space(uint32_t& free_bytes, uint32_t& total_bytes) const
{
	std::error_code ec;
	const auto s = fs::space(root_, ec);
	const uintmax_t cap = 0x7fffffff;
	free_bytes  = ec ? 0 : static_cast<uint32_t>(std::min<uintmax_t>(s.available, cap));
	total_bytes = ec ? 0 : static_cast<uint32_t>(std::min<uintmax_t>(s.capacity, cap));
}

uint32_t Volume::id_for(const std::string& rel) const
{
	return rel.empty() ? root_id_ : path_id(rel, seed_);
}

void Volume::remember_dir(const uint32_t id, const std::string& rel)
{
	dir_ids_[id] = rel;
}

Error Volume::resolve_dir(const uint32_t id, fs::path& host, std::string& rel)
{
	const auto it = dir_ids_.find(id);
	if (it == dir_ids_.end()) {
		return ERROR_PATH_NOT_FOUND;
	}
	rel  = it->second;
	host = rel.empty() ? root_ : root_ / from_utf8(rel);
	std::error_code ec;
	if (!fs::is_directory(host, ec)) {
		return ERROR_PATH_NOT_FOUND;
	}
	return OK;
}

/* ----------------------------------------------------------------
 * Stable names
 *
 * A name only depends on the host name of the file itself, not on the
 * other files in the directory, so it doesn't change when files come and
 * go on the host:
 *  - GEOS names up to 32 characters are used as they are; longer ones are
 *    cut and get "~" plus 6 hex digits of a hash of the full host name,
 *    keeping the extension: "A very long name that goes on.png" ->
 *    "A very long name th~4F2A9C.png".
 *  - Host names that are valid 8.3 names are their own short name; all
 *    others get "X~HHHHHH.EXT" (first character, hash, extension).
 * Only if two files would still get the same name (case-only differences,
 * or a hash collision) does the one whose host name sorts later get a
 * different hash.
 * ---------------------------------------------------------------- */

namespace {

std::string hash6(const std::string& host_name, const int salt)
{
	uint32_t h = 0x811c9dc5u;
	for (const char c : host_name) {
		h ^= static_cast<uint8_t>(c);
		h *= 0x01000193u;
	}
	for (int i = 0; i < salt; ++i) {
		h ^= 0x5au;
		h *= 0x01000193u;
	}
	h ^= h >> 24; // fold to 24 bits
	char buf[8];
	std::snprintf(buf, sizeof(buf), "%06X", h & 0xffffffu);
	return buf;
}

std::string shortened_long_name(const std::string& name, const std::string& host_name,
                                const int salt)
{
	std::string ext;
	const auto dot = name.rfind('.');
	if (dot != std::string::npos && dot > 0 && name.size() - dot <= 5) {
		ext = name.substr(dot); // ".png", ".jpeg"
	}
	const std::string tag = "~" + hash6(host_name, salt);
	const size_t room = LongNameLength - tag.size() - ext.size();
	std::string base = name.substr(0, name.size() - ext.size());
	if (base.size() > room) {
		base.resize(room);
	}
	return base + tag + ext;
}

void hashed_dos_name(const std::string& host_name, const int salt, char (&out)[DosNameLength])
{
	std::memset(out, ' ', DosNameLength);
	const auto dot = host_name.rfind('.');
	char first = '_';
	for (size_t k = 0; k < host_name.size() && k < dot; ++k) {
		const char c = host_name[k];
		if (std::isalnum(static_cast<unsigned char>(c))) {
			first = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			break;
		}
	}
	const std::string b = std::string(1, first) + "~" + hash6(host_name, salt);
	std::memcpy(out, b.data(), b.size());
	if (dot != std::string::npos) {
		size_t n = 0;
		for (size_t k = dot + 1; k < host_name.size() && n < 3; ++k) {
			const char c = host_name[k];
			if (is_dos_char(c) && c != '%' && c != '~') {
				out[8 + n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
		}
	}
}

} // namespace

void assign_names(std::vector<Item>& items)
{
	// items are sorted by host name, so "sorts later" is "comes later"
	std::set<std::string> longs, doses;
	for (auto& item : items) {
		const std::string natural = geos_long_name(item);
		std::string ln = natural.size() <= LongNameLength
		                       ? natural
		                       : shortened_long_name(natural, item.host_name, 0);
		for (int salt = 1; longs.count(upper(ln)) && salt < 1000; ++salt) {
			ln = shortened_long_name(natural, item.host_name, salt);
		}
		longs.insert(upper(ln));
		std::memset(item.long_name, 0, sizeof(item.long_name));
		std::memcpy(item.long_name, ln.data(), ln.size());

		char dos[DosNameLength];
		if (!to_dos_name(item.host_name, dos)) {
			hashed_dos_name(item.host_name, 0, dos);
		}
		for (int salt = 1; doses.count(std::string(dos, DosNameLength)) && salt < 1000; ++salt) {
			hashed_dos_name(item.host_name, salt, dos);
		}
		doses.insert(std::string(dos, DosNameLength));
		std::memcpy(item.dos_name, dos, DosNameLength);
	}
}

const Volume::Listing* Volume::get_listing(const fs::path& dir, const std::string& rel,
                                           Error& err, const bool record_watch,
                                           const bool fresh)
{
	// Reuse the listing while the directory's time is unchanged (entries
	// added, removed or renamed change it) and it isn't older than
	// ListingMaxAgeMs (for file systems that don't keep the time well).
	// Callers refresh the size and time of the entries they hand out.
	const auto now = std::chrono::steady_clock::now();
	std::error_code tec0;
	const auto dir_mtime = fs::last_write_time(dir, tec0);
	if (!fresh && !tec0) {
		const auto cached = listings_.find(dir.string());
		if (cached != listings_.end() && cached->second->dir_mtime == dir_mtime &&
		    now - cached->second->built < std::chrono::milliseconds(ListingMaxAgeMs)) {
			err = OK;
			if (record_watch) {
				watch(dir, rel, cached->second->items);
			}
			return cached->second.get();
		}
	}

	auto listing = std::make_unique<Listing>();
	listing->dir_mtime = dir_mtime;
	listing->built     = now;
	std::error_code ec;
	fs::directory_iterator it(dir, ec);
	if (ec) {
		err = ERROR_PATH_NOT_FOUND;
		return nullptr;
	}
	for (const auto& e : it) {
		Item item;
		item.host      = e.path();
		item.host_name = utf8_name(e.path());
		if (item.host_name.empty() || item.host_name[0] == '.' ||
		    iequals(item.host_name, DirNameFile)) {
			continue;
		}
		std::error_code tec;
		item.is_dir = e.is_directory(tec);
		if (tec || (!item.is_dir && !e.is_regular_file(tec))) {
			continue;
		}
		item.rel = join_rel(rel, item.host_name);
		item.id  = id_for(item.rel);

		std::error_code mec;
		const auto mt = e.last_write_time(mec);
		if (!mec) {
			to_date_time(mt, item.mdate, item.mtime);
		}
		const auto perms = e.status(mec).permissions();
		if (!mec && (perms & fs::perms::owner_write) == fs::perms::none) {
			item.attrs |= FA_RDONLY;
		}

		if (item.is_dir) {
			item.attrs |= FA_SUBDIR;
			// @DIRNAME.000 only exists in directories the DOS drivers
			// wrote; a failed stat is much cheaper than a failed open
			const fs::path dn = item.host / DirNameFile;
			std::error_code dec;
			const auto dsz = fs::file_size(dn, dec);
			if (!dec) {
				const auto dmt = fs::last_write_time(dn, dec);
				item.has_header = !dec && cached_header(dn, dsz, dmt, item.header, nullptr);
			}
			remember_dir(item.id, item.rel);
		} else {
			std::error_code sec;
			const auto sz = e.file_size(sec);
			item.has_header = !sec && !mec &&
			                  cached_header(item.host, sz, mt, item.header, &item.geode_attrs);
			const uintmax_t data = sec ? 0
			                           : (item.has_header && sz >= HeaderSize
			                                      ? sz - HeaderSize
			                                      : sz);
			item.size = static_cast<uint32_t>(std::min<uintmax_t>(data, 0xffffffffu));
		}
		listing->items.push_back(std::move(item));
	}

	// Stable order, so name collisions resolve the same way every time
	std::sort(listing->items.begin(), listing->items.end(), [](const Item& a, const Item& b) {
		return upper(a.host_name) < upper(b.host_name);
	});

	assign_names(listing->items);

	err = OK;
	if (record_watch) {
		watch(dir, rel, listing->items);
	}
	auto& slot = listings_[dir.string()];
	slot = std::move(listing);
	return slot.get();
}

bool Volume::cached_header(const fs::path& file, const uintmax_t size,
                           const fs::file_time_type mtime, uint8_t (&header)[HeaderSize],
                           uint16_t* geode_attrs)
{
	const std::string key = file.string();
	auto it = header_cache_.find(key);
	if (it == header_cache_.end() || it->second.size != size || it->second.mtime != mtime) {
		if (header_cache_.size() >= MaxCachedHeaders) {
			header_cache_.clear(); // simple bound; refills as needed
		}
		HeaderCacheEntry entry;
		entry.size  = size;
		entry.mtime = mtime;
		++header_reads_;
		// Files smaller than a header can't be GEOS files: no need to open
		entry.has_header = size >= HeaderSize &&
		                   read_header(file, entry.header, &entry.geode_attrs);
		it = header_cache_.insert_or_assign(key, entry).first;
	}
	if (it->second.has_header) {
		std::memcpy(header, it->second.header, HeaderSize);
		if (geode_attrs) {
			*geode_attrs = it->second.geode_attrs;
		}
	}
	return it->second.has_header;
}

void Volume::refresh_item(Item& item)
{
	std::error_code ec;
	const auto mt = fs::last_write_time(item.host, ec);
	if (ec) {
		return;
	}
	to_date_time(mt, item.mdate, item.mtime);
	if (item.is_dir) {
		// GEOS attributes of the directory itself live in @DIRNAME.000
		const fs::path dn = item.host / DirNameFile;
		const auto dsz = fs::file_size(dn, ec);
		if (!ec) {
			const auto dmt = fs::last_write_time(dn, ec);
			if (!ec) {
				item.has_header = cached_header(dn, dsz, dmt, item.header, nullptr);
			}
		}
		return;
	}
	const auto sz = fs::file_size(item.host, ec);
	if (ec) {
		return;
	}
	item.has_header = cached_header(item.host, sz, mt, item.header, &item.geode_attrs);
	const uintmax_t data = item.has_header && sz >= HeaderSize ? sz - HeaderSize : sz;
	item.size = static_cast<uint32_t>(std::min<uintmax_t>(data, 0xffffffffu));
}

void Volume::invalidate(const fs::path& dir)
{
	listings_.erase(dir.string());
	// GEOS changed this directory itself (and notifies about that): take
	// the new state as known, so poll() doesn't report it again.
	const auto it = watches_.find(dir.string());
	if (it != watches_.end()) {
		Error err = OK;
		const std::string rel = it->second->rel;
		watches_.erase(it);
		get_listing(dir, rel, err); // re-creates the watch
	}
}

void Volume::watch(const fs::path& dir, const std::string& rel, const std::vector<Item>& items)
{
	const std::string key = dir.string();
	auto it = watches_.find(key);
	if (it != watches_.end()) {
		it->second->used = ++watch_clock_; // known: poll() keeps it current
		return;
	}
	if (watches_.size() >= MaxWatchedDirs) {
		auto oldest = watches_.begin();
		for (auto w = watches_.begin(); w != watches_.end(); ++w) {
			if (w->second->used < oldest->second->used) {
				oldest = w;
			}
		}
		watches_.erase(oldest);
	}
	auto w  = std::make_unique<Watch>();
	w->rel  = rel;
	w->dir  = dir;
	w->used = ++watch_clock_;
	for (const auto& item : items) {
		w->entries[item.host_name] = {item.is_dir, item.size, item.mdate, item.mtime, item.id};
	}
	watches_[key] = std::move(w);
}

void Volume::poll()
{
	std::vector<std::string> gone;
	for (auto& [key, w] : watches_) {
		std::error_code ec;
		if (!fs::is_directory(w->dir, ec)) {
			gone.push_back(key); // the parent's watch reports the deletion
			continue;
		}
		Error err = OK;
		const Listing* l = get_listing(w->dir, w->rel, err, false, true);
		if (!l) {
			continue;
		}
		const uint32_t dir_id = id_for(w->rel);
		std::unordered_map<std::string, Watch::Seen> now;
		for (const auto& item : l->items) {
			now[item.host_name] = {item.is_dir, item.size, item.mdate, item.mtime, item.id};
			const auto prev = w->entries.find(item.host_name);
			if (prev == w->entries.end()) {
				changes_.push_back({ChangeEvent::Created, dir_id, item.long_name});
			} else if (!item.is_dir &&
			           (prev->second.size != item.size || prev->second.mdate != item.mdate ||
			            prev->second.mtime != item.mtime) &&
			           !is_open(item.host)) {
				changes_.push_back({ChangeEvent::Contents, item.id, {}});
			}
		}
		for (const auto& [name, seen] : w->entries) {
			if (!now.count(name)) {
				changes_.push_back({ChangeEvent::Deleted, seen.id, {}});
			}
		}
		w->entries = std::move(now);
	}
	for (const auto& key : gone) {
		watches_.erase(key);
	}
}

bool Volume::next_change(ChangeEvent& event)
{
	if (changes_.empty()) {
		return false;
	}
	event = std::move(changes_.front());
	changes_.pop_front();
	return true;
}

Error Volume::list(const uint32_t dir_id, std::vector<Item>& out)
{
	fs::path host;
	std::string rel;
	if (const auto e = resolve_dir(dir_id, host, rel); e != OK) {
		return e;
	}
	Error err = OK;
	const Listing* l = get_listing(host, rel, err);
	if (!l) {
		return err;
	}
	out = l->items;
	for (auto& item : out) {
		refresh_item(item);
	}
	return OK;
}

Error Volume::dir_item(const uint32_t id, Item& out)
{
	fs::path host;
	std::string rel;
	if (const auto e = resolve_dir(id, host, rel); e != OK) {
		return e;
	}
	if (rel.empty()) {
		out          = Item{};
		out.host     = root_;
		out.is_dir   = true;
		out.attrs    = FA_SUBDIR;
		out.id       = root_id_;
		std::memcpy(out.long_name, name_.data(), name_.size());
		std::memset(out.dos_name, ' ', DosNameLength);
		std::error_code ec;
		const auto mt = fs::last_write_time(root_, ec);
		if (!ec) {
			to_date_time(mt, out.mdate, out.mtime);
		}
		return OK;
	}
	const auto slash = rel.rfind('/');
	const std::string parent_rel = slash == std::string::npos ? "" : rel.substr(0, slash);
	const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
	Error err = OK;
	const Listing* l = get_listing(parent_rel.empty() ? root_ : root_ / from_utf8(parent_rel),
	                               parent_rel,
	                               err);
	if (!l) {
		return err;
	}
	for (const auto& it : l->items) {
		if (it.host_name == name) {
			out = it;
			return OK;
		}
	}
	return ERROR_PATH_NOT_FOUND;
}

Error Volume::lookup(const uint32_t base_id, std::string_view path, Lookup& out,
                     const bool final_alias)
{
	out = Lookup{};
	fs::path dir;
	std::string rel;
	uint32_t dir_id = base_id;
	if (!path.empty() && path.front() == '\\') {
		dir_id = root_id_;
		path.remove_prefix(1);
	}
	if (const auto e = resolve_dir(dir_id, dir, rel); e != OK) {
		return e;
	}

	// Split into components
	std::vector<std::string> comps;
	size_t start = 0;
	while (start <= path.size()) {
		size_t end = path.find('\\', start);
		if (end == std::string_view::npos) {
			end = path.size();
		}
		const std::string c(path.substr(start, end - start));
		start = end + 1;
		if (c.empty() || c == ".") {
			continue;
		}
		if (c == "..") {
			if (!comps.empty()) {
				comps.pop_back();
			} else if (!rel.empty()) {
				// Climb above the base directory
				const auto slash = rel.rfind('/');
				rel = slash == std::string::npos ? "" : rel.substr(0, slash);
				dir = rel.empty() ? root_ : root_ / from_utf8(rel);
			}
			continue;
		}
		if (c.size() > 255) {
			return ERROR_PATH_TOO_LONG;
		}
		comps.push_back(c);
	}
	if (comps.empty()) {
		// The base directory itself
		const uint32_t id = id_for(rel);
		remember_dir(id, rel);
		out.exists = true;
		out.parent_id = id;
		out.parent = dir;
		out.parent_rel = rel;
		return dir_item(id, out.item);
	}

	for (size_t i = 0; i < comps.size(); ++i) {
		const bool final = i + 1 == comps.size();
		Error err = OK;
		const Listing* l = get_listing(dir, rel, err);
		if (!l) {
			return ERROR_PATH_NOT_FOUND;
		}
		char dos[DosNameLength];
		const bool is_dos = (!final || final_alias) && to_dos_name(comps[i], dos);
		const Item* found = nullptr;
		for (const auto& it : l->items) {
			if (iequals(it.long_name, comps[i]) || it.host_name == comps[i] ||
			    (is_dos && std::memcmp(it.dos_name, dos, DosNameLength) == 0)) {
				found = &it;
				break;
			}
		}
		if (final) {
			out.parent_id  = id_for(rel);
			out.parent     = dir;
			out.parent_rel = rel;
			out.last       = comps[i];
			remember_dir(out.parent_id, rel);
			if (found) {
				out.exists = true;
				out.item   = *found;
				refresh_item(out.item);
			}
			return OK;
		}
		if (!found || !found->is_dir) {
			return ERROR_PATH_NOT_FOUND;
		}
		dir = found->host;
		rel = found->rel;
	}
	return ERROR_PATH_NOT_FOUND; // not reached
}

Error Volume::make_host_name(const fs::path& parent, const std::string& parent_rel,
                             std::string_view geos_name, std::string& host_name)
{
	if (geos_name.empty() || geos_name.size() > LongNameLength ||
	    geos_name.find('\\') != std::string_view::npos) {
		return ERROR_INVALID_NAME;
	}
	Error err = OK;
	const Listing* l = get_listing(parent, parent_rel, err);
	if (!l) {
		return err;
	}
	host_name = encode_geos_name(geos_name);
	for (const auto& it : l->items) {
		if (iequals(it.long_name, geos_name) || iequals(it.host_name, host_name)) {
			return ERROR_FILE_EXISTS;
		}
	}
	std::error_code ec;
	if (fs::exists(parent / from_utf8(host_name), ec)) {
		return ERROR_FILE_EXISTS; // hidden entry with that name
	}
	return OK;
}

Error Volume::write_header(const fs::path& host, const uint8_t* header)
{
	std::FILE* f = open_file(host, "r+b");
	if (!f) {
		f = open_file(host, "w+b");
	}
	if (!f) {
		return ERROR_ACCESS_DENIED;
	}
	const bool ok = seek_file(f, 0) && std::fwrite(header, 1, HeaderSize, f) == HeaderSize;
	std::fclose(f);
	return ok ? OK : ERROR_WRITE_FAULT;
}

Error Volume::update_header_field(Item& item, const size_t offset, const uint8_t* value,
                                  const size_t size)
{
	if (!item.has_header) {
		if (!item.is_dir) {
			return ERROR_ATTR_NOT_SUPPORTED; // plain host file
		}
		// Directory attributes live in @DIRNAME.000, as on DOS drives
		std::memset(item.header, 0, HeaderSize);
		item.header[0] = 'G' | 0x80;
		item.header[1] = 'E';
		item.header[2] = 'A' | 0x80;
		item.header[3] = 'S';
		std::memcpy(item.header + GFH_longName, item.long_name,
		            strnlen(item.long_name, LongNameLength));
		put16(item.header + GFH_type, GFT_DIRECTORY);
		put16(item.header + GFH_protocol, 1);
		item.has_header = true;
	}
	std::memcpy(item.header + offset, value, size);
	const fs::path target = item.is_dir ? item.host / DirNameFile : item.host;
	// Write first: invalidate() re-reads the directory
	const Error e = write_header(target, item.header);
	invalidate(item.host.parent_path());
	return e;
}

std::vector<uint16_t> Volume::all_ext_attrs(const Item& item) const
{
	std::vector<uint16_t> a = {FEA_MODIFICATION, FEA_FILE_ATTR, FEA_SIZE, FEA_FILE_TYPE};
	if (item.has_header) {
		for (const auto x : {FEA_FLAGS, FEA_RELEASE, FEA_PROTOCOL, FEA_TOKEN, FEA_CREATOR,
		                     FEA_USER_NOTES, FEA_NOTICE, FEA_CREATION, FEA_DESKTOP_INFO}) {
			a.push_back(x);
		}
	}
	return a;
}

Error Volume::get_ext_attr(const Item& item, const uint16_t attr, const KernelContext& ctx,
                           AttrValue& out) const
{
	auto& v = out.bytes;
	v.clear();
	auto header_bytes = [&](const size_t off, const size_t n) -> Error {
		if (!item.has_header) {
			return ERROR_ATTR_NOT_FOUND;
		}
		v.assign(item.header + off, item.header + off + n);
		return OK;
	};
	switch (attr) {
	case FEA_MODIFICATION:
		put16(v, item.mdate);
		put16(v, item.mtime);
		return OK;
	case FEA_FILE_ATTR: v.push_back(item.attrs); return OK;
	case FEA_SIZE: put32(v, item.is_dir ? 0 : item.size); return OK;
	case FEA_FILE_TYPE: put16(v, item.file_type()); return OK;
	case FEA_FLAGS: return header_bytes(GFH_flags, 2);
	case FEA_RELEASE: return header_bytes(GFH_release, 8);
	case FEA_PROTOCOL: return header_bytes(GFH_protocol, 4);
	case FEA_TOKEN: return header_bytes(GFH_token, 6);
	case FEA_CREATOR: return header_bytes(GFH_creator, 6);
	case FEA_USER_NOTES: return header_bytes(GFH_userNotes, 100);
	case FEA_NOTICE: return header_bytes(GFH_notice, 32);
	case FEA_PASSWORD: return header_bytes(GFH_password, 8);
	case FEA_DESKTOP_INFO: return header_bytes(GFH_desktop, 16);
	case FEA_CREATION:
		if (item.has_header) {
			return header_bytes(GFH_created, 4);
		}
		put16(v, item.mdate);
		put16(v, item.mtime);
		return OK;
	case FEA_NAME: {
		const size_t n = strnlen(item.long_name, LongNameLength);
		v.assign(item.long_name, item.long_name + n);
		v.push_back(0);
		return OK;
	}
	case FEA_DOS_NAME: {
		std::string s;
		for (size_t i = 0; i < 8 && item.dos_name[i] != ' '; ++i) {
			s.push_back(item.dos_name[i]);
		}
		if (item.dos_name[8] != ' ') {
			s.push_back('.');
			for (size_t i = 8; i < DosNameLength && item.dos_name[i] != ' '; ++i) {
				s.push_back(item.dos_name[i]);
			}
		}
		v.assign(s.begin(), s.end());
		v.push_back(0);
		return OK;
	}
	case FEA_GEODE_ATTR: put16(v, item.geode_attrs); return OK;
	case FEA_FILE_ID: put32(v, item.id); return OK;
	case FEA_PATH_INFO: put16(v, ctx.path_info); return OK;
	case FEA_DRIVE_STATUS: put16(v, ctx.drive_status); return OK;
	case FEA_DISK: put16(v, ctx.disk_handle); return OK;
	default: return ERROR_ATTR_NOT_FOUND; // custom, owner, rights, links
	}
}

Error Volume::set_ext_attr(const uint32_t base_id, std::string_view path,
                           const uint16_t handle, const uint16_t attr,
                           const uint8_t* value, const uint16_t size)
{
	Item item;
	if (handle) {
		if (const auto e = handle_item(handle, item); e != OK) {
			return e;
		}
	} else {
		Lookup lk;
		if (const auto e = lookup(base_id, path, lk); e != OK) {
			return e;
		}
		if (!lk.exists) {
			return ERROR_FILE_NOT_FOUND;
		}
		item = lk.item;
	}

	struct Field {
		size_t offset, size;
		bool text; // shorter values allowed, rest zeroed
	};
	Field f{};
	switch (attr) {
	case FEA_MODIFICATION: {
		if (size != 4) {
			return ERROR_ATTR_SIZE_MISMATCH;
		}
		std::error_code ec;
		fs::last_write_time(item.host, from_date_time(get16(value), get16(value + 2)), ec);
		invalidate(item.host.parent_path());
		return ec ? ERROR_ACCESS_DENIED : OK;
	}
	case FEA_FILE_ATTR:
		if (size != 1) {
			return ERROR_ATTR_SIZE_MISMATCH;
		}
		{
			std::error_code ec;
			fs::permissions(item.host, fs::perms::owner_write,
			                (value[0] & FA_RDONLY) ? fs::perm_options::remove
			                                       : fs::perm_options::add,
			                ec);
			invalidate(item.host.parent_path());
			return ec ? ERROR_ACCESS_DENIED : OK;
		}
	case FEA_FILE_TYPE: f = {GFH_type, 2, false}; break;
	case FEA_FLAGS: f = {GFH_flags, 2, false}; break;
	case FEA_RELEASE: f = {GFH_release, 8, false}; break;
	case FEA_PROTOCOL: f = {GFH_protocol, 4, false}; break;
	case FEA_TOKEN: f = {GFH_token, 6, false}; break;
	case FEA_CREATOR: f = {GFH_creator, 6, false}; break;
	case FEA_USER_NOTES: f = {GFH_userNotes, 100, true}; break;
	case FEA_NOTICE: f = {GFH_notice, 32, true}; break;
	case FEA_CREATION: f = {GFH_created, 4, false}; break;
	case FEA_PASSWORD: f = {GFH_password, 8, true}; break;
	case FEA_DESKTOP_INFO: f = {GFH_desktop, 16, true}; break;
	case FEA_NAME:
	case FEA_SIZE:
	case FEA_FILE_ID:
	case FEA_DOS_NAME:
	case FEA_GEODE_ATTR:
	case FEA_PATH_INFO:
	case FEA_DRIVE_STATUS:
	case FEA_DISK: return ERROR_ATTR_CANNOT_BE_SET;
	default: return ERROR_ATTR_NOT_SUPPORTED;
	}
	if (!item.has_header && !item.is_dir) {
		return ERROR_ATTR_NOT_SUPPORTED; // plain host file
	}
	if (size > f.size || (!f.text && size != f.size)) {
		return ERROR_ATTR_SIZE_MISMATCH;
	}
	uint8_t buf[100] = {};
	std::memcpy(buf, value, size);
	return update_header_field(item, f.offset, buf, f.size);
}

Error Volume::make_dir(const uint32_t base_id, std::string_view path, Item& created)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk, false); e != OK) {
		return e;
	}
	if (lk.exists) {
		return ERROR_FILE_EXISTS;
	}
	std::string host_name;
	if (const auto e = make_host_name(lk.parent, lk.parent_rel, lk.last, host_name); e != OK) {
		return e;
	}
	std::error_code ec;
	if (!fs::create_directory(lk.parent / from_utf8(host_name), ec)) {
		return ERROR_ACCESS_DENIED;
	}
	invalidate(lk.parent);
	Lookup again;
	lookup(lk.parent_id, lk.last, again);
	created = again.item;
	return OK;
}

Error Volume::remove_dir(const uint32_t base_id, std::string_view path, uint32_t& id)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk); e != OK) {
		return e;
	}
	if (!lk.exists || !lk.item.is_dir) {
		return ERROR_PATH_NOT_FOUND;
	}
	if (lk.item.id == root_id_) {
		return ERROR_ACCESS_DENIED;
	}
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(lk.item.host, ec)) {
		if (!iequals(utf8_name(e.path()), DirNameFile)) {
			return ERROR_DIRECTORY_NOT_EMPTY;
		}
	}
	fs::remove(lk.item.host / DirNameFile, ec);
	if (!fs::remove(lk.item.host, ec)) {
		return ERROR_ACCESS_DENIED;
	}
	invalidate(lk.parent);
	dir_ids_.erase(lk.item.id);
	id = lk.item.id;
	return OK;
}

Error Volume::remove_file(const uint32_t base_id, std::string_view path, uint32_t& id)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk); e != OK) {
		return e;
	}
	if (!lk.exists) {
		return ERROR_FILE_NOT_FOUND;
	}
	if (lk.item.is_dir) {
		return ERROR_ACCESS_DENIED;
	}
	if (is_open(lk.item.host)) {
		return ERROR_FILE_IN_USE;
	}
	if (lk.item.attrs & FA_RDONLY) {
		return ERROR_ACCESS_DENIED;
	}
	std::error_code ec;
	if (!fs::remove(lk.item.host, ec)) {
		return ERROR_ACCESS_DENIED;
	}
	invalidate(lk.parent);
	id = lk.item.id;
	return OK;
}

Error Volume::rename(const uint32_t base_id, std::string_view path,
                     std::string_view new_name, Item& renamed)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk); e != OK) {
		return e;
	}
	if (!lk.exists) {
		return ERROR_FILE_NOT_FOUND;
	}
	if (lk.item.id == root_id_) {
		return ERROR_ACCESS_DENIED;
	}
	if (!lk.item.is_dir && is_open(lk.item.host)) {
		return ERROR_FILE_IN_USE;
	}
	if (iequals(lk.item.long_name, new_name) && !lk.item.has_header &&
	    decode_host_name(lk.item.host_name) == new_name) {
		renamed = lk.item;
		return OK;
	}
	std::string host_name;
	Error e = make_host_name(lk.parent, lk.parent_rel, new_name, host_name);
	if (e == ERROR_FILE_EXISTS && iequals(lk.item.long_name, new_name)) {
		e = OK; // only the case changes
		host_name = encode_geos_name(new_name);
	}
	if (e != OK) {
		return e;
	}
	std::error_code ec;
	const fs::path target = lk.parent / from_utf8(host_name);
	fs::rename(lk.item.host, target, ec);
	if (ec) {
		return ERROR_ACCESS_DENIED;
	}
	invalidate(lk.parent);
	Item moved = lk.item;
	moved.host = target;
	moved.host_name = host_name;
	if (moved.has_header) {
		uint8_t name[36] = {};
		std::memcpy(name, new_name.data(), new_name.size());
		update_header_field(moved, GFH_longName, name, sizeof(name));
	}
	if (moved.is_dir) {
		dir_ids_.erase(lk.item.id);
	}
	Lookup again;
	lookup(lk.parent_id, new_name, again);
	renamed = again.item;
	return OK;
}

Error Volume::move(const uint32_t base_id, std::string_view path,
                   const uint32_t dest_base_id, std::string_view dest_path, Item& moved)
{
	Lookup src, dst;
	if (const auto e = lookup(base_id, path, src); e != OK) {
		return e;
	}
	if (!src.exists) {
		return ERROR_FILE_NOT_FOUND;
	}
	if (!src.item.is_dir && is_open(src.item.host)) {
		return ERROR_FILE_IN_USE;
	}
	if (const auto e = lookup(dest_base_id, dest_path, dst, false); e != OK) {
		return e;
	}
	if (dst.exists) {
		return ERROR_FILE_EXISTS;
	}
	// No moving a directory into itself
	if (src.item.is_dir &&
	    (dst.parent_rel == src.item.rel ||
	     dst.parent_rel.rfind(src.item.rel + "/", 0) == 0)) {
		return ERROR_ACCESS_DENIED;
	}
	std::string host_name;
	if (const auto e = make_host_name(dst.parent, dst.parent_rel, dst.last, host_name); e != OK) {
		return e;
	}
	std::error_code ec;
	const fs::path target = dst.parent / from_utf8(host_name);
	fs::rename(src.item.host, target, ec);
	if (ec) {
		return ERROR_ACCESS_DENIED;
	}
	invalidate(src.parent);
	invalidate(dst.parent);
	Item m = src.item;
	m.host = target;
	if (m.has_header && !iequals(m.long_name, dst.last)) {
		uint8_t name[36] = {};
		std::memcpy(name, dst.last.data(), std::min<size_t>(dst.last.size(), 32));
		update_header_field(m, GFH_longName, name, sizeof(name));
	}
	if (m.is_dir) {
		dir_ids_.erase(src.item.id);
	}
	Lookup again;
	lookup(dst.parent_id, dst.last, again);
	moved = again.item;
	return OK;
}

Error Volume::get_attrs(const uint32_t base_id, std::string_view path, uint8_t& attrs)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk); e != OK) {
		return e;
	}
	if (!lk.exists) {
		return ERROR_FILE_NOT_FOUND;
	}
	attrs = lk.item.attrs;
	return OK;
}

Error Volume::set_attrs(const uint32_t base_id, std::string_view path, const uint8_t attrs)
{
	const uint8_t v = attrs;
	return set_ext_attr(base_id, path, 0, FEA_FILE_ATTR, &v, 1);
}

/* ----------------------------------------------------------------
 * Open files
 * ---------------------------------------------------------------- */

Volume::OpenFile* Volume::file(const uint16_t handle)
{
	if (handle == 0 || handle > files_.size()) {
		return nullptr;
	}
	return files_[handle - 1].get();
}

bool Volume::is_open(const fs::path& host) const
{
	for (const auto& f : files_) {
		if (f && f->host == host) {
			return true;
		}
	}
	return false;
}

Error Volume::open_host(const Item& item, const uint8_t access, const bool create_new,
                        uint16_t& handle)
{
	const uint8_t mode = access & FFAF_MODE;
	const bool writable = mode != 0;
	std::FILE* f = open_file(item.host, create_new ? "w+b" : (writable ? "r+b" : "rb"));
	if (!f) {
		return ERROR_ACCESS_DENIED;
	}
	auto of      = std::make_unique<OpenFile>();
	of->f        = f;
	of->host     = item.host;
	of->rel      = item.rel;
	of->id       = item.id;
	of->skip     = (item.has_header && !(access & FFAF_RAW)) ? HeaderSize : 0;
	of->writable = writable;

	for (size_t i = 0; i < files_.size(); ++i) {
		if (!files_[i]) {
			files_[i] = std::move(of);
			handle    = static_cast<uint16_t>(i + 1);
			return OK;
		}
	}
	if (files_.size() >= 0xfff0) {
		std::fclose(f);
		return ERROR_TOO_MANY_OPEN_FILES;
	}
	files_.push_back(std::move(of));
	handle = static_cast<uint16_t>(files_.size());
	return OK;
}

Error Volume::open(const uint32_t base_id, std::string_view path, const uint8_t access,
                   uint16_t& handle, Item& item)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk); e != OK) {
		return e;
	}
	if (!lk.exists) {
		return ERROR_FILE_NOT_FOUND;
	}
	if (lk.item.is_dir) {
		return ERROR_ACCESS_DENIED;
	}
	if ((access & FFAF_MODE) != 0 && (lk.item.attrs & FA_RDONLY)) {
		return ERROR_ACCESS_DENIED;
	}
	item = lk.item;
	return open_host(item, access, false, handle);
}

Error Volume::create(const uint32_t base_id, std::string_view path, const uint8_t attrs,
                     const uint8_t create_flags, const uint8_t access, uint16_t& handle,
                     Item& item)
{
	Lookup lk;
	if (const auto e = lookup(base_id, path, lk, false); e != OK) {
		return e;
	}
	if (lk.exists) {
		return ERROR_FILE_EXISTS;
	}
	std::string host_name;
	if (const auto e = make_host_name(lk.parent, lk.parent_rel, lk.last, host_name); e != OK) {
		return e;
	}
	Item it;
	it.host      = lk.parent / from_utf8(host_name);
	it.host_name = host_name;
	it.rel       = join_rel(lk.parent_rel, host_name);
	it.id        = id_for(it.rel);
	it.attrs     = attrs & ~FA_SUBDIR;
	std::memcpy(it.long_name, lk.last.data(), std::min<size_t>(lk.last.size(), LongNameLength));

	// A GEOS file gets a GEOS header, like on a DOS drive
	const bool native = (create_flags & FCF_NATIVE) != 0;
	if (!native) {
		std::memset(it.header, 0, HeaderSize);
		it.header[0] = 'G' | 0x80;
		it.header[1] = 'E';
		it.header[2] = 'A' | 0x80;
		it.header[3] = 'S';
		std::memcpy(it.header + GFH_longName, lk.last.data(),
		            std::min<size_t>(lk.last.size(), LongNameLength));
		put16(it.header + GFH_type, GFT_DATA);
		put16(it.header + GFH_protocol, 1);
		uint16_t d = 0, t = 0;
		now_date_time(d, t);
		put16(it.header + GFH_created, d);
		put16(it.header + GFH_created + 2, t);
		it.has_header = true;
	}
	if (const auto e = open_host(it, access | 0x02, true, handle); e != OK) {
		return e;
	}
	OpenFile* of = file(handle);
	if (it.has_header &&
	    std::fwrite(it.header, 1, HeaderSize, of->f) != HeaderSize) {
		bool closed = false, dirty = false;
		uint32_t id = 0;
		close(handle, closed, dirty, id);
		return ERROR_WRITE_FAULT;
	}
	of->writable = (access & FFAF_MODE) != 0;
	of->dirty    = true;
	std::fflush(of->f); // so the directory read below sees the header
	invalidate(lk.parent);
	item = it;
	return OK;
}

Error Volume::read(const uint16_t handle, uint8_t* buf, const uint16_t count, uint16_t& got)
{
	OpenFile* of = file(handle);
	got          = 0;
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	if (count == 0) {
		return OK;
	}
	if (!seek_file(of->f, static_cast<int64_t>(of->skip) + of->pos)) {
		return ERROR_READ_FAULT;
	}
	const size_t n = std::fread(buf, 1, count, of->f);
	if (n < count && std::ferror(of->f)) {
		std::clearerr(of->f);
		return ERROR_READ_FAULT;
	}
	std::clearerr(of->f);
	got = static_cast<uint16_t>(n);
	of->pos += got;
	return OK;
}

Error Volume::write(const uint16_t handle, const uint8_t* buf, const uint16_t count,
                    uint16_t& put)
{
	OpenFile* of = file(handle);
	put          = 0;
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	if (!of->writable) {
		return ERROR_ACCESS_DENIED;
	}
	if (count == 0) {
		return OK;
	}
	if (!seek_file(of->f, static_cast<int64_t>(of->skip) + of->pos)) {
		return ERROR_WRITE_FAULT;
	}
	const size_t n = std::fwrite(buf, 1, count, of->f);
	put = static_cast<uint16_t>(n);
	of->pos += put;
	of->dirty = true;
	if (n < count) {
		std::clearerr(of->f);
		return ERROR_SHORT_READ_WRITE;
	}
	return OK;
}

Error Volume::file_size(const uint16_t handle, uint32_t& size)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	std::fflush(of->f);
	std::error_code ec;
	const auto s = fs::file_size(of->host, ec);
	if (ec) {
		return ERROR_GENERAL_FAILURE;
	}
	size = static_cast<uint32_t>(std::min<uintmax_t>(s > of->skip ? s - of->skip : 0, 0xffffffffu));
	return OK;
}

Error Volume::seek(const uint16_t handle, const uint8_t mode, const int32_t offset,
                   uint32_t& pos)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	int64_t base = 0;
	if (mode == FILE_POS_RELATIVE) {
		base = of->pos;
	} else if (mode == FILE_POS_END) {
		uint32_t size = 0;
		if (const auto e = file_size(handle, size); e != OK) {
			return e;
		}
		base = size;
	}
	int64_t np = base + offset;
	if (mode == FILE_POS_START) {
		np = static_cast<uint32_t>(offset); // absolute positions are unsigned
	}
	of->pos = static_cast<uint32_t>(std::clamp<int64_t>(np, 0, 0xffffffffll));
	pos     = of->pos;
	return OK;
}

Error Volume::truncate(const uint16_t handle, const uint32_t size)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	if (!of->writable) {
		return ERROR_ACCESS_DENIED;
	}
	std::fflush(of->f);
	std::error_code ec;
	fs::resize_file(of->host, static_cast<uintmax_t>(of->skip) + size, ec);
	if (ec) {
		return ERROR_WRITE_FAULT;
	}
	of->dirty = true;
	return OK;
}

Error Volume::commit(const uint16_t handle)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	return std::fflush(of->f) == 0 ? OK : ERROR_WRITE_FAULT;
}

Error Volume::get_date_time(const uint16_t handle, uint16_t& date, uint16_t& time)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	std::fflush(of->f);
	std::error_code ec;
	const auto t = fs::last_write_time(of->host, ec);
	if (ec) {
		return ERROR_GENERAL_FAILURE;
	}
	to_date_time(t, date, time);
	return OK;
}

Error Volume::set_date_time(const uint16_t handle, const uint16_t date, const uint16_t time)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	std::fflush(of->f);
	std::error_code ec;
	fs::last_write_time(of->host, from_date_time(date, time), ec);
	return ec ? ERROR_ACCESS_DENIED : OK;
}

Error Volume::add_ref(const uint16_t handle)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	++of->refs;
	return OK;
}

Error Volume::is_dirty(const uint16_t handle, bool& dirty)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	dirty = of->dirty;
	return OK;
}

Error Volume::close(const uint16_t handle, bool& closed, bool& was_dirty, uint32_t& id)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	id        = of->id;
	was_dirty = of->dirty;
	closed    = --of->refs <= 0;
	if (closed) {
		std::fclose(of->f);
		invalidate(of->host.parent_path());
		files_[handle - 1].reset();
	}
	return OK;
}

Error Volume::handle_item(const uint16_t handle, Item& item)
{
	OpenFile* of = file(handle);
	if (!of) {
		return ERROR_GENERAL_FAILURE;
	}
	std::fflush(of->f);
	const auto slash = of->rel.rfind('/');
	const std::string parent_rel = slash == std::string::npos ? "" : of->rel.substr(0, slash);
	Error err = OK;
	const Listing* l = get_listing(of->host.parent_path(), parent_rel, err);
	if (!l) {
		return err;
	}
	for (const auto& it : l->items) {
		if (it.host == of->host) {
			item = it;
			refresh_item(item);
			return OK;
		}
	}
	return ERROR_FILE_NOT_FOUND;
}

/* ----------------------------------------------------------------
 * Enumeration
 * ---------------------------------------------------------------- */

Error Volume::enum_begin(const uint32_t dir_id, uint16_t& enum_handle)
{
	std::vector<Item> items;
	if (const auto e = list(dir_id, items); e != OK) {
		return e;
	}
	if (items.empty()) {
		return ERROR_NO_MORE_FILES;
	}
	std::reverse(items.begin(), items.end()); // pop from the back
	if (next_enum_ == 0) {
		next_enum_ = 1;
	}
	enum_handle = next_enum_++;
	enums_[enum_handle] = std::move(items);
	return OK;
}

Error Volume::enum_next(const uint16_t enum_handle, Item& item)
{
	const auto it = enums_.find(enum_handle);
	if (it == enums_.end() || it->second.empty()) {
		enums_.erase(enum_handle);
		return ERROR_NO_MORE_FILES;
	}
	item = std::move(it->second.back());
	it->second.pop_back();
	return OK;
}

void Volume::enum_end(const uint16_t enum_handle)
{
	enums_.erase(enum_handle);
}

} // namespace HostFs
