// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file hostfs_lfn.cpp
 * @brief Long file name support for the GEOS DOS fast path, host side core.
 */

#include "hostfs_lfn.h"

#include "hostfs_volume.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

namespace HostLfn {

namespace {

std::string upper_ascii(std::string s)
{
	for (auto& c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

bool iequals_ascii(std::string_view a, std::string_view b)
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

std::string utf8_of(const fs::path& name)
{
	const auto u8 = name.u8string();
	return std::string(u8.begin(), u8.end());
}

void put16(uint8_t* p, const uint16_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, const uint32_t v)
{
	put16(p, static_cast<uint16_t>(v));
	put16(p + 2, static_cast<uint16_t>(v >> 16));
}

bool chars_equal(const char a, const char b)
{
	return HostFs::geos_iequals(std::string_view(&a, 1), std::string_view(&b, 1));
}

bool glob(std::string_view p, std::string_view n)
{
	// iterative wildcard match with backtracking on the last '*'
	size_t pi = 0, ni = 0, star = std::string_view::npos, mark = 0;
	while (ni < n.size()) {
		if (pi < p.size() && (p[pi] == '?' || (p[pi] != '*' && chars_equal(p[pi], n[ni])))) {
			++pi;
			++ni;
		} else if (pi < p.size() && p[pi] == '*') {
			star = pi++;
			mark = ni;
		} else if (star != std::string_view::npos) {
			pi = star + 1;
			ni = ++mark;
		} else {
			return false;
		}
	}
	while (pi < p.size() && p[pi] == '*') {
		++pi;
	}
	return pi == p.size();
}

} // namespace

fs::path from_utf8(const std::string& utf8)
{
	return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

fs::path from_native(const std::string& native)
{
	// POSIX: the bytes themselves; Windows: interpreted in the ANSI code
	// page, which is how DOSBox's DOS core produced them
	return fs::path(native);
}

std::string native_name(const fs::path& name)
{
#ifdef _WIN32
	try {
		const std::string s = name.string(); // ANSI code page
		if (s.find('?') != std::string::npos || fs::path(s) != name) {
			return {}; // not representable: DOSBox can't reach it
		}
		return s;
	} catch (const std::exception&) {
		return {};
	}
#else
	return name.string();
#endif
}

std::vector<Entry> list_dir(const fs::path& dir, const ShortNameFn& short_name)
{
	std::vector<Entry> entries;
	std::error_code ec;
	fs::directory_iterator it(dir, ec);
	if (ec) {
		return entries;
	}
	for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
		const auto& de       = *it;
		const fs::path fname = de.path().filename();
		Entry e;
		e.host_name = utf8_of(fname);
		if (e.host_name.empty() || e.host_name.front() == '.') {
			continue; // hidden, as in hostfs.geo
		}
		e.native_name = native_name(fname);
		if (e.native_name.empty()) {
			continue;
		}
		std::error_code eec;
		e.is_dir     = de.is_directory(eec);
		e.short_name = short_name(e.native_name, e.is_dir);
		if (e.short_name.empty()) {
			continue;
		}
		if (e.is_dir) {
			e.attrs = A_SUBDIR;
		} else {
			e.attrs        = A_ARCHIVE;
			const auto len = de.file_size(eec);
			e.size = eec ? 0 : static_cast<uint32_t>(std::min<uintmax_t>(len, 0xffffffffu));
		}
		const auto st = de.status(eec);
		if (!eec && (st.permissions() & fs::perms::owner_write) == fs::perms::none) {
			e.attrs |= A_RDONLY;
		}
		const auto mt = de.last_write_time(eec);
		if (!eec) {
			HostFs::dos_date_time(mt, e.date, e.time);
		}
		entries.push_back(std::move(e));
	}

	// GEOS names: the host file system's rules, in its order
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		return upper_ascii(a.host_name) < upper_ascii(b.host_name);
	});
	std::vector<HostFs::Item> items(entries.size());
	for (size_t i = 0; i < entries.size(); ++i) {
		items[i].host_name = entries[i].host_name;
		items[i].is_dir    = entries[i].is_dir;
	}
	HostFs::assign_names(items);
	for (size_t i = 0; i < entries.size(); ++i) {
		entries[i].geos_name = items[i].long_name;
	}
	return entries;
}

const Entry* find_entry(const std::vector<Entry>& entries, std::string_view comp)
{
	for (const auto& e : entries) {
		if (HostFs::geos_iequals(e.geos_name, comp)) {
			return &e;
		}
	}
	for (const auto& e : entries) {
		if (HostFs::geos_iequals(HostFs::decode_host_name(e.host_name), comp)) {
			return &e;
		}
	}
	for (const auto& e : entries) {
		if (iequals_ascii(e.short_name, comp)) {
			return &e;
		}
	}
	return nullptr;
}

bool has_wildcards(std::string_view s)
{
	return s.find_first_of("*?") != std::string_view::npos;
}

bool match_pattern(std::string_view pattern, std::string_view name)
{
	if (pattern == "*" || pattern == "*.*") {
		return true;
	}
	return glob(pattern, name);
}

bool entry_matches(const Entry& e, std::string_view pattern)
{
	return match_pattern(pattern, e.geos_name) || match_pattern(pattern, e.short_name) ||
	       match_pattern(pattern, HostFs::decode_host_name(e.host_name));
}

bool attrs_match(const uint8_t attrs, const uint8_t allowed, const uint8_t required)
{
	constexpr uint8_t Special = A_HIDDEN | A_SYSTEM | A_SUBDIR;
	return (attrs & Special & ~allowed) == 0 && (attrs & required) == required;
}

bool split_path(std::string_view path, SplitPath& out)
{
	out = {};
	if (path.empty() || path.size() >= MaxPath) {
		return false;
	}
	if (path.size() >= 2 && path[1] == ':' &&
	    std::isalpha(static_cast<unsigned char>(path[0]))) {
		out.drive = std::toupper(static_cast<unsigned char>(path[0])) - 'A';
		path.remove_prefix(2);
	}
	if (!path.empty() && (path.front() == '\\' || path.front() == '/')) {
		out.absolute = true;
	}
	size_t start = 0;
	while (start <= path.size()) {
		const size_t end  = path.find_first_of("\\/", start);
		const auto stop   = end == std::string_view::npos ? path.size() : end;
		const auto comp   = path.substr(start, stop - start);
		if (!comp.empty()) {
			if (comp.size() > MaxName) {
				return false;
			}
			out.comps.emplace_back(comp);
		}
		if (end == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return out.drive >= 0 || out.absolute || !out.comps.empty();
}

bool valid_new_name(std::string_view name)
{
	if (name.empty() || name.size() > MaxName || name == "." || name == "..") {
		return false;
	}
	for (const char c : name) {
		if (static_cast<unsigned char>(c) < 0x20 || std::strchr("\\/:*?\"<>|", c)) {
			return false;
		}
	}
	return name.find_first_not_of(". ") != std::string_view::npos;
}

void pack_find_data(const Entry& e, uint8_t (&out)[FindDataSize])
{
	std::memset(out, 0, sizeof(out));
	put32(out + 0, e.attrs);
	for (const size_t off : {size_t{4}, size_t{12}, size_t{20}}) { // created, accessed, modified
		put16(out + off, e.time);
		put16(out + off + 2, e.date);
	}
	put32(out + 28, 0);      // size, high dword
	put32(out + 32, e.size); // size, low dword
	const size_t n = std::min(e.geos_name.size(), MaxPath - 1);
	std::memcpy(out + 44, e.geos_name.data(), n);
	if (e.geos_name != e.short_name) {
		const size_t s = std::min<size_t>(e.short_name.size(), 13);
		std::memcpy(out + 44 + MaxPath, e.short_name.data(), s);
	}
}

} // namespace HostLfn
