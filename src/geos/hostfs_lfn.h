// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file hostfs_lfn.h
 * @brief Long file name support for the GEOS DOS fast path, host side core.
 *
 * Everything the LFN calls of the DOS fast path (geoshost_fs.cpp, protocol
 * in geoshost_fs.h) need that doesn't touch DOSBox: listing a host
 * directory with GEOS names, matching path components and wildcards,
 * splitting paths and filling WIN32_FIND_DATA. The DOS short names come
 * from DOSBox's directory cache through a callback, so this file has no
 * DOSBox dependencies and is unit-tested.
 *
 * Names follow the host file system's rules (hostfs_volume.h): GEOS names
 * are the decoded host names, at most 32 characters, longer ones cut with
 * "~" plus a hash. Unlike hostfs.geo, @DIRNAME.000 is listed (the DOS IFS
 * driver keeps its directory names there itself).
 */

#ifndef DOSBOX_HOSTFS_LFN_H
#define DOSBOX_HOSTFS_LFN_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace HostLfn {

namespace fs = std::filesystem;

constexpr size_t MaxPath      = 260; // incl. drive, leading '\' and NUL
constexpr size_t MaxName      = 255;
constexpr size_t FindDataSize = 318; // WIN32_FIND_DATA, OEM variant

// FileAttrs
constexpr uint8_t A_RDONLY  = 0x01;
constexpr uint8_t A_HIDDEN  = 0x02;
constexpr uint8_t A_SYSTEM  = 0x04;
constexpr uint8_t A_SUBDIR  = 0x10;
constexpr uint8_t A_ARCHIVE = 0x20;

struct Entry {
	std::string host_name   = {}; // UTF-8
	std::string native_name = {}; // as DOSBox's DOS core sees it
	std::string geos_name   = {}; // GEOS characters, <= 32
	std::string short_name  = {}; // DOS 8.3 name, "NAME.EXT"
	bool is_dir             = false;
	uint8_t attrs           = 0;
	uint32_t size           = 0;
	uint16_t date = 0, time = 0;
};

/**
 * DOS short name of a host entry, from DOSBox's directory cache; empty if
 * DOS can't reach it (the entry is then not listed).
 */
using ShortNameFn = std::function<std::string(const std::string& native_name, bool is_dir)>;

/**
 * The file name as DOSBox's DOS core sees it: UTF-8 bytes on POSIX hosts,
 * the ANSI code page on Windows. Empty if it can't be represented there.
 */
std::string native_name(const fs::path& name);

/** Host path of a native name (inverse of native_name). */
fs::path from_native(const std::string& native);

/** Host path of a UTF-8 name. */
fs::path from_utf8(const std::string& utf8);

/** All reachable entries of a host directory, with GEOS and short names. */
std::vector<Entry> list_dir(const fs::path& dir, const ShortNameFn& short_name);

/**
 * Entry for a path component given by GEOS: its GEOS name, the full
 * decoded host name, or the DOS short name; nullptr if none matches.
 */
const Entry* find_entry(const std::vector<Entry>& entries, std::string_view comp);

bool has_wildcards(std::string_view s);

/**
 * LFN wildcard match ('*' any run, '?' one character, "*.*" matches all)
 * against a GEOS name, case-insensitive in the GEOS character set.
 */
bool match_pattern(std::string_view pattern, std::string_view name);

/**
 * True if a find pattern matches the entry in any of its name forms: the
 * GEOS name shown, the full decoded host name, or the DOS short name.
 */
bool entry_matches(const Entry& e, std::string_view pattern);

/** True if the entry passes the find attribute masks (CL, CH of 714Eh). */
bool attrs_match(uint8_t attrs, uint8_t allowed, uint8_t required);

struct SplitPath {
	int drive     = -1;    // 0 = A:, -1 = none given
	bool absolute = false; // starts with '\' or '/'
	std::vector<std::string> comps = {};
};

/** Split a GEOS path. False if it is empty, too long or malformed. */
bool split_path(std::string_view path, SplitPath& out);

/** Name GEOS may create: not empty, no wildcards or separators, <= 255. */
bool valid_new_name(std::string_view name);

/** Fill WIN32_FIND_DATA (DOS date/time format, SI = 1). */
void pack_find_data(const Entry& e, uint8_t (&out)[FindDataSize]);

} // namespace HostLfn

#endif // DOSBOX_HOSTFS_LFN_H
