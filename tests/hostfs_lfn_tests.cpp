// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Tests for HostLfn, the host side core of the DOS fast path's long file
 * name support.
 */

#include "geos/hostfs_lfn.h"
#include "geos/hostfs_volume.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace HostLfn;

namespace {

// Stand-in for DOSBox's directory cache: 8.3 names as they are,
// others "XXXXXX~N.EXT" in order of appearance, like DOSBox does
struct FakeCache {
	int counter = 0;
	std::string operator()(const std::string& native, bool)
	{
		const auto dot = native.rfind('.');
		std::string base = native.substr(0, dot);
		std::string ext  = dot == std::string::npos ? "" : native.substr(dot + 1);
		const bool plain = base.size() <= 8 && ext.size() <= 3 && !base.empty() &&
		                   native.find(' ') == std::string::npos &&
		                   std::all_of(native.begin(), native.end(), [](char c) {
			                   return static_cast<unsigned char>(c) < 0x80;
		                   });
		auto up = [](std::string s) {
			for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return s;
		};
		if (plain) {
			return up(native);
		}
		std::string b;
		for (char c : base) {
			if (std::isalnum(static_cast<unsigned char>(c)) && b.size() < 6) b += c;
		}
		b = up(b) + "~" + std::to_string(++counter);
		ext = up(ext.substr(0, 3));
		return ext.empty() ? b : b + "." + ext;
	}
};

class HostLfnTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		std::random_device rd;
		root_ = fs::temp_directory_path() / ("hostlfn_test_" + std::to_string(rd()));
		fs::create_directories(root_ / "Sub Folder");
		touch("readme.txt");
		touch("A rather long document name.txt");
		touch(".hidden");
		touch("@DIRNAME.000");
		touch(u8"Gr\u00FC\u00DFe.txt");
	}
	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(root_, ec);
	}
	void touch(const std::u8string& n)
	{
		std::ofstream(root_ / fs::path(n)) << "x";
	}
	void touch(const char* n)
	{
		std::ofstream(root_ / n) << "x";
	}
	std::vector<Entry> list()
	{
		FakeCache cache;
		return list_dir(root_, [&](const std::string& n, bool d) { return cache(n, d); });
	}
	fs::path root_;
};

} // namespace

TEST_F(HostLfnTest, ListsWithGeosAndShortNames)
{
	const auto entries = list();
	EXPECT_EQ(entries.size(), 5u); // ".hidden" is not listed
	const Entry* e = find_entry(entries, "A rather long document name.txt");
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->short_name.substr(0, 7), "ARATHE~");
	EXPECT_FALSE(e->is_dir);
	EXPECT_EQ(e->size, 1u);
	EXPECT_NE(e->date, 0);

	const Entry* d = find_entry(entries, "sub folder"); // case-insensitive
	ASSERT_NE(d, nullptr);
	EXPECT_TRUE(d->is_dir);
	EXPECT_EQ(d->attrs & A_SUBDIR, A_SUBDIR);

	// @DIRNAME.000 stays visible for the DOS IFS driver
	EXPECT_NE(find_entry(entries, "@DIRNAME.000"), nullptr);
}

TEST_F(HostLfnTest, FindsByAnyForm)
{
	const auto entries = list();
	const Entry* byGeos = find_entry(entries, "Gr\x9F\xA7" "e.txt"); // GEOS chars
	ASSERT_NE(byGeos, nullptr);
	const Entry* byShort = find_entry(entries, byGeos->short_name);
	EXPECT_EQ(byShort, byGeos);
	EXPECT_EQ(find_entry(entries, "GR\x86\xA7" "E.TXT"), byGeos); // GEOS case folding
	EXPECT_EQ(find_entry(entries, "nothing.txt"), nullptr);
}

TEST_F(HostLfnTest, LongHostNamesUseTheHostfsRule)
{
	const std::string longName = "An extremely long host file name beyond the GEOS limit.document";
	touch(longName.c_str());
	const auto entries = list();
	const Entry* e = find_entry(entries, longName); // full host name works
	ASSERT_NE(e, nullptr);
	EXPECT_LE(e->geos_name.size(), 32u);
	EXPECT_NE(e->geos_name.find('~'), std::string::npos);
	EXPECT_EQ(find_entry(entries, e->geos_name), e); // and the GEOS name

	// find patterns match the full host name too, not only the cut name
	EXPECT_TRUE(entry_matches(*e, longName));
	EXPECT_TRUE(entry_matches(*e, "*beyond the GEOS*"));
	EXPECT_TRUE(entry_matches(*e, e->geos_name));
	EXPECT_TRUE(entry_matches(*e, e->short_name));
	EXPECT_FALSE(entry_matches(*e, "other*"));
}

TEST(HostLfnNames, Wildcards)
{
	EXPECT_TRUE(match_pattern("*.*", "noext"));
	EXPECT_TRUE(match_pattern("*", "a.b.c"));
	EXPECT_TRUE(match_pattern("*.txt", "Long Name.TXT"));
	EXPECT_TRUE(match_pattern("gr?\xA7" "e.*", "Gr\x9F\xA7" "e.txt"));
	EXPECT_TRUE(match_pattern("*name*", "A long NAME here"));
	EXPECT_FALSE(match_pattern("*.doc", "file.txt"));
	EXPECT_FALSE(match_pattern("a?", "a"));
	EXPECT_TRUE(has_wildcards("a*"));
	EXPECT_FALSE(has_wildcards("plain"));
}

TEST(HostLfnNames, AttributeMasks)
{
	EXPECT_TRUE(attrs_match(A_ARCHIVE, 0, 0));
	EXPECT_FALSE(attrs_match(A_SUBDIR, 0, 0));            // dirs need CL bit
	EXPECT_TRUE(attrs_match(A_SUBDIR, A_SUBDIR, 0));
	EXPECT_FALSE(attrs_match(A_ARCHIVE, A_SUBDIR, A_SUBDIR)); // CH: dirs only
	EXPECT_TRUE(attrs_match(A_SUBDIR | A_HIDDEN, A_SUBDIR | A_HIDDEN, A_SUBDIR));
}

TEST(HostLfnNames, SplitPath)
{
	SplitPath sp;
	ASSERT_TRUE(split_path("C:\\Long Folder\\Grüße.txt", sp));
	EXPECT_EQ(sp.drive, 2);
	EXPECT_TRUE(sp.absolute);
	ASSERT_EQ(sp.comps.size(), 2u);
	EXPECT_EQ(sp.comps[0], "Long Folder");

	ASSERT_TRUE(split_path("sub//dir/file", sp));
	EXPECT_EQ(sp.drive, -1);
	EXPECT_FALSE(sp.absolute);
	EXPECT_EQ(sp.comps.size(), 3u);

	ASSERT_TRUE(split_path("D:", sp)); // current directory of D:
	EXPECT_EQ(sp.drive, 3);
	EXPECT_TRUE(sp.comps.empty());

	ASSERT_TRUE(split_path("\\", sp));
	EXPECT_TRUE(sp.absolute);
	EXPECT_TRUE(sp.comps.empty());

	EXPECT_FALSE(split_path("", sp));
	EXPECT_FALSE(split_path(std::string(300, 'a'), sp));
}

TEST(HostLfnNames, NewNames)
{
	EXPECT_TRUE(valid_new_name("A long name with spaces.txt"));
	EXPECT_TRUE(valid_new_name("Gr\x9F\xA7" "e"));
	EXPECT_FALSE(valid_new_name(""));
	EXPECT_FALSE(valid_new_name(".."));
	EXPECT_FALSE(valid_new_name("a*b"));
	EXPECT_FALSE(valid_new_name("a:b"));
	EXPECT_FALSE(valid_new_name(" . "));
	EXPECT_FALSE(valid_new_name(std::string(256, 'a')));
}

TEST(HostLfnNames, FindDataLayout)
{
	Entry e;
	e.attrs      = A_ARCHIVE;
	e.size       = 0x12345678;
	e.date       = 0x5A21;
	e.time       = 0x6B42;
	e.geos_name  = "A long name.txt";
	e.short_name = "ALONGN~1.TXT";
	uint8_t fd[FindDataSize];
	pack_find_data(e, fd);
	EXPECT_EQ(fd[0], A_ARCHIVE);
	EXPECT_EQ(fd[20] | fd[21] << 8, 0x6B42); // modified: time, then date
	EXPECT_EQ(fd[22] | fd[23] << 8, 0x5A21);
	EXPECT_EQ(fd[32] | fd[33] << 8 | fd[34] << 16 | fd[35] << 24, 0x12345678);
	EXPECT_STREQ(reinterpret_cast<char*>(fd + 44), "A long name.txt");
	EXPECT_STREQ(reinterpret_cast<char*>(fd + 304), "ALONGN~1.TXT");

	e.geos_name = e.short_name = "README.TXT"; // no alias when equal
	pack_find_data(e, fd);
	EXPECT_EQ(fd[304], 0);
}

TEST_F(HostLfnTest, OddHostNamesDontThrow)
{
	// Not valid UTF-8: the POSIX counterpart of a name Windows can't put
	// into the ANSI code page. Listing must neither throw nor lose it.
	std::ofstream(root_ / std::string("bad\xff" "name.txt")) << "x";
	std::vector<Entry> entries;
	ASSERT_NO_THROW(entries = list());
	bool seen = false;
	for (const auto& e : entries) {
		if (e.host_name.rfind("bad", 0) == 0) {
			seen = true;
			EXPECT_NE(e.geos_name.find('~'), std::string::npos); // hashed
			EXPECT_EQ(find_entry(entries, e.geos_name), &e);
		}
	}
	EXPECT_TRUE(seen);
}
