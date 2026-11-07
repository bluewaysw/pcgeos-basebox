// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Tests for HostFs::Volume, the live host file system behind hostfs.geo.
 */

#include "geos/hostfs_volume.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace HostFs;

namespace {

std::vector<uint8_t> geos_header(const std::string& name, const uint16_t type,
                                 const char* token)
{
	std::vector<uint8_t> h(HeaderSize, 0);
	h[0] = 'G' | 0x80;
	h[1] = 'E';
	h[2] = 'A' | 0x80;
	h[3] = 'S';
	std::memcpy(h.data() + 4, name.data(), name.size());
	h[40] = static_cast<uint8_t>(type);
	std::memcpy(h.data() + 56, token, 4);
	return h;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& d)
{
	std::ofstream f(p, std::ios::binary);
	f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

void write_text(const fs::path& p, const std::string& s)
{
	write_file(p, std::vector<uint8_t>(s.begin(), s.end()));
}

std::vector<uint8_t> read_all(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

class HostFsTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		std::random_device rd;
		root_ = fs::temp_directory_path() / ("hostfs_test_" + std::to_string(rd()));
		fs::create_directories(root_ / "Docs");
		write_text(root_ / "readme.txt", "hello world");
		auto doc = geos_header("Geos Document", GFT_VM, "TEST");
		for (int i = 0; i < 100; ++i) {
			doc.push_back(static_cast<uint8_t>(i));
		}
		write_file(root_ / "Docs" / "DOC00001.000", doc);
		write_file(root_ / "Docs" / "@DIRNAME.000", geos_header("My Documents", GFT_DIRECTORY, "FLDR"));
		vol_ = std::make_unique<Volume>(root_, "HOST");
		ASSERT_TRUE(vol_->ok());
		root_id_ = vol_->root_id();
	}

	void TearDown() override
	{
		vol_.reset();
		std::error_code ec;
		fs::remove_all(root_, ec);
	}

	Item must(const std::string& path)
	{
		Lookup lk;
		EXPECT_EQ(vol_->lookup(root_id_, path, lk), OK) << path;
		EXPECT_TRUE(lk.exists) << path;
		return lk.item;
	}

	bool exists(const std::string& path)
	{
		Lookup lk;
		return vol_->lookup(root_id_, path, lk) == OK && lk.exists;
	}

	std::string read_file(const std::string& path, uint8_t access = 0)
	{
		uint16_t h = 0;
		Item it;
		EXPECT_EQ(vol_->open(root_id_, path, access, h, it), OK);
		std::string out;
		uint8_t buf[4096];
		for (;;) {
			uint16_t got = 0;
			EXPECT_EQ(vol_->read(h, buf, sizeof(buf), got), OK);
			if (!got) {
				break;
			}
			out.append(reinterpret_cast<char*>(buf), got);
		}
		bool closed = false, dirty = false;
		uint32_t id = 0;
		vol_->close(h, closed, dirty, id);
		return out;
	}

	fs::path root_;
	std::unique_ptr<Volume> vol_;
	uint32_t root_id_ = 0;
};

} // namespace

TEST(HostFsNames, EncodeDecodeRoundTrip)
{
	for (const std::string n : {"Plain Name", "a:b*c?d", "100%", "back\\slash",
	                            ".leading", "trailing.", "trailing ", "\x80\xE4\xFF"}) {
		if (n.find('\\') != std::string::npos) {
			continue; // never a component
		}
		const auto host = encode_geos_name(n);
		EXPECT_EQ(decode_host_name(host), n) << n << " -> " << host;
		EXPECT_EQ(host.find_first_of(":*?\"<>|"), std::string::npos);
		EXPECT_NE(host.front(), '.');
	}
	EXPECT_EQ(encode_geos_name("Plain Name"), "Plain Name");
	EXPECT_EQ(decode_host_name("Gr\xc3\xbc\xc3\x9f" "e"), "Gr__e");
	EXPECT_EQ(decode_host_name("50%.txt"), "50%.txt"); // not an escape
}

TEST(HostFsNames, StableIds)
{
	EXPECT_EQ(path_id("Docs/a.txt"), path_id("DOCS/A.TXT"));
	EXPECT_NE(path_id("Docs/a.txt"), path_id("Docs/b.txt"));
}

TEST_F(HostFsTest, LookupNamesAndGeosHeaders)
{
	EXPECT_TRUE(exists("\\readme.txt"));
	EXPECT_TRUE(exists("\\README.TXT"));       // 8.3 / case-insensitive
	EXPECT_TRUE(exists("\\My Documents"));     // @DIRNAME.000 long name
	EXPECT_TRUE(exists("\\Docs"));             // host name
	EXPECT_FALSE(exists("\\Docs\\@DIRNAME.000"));

	const Item doc = must("\\My Documents\\Geos Document");
	EXPECT_EQ(doc.file_type(), GFT_VM);
	EXPECT_EQ(doc.size, 100u);
	EXPECT_EQ(std::string(doc.long_name), "Geos Document");

	AttrValue v;
	ASSERT_EQ(vol_->get_ext_attr(doc, FEA_TOKEN, {}, v), OK);
	EXPECT_EQ(std::memcmp(v.bytes.data(), "TEST", 4), 0);
	const Item txt = must("\\readme.txt");
	EXPECT_EQ(vol_->get_ext_attr(txt, FEA_TOKEN, {}, v), ERROR_ATTR_NOT_FOUND);
	ASSERT_EQ(vol_->get_ext_attr(txt, FEA_DOS_NAME, {}, v), OK);
	EXPECT_STREQ(reinterpret_cast<const char*>(v.bytes.data()), "README.TXT");
}

TEST_F(HostFsTest, RelativeLookupAndDotDot)
{
	const Item docs = must("\\Docs");
	Lookup lk;
	ASSERT_EQ(vol_->lookup(docs.id, "Geos Document", lk), OK);
	EXPECT_TRUE(lk.exists);
	ASSERT_EQ(vol_->lookup(docs.id, "..\\readme.txt", lk), OK);
	EXPECT_TRUE(lk.exists);
	ASSERT_EQ(vol_->lookup(docs.id, "", lk), OK);
	EXPECT_EQ(lk.item.id, docs.id);
	EXPECT_EQ(vol_->lookup(docs.id, "nope\\x", lk), ERROR_PATH_NOT_FOUND);
	EXPECT_EQ(vol_->lookup(12345, "x", lk), ERROR_PATH_NOT_FOUND);
}

TEST_F(HostFsTest, HostChangesAreLive)
{
	EXPECT_FALSE(exists("\\later.txt"));
	write_text(root_ / "later.txt", "new");
	EXPECT_TRUE(exists("\\later.txt"));
	EXPECT_EQ(read_file("\\later.txt"), "new");

	write_text(root_ / "later.txt", "changed content");
	EXPECT_EQ(must("\\later.txt").size, 15u);

	fs::remove(root_ / "later.txt");
	EXPECT_FALSE(exists("\\later.txt"));
}

TEST_F(HostFsTest, GeosDataHidesHeaderUnlessRaw)
{
	const std::string data = read_file("\\Docs\\DOC00001.000");
	ASSERT_EQ(data.size(), 100u);
	EXPECT_EQ(static_cast<uint8_t>(data[5]), 5);
	EXPECT_EQ(read_file("\\Docs\\DOC00001.000", FFAF_RAW).size(), 356u);
}

TEST_F(HostFsTest, CreateWriteSeekTruncate)
{
	uint16_t h = 0;
	Item it;
	ASSERT_EQ(vol_->create(root_id_, "\\native.txt", 0, FCF_NATIVE, 2, h, it), OK);
	const std::string text = "0123456789";
	uint16_t put = 0;
	ASSERT_EQ(vol_->write(h, reinterpret_cast<const uint8_t*>(text.data()), 10, put), OK);
	EXPECT_EQ(put, 10);
	uint32_t pos = 0;
	ASSERT_EQ(vol_->seek(h, FILE_POS_START, 2, pos), OK);
	uint8_t b[4];
	uint16_t got = 0;
	ASSERT_EQ(vol_->read(h, b, 3, got), OK);
	EXPECT_EQ(std::string(reinterpret_cast<char*>(b), got), "234");
	ASSERT_EQ(vol_->seek(h, FILE_POS_END, -1, pos), OK);
	EXPECT_EQ(pos, 9u);
	ASSERT_EQ(vol_->truncate(h, 4), OK);
	uint32_t size = 0;
	ASSERT_EQ(vol_->file_size(h, size), OK);
	EXPECT_EQ(size, 4u);
	bool dirty = false, closed = false;
	vol_->is_dirty(h, dirty);
	EXPECT_TRUE(dirty);
	uint32_t id = 0;
	ASSERT_EQ(vol_->close(h, closed, dirty, id), OK);
	EXPECT_TRUE(closed);
	EXPECT_EQ(id, it.id);
	const auto host = read_all(root_ / "native.txt");
	EXPECT_EQ(std::string(host.begin(), host.end()), "0123");

	EXPECT_EQ(vol_->create(root_id_, "\\native.txt", 0, FCF_NATIVE, 2, h, it), ERROR_FILE_EXISTS);
}

TEST_F(HostFsTest, CreateGeosFileWritesHeaderAndAttrs)
{
	uint16_t h = 0;
	Item it;
	ASSERT_EQ(vol_->create(root_id_, "\\Docs\\New: Report?", 0, 0, 2, h, it), OK);
	uint16_t put = 0;
	ASSERT_EQ(vol_->write(h, reinterpret_cast<const uint8_t*>("abc"), 3, put), OK);
	const uint8_t token[6] = {'A', 'B', 'C', 'D', 0, 0};
	ASSERT_EQ(vol_->set_ext_attr(0, "", h, FEA_TOKEN, token, 6), OK);
	const uint8_t vm[2] = {GFT_VM, 0};
	ASSERT_EQ(vol_->set_ext_attr(0, "", h, FEA_FILE_TYPE, vm, 2), OK);
	bool closed = false, dirty = false;
	uint32_t id = 0;
	vol_->close(h, closed, dirty, id);

	// Host side: escaped name, header + 3 data bytes
	const fs::path host = root_ / "Docs" / "New%3A Report%3F";
	ASSERT_TRUE(fs::exists(host));
	const auto bytes = read_all(host);
	ASSERT_EQ(bytes.size(), HeaderSize + 3);
	EXPECT_EQ(bytes[0], 'G' | 0x80);
	EXPECT_EQ(std::memcmp(bytes.data() + 56, "ABCD", 4), 0);

	// GEOS side: exact name, type, data without header
	const Item again = must("\\Docs\\New: Report?");
	EXPECT_EQ(again.file_type(), GFT_VM);
	EXPECT_EQ(again.size, 3u);
	EXPECT_EQ(read_file("\\Docs\\New: Report?"), "abc");
}

TEST_F(HostFsTest, ExtAttrsOnNativeFilesAndDirectories)
{
	const uint8_t token[6] = {'X', 'X', 'X', 'X', 0, 0};
	EXPECT_EQ(vol_->set_ext_attr(root_id_, "\\readme.txt", 0, FEA_TOKEN, token, 6),
	          ERROR_ATTR_NOT_SUPPORTED);
	EXPECT_EQ(vol_->set_ext_attr(root_id_, "\\readme.txt", 0, FEA_NAME, token, 6),
	          ERROR_ATTR_CANNOT_BE_SET);
	EXPECT_EQ(vol_->set_ext_attr(root_id_, "\\readme.txt", 0, FEA_TOKEN, token, 5),
	          ERROR_ATTR_NOT_SUPPORTED);

	// Directories keep GEOS attributes in @DIRNAME.000
	Item d;
	ASSERT_EQ(vol_->make_dir(root_id_, "\\Folder", d), OK);
	ASSERT_EQ(vol_->set_ext_attr(root_id_, "\\Folder", 0, FEA_TOKEN, token, 6), OK);
	EXPECT_TRUE(fs::exists(root_ / "Folder" / "@DIRNAME.000"));
	AttrValue v;
	ASSERT_EQ(vol_->get_ext_attr(must("\\Folder"), FEA_TOKEN, {}, v), OK);
	EXPECT_EQ(std::memcmp(v.bytes.data(), "XXXX", 4), 0);

	// Modification time round trip (2-second resolution)
	const uint8_t when[4] = {static_cast<uint8_t>(to_file_date(2024, 5, 17) & 0xff),
	                         static_cast<uint8_t>(to_file_date(2024, 5, 17) >> 8),
	                         static_cast<uint8_t>(to_file_time(13, 37, 42) & 0xff),
	                         static_cast<uint8_t>(to_file_time(13, 37, 42) >> 8)};
	ASSERT_EQ(vol_->set_ext_attr(root_id_, "\\readme.txt", 0, FEA_MODIFICATION, when, 4), OK);
	const Item r = must("\\readme.txt");
	EXPECT_EQ(r.mdate, to_file_date(2024, 5, 17));
	EXPECT_EQ(r.mtime, to_file_time(13, 37, 42));
}

TEST_F(HostFsTest, DirectoriesAndRemoval)
{
	Item d;
	ASSERT_EQ(vol_->make_dir(root_id_, "\\Sub", d), OK);
	EXPECT_TRUE(d.is_dir);
	EXPECT_EQ(vol_->make_dir(root_id_, "\\Sub", d), ERROR_FILE_EXISTS);
	uint16_t h = 0;
	Item f;
	ASSERT_EQ(vol_->create(d.id, "inner", 0, FCF_NATIVE, 2, h, f), OK);
	uint32_t id = 0;
	EXPECT_EQ(vol_->remove_file(root_id_, "\\Sub\\inner", id), ERROR_FILE_IN_USE);
	bool closed = false, dirty = false;
	vol_->close(h, closed, dirty, id);
	EXPECT_EQ(vol_->remove_dir(root_id_, "\\Sub", id), ERROR_DIRECTORY_NOT_EMPTY);
	ASSERT_EQ(vol_->remove_file(root_id_, "\\Sub\\inner", id), OK);
	EXPECT_EQ(id, f.id);
	ASSERT_EQ(vol_->remove_dir(root_id_, "\\Sub", id), OK);
	EXPECT_FALSE(fs::exists(root_ / "Sub"));
	EXPECT_EQ(vol_->remove_dir(root_id_, "\\", id), ERROR_ACCESS_DENIED); // root
}

TEST_F(HostFsTest, RenameAndMove)
{
	Item r;
	ASSERT_EQ(vol_->rename(root_id_, "\\My Documents\\Geos Document", "Renamed Doc", r), OK);
	EXPECT_TRUE(exists("\\Docs\\Renamed Doc"));
	EXPECT_FALSE(exists("\\Docs\\Geos Document"));
	// The header's long name follows the rename
	EXPECT_EQ(std::string(r.long_name), "Renamed Doc");
	EXPECT_TRUE(fs::exists(root_ / "Docs" / "Renamed Doc"));

	ASSERT_EQ(vol_->rename(root_id_, "\\readme.txt", "Docs", r), ERROR_FILE_EXISTS);

	Item m;
	ASSERT_EQ(vol_->move(root_id_, "\\readme.txt", root_id_, "\\Docs\\moved.txt", m), OK);
	EXPECT_EQ(read_file("\\Docs\\moved.txt"), "hello world");
	EXPECT_FALSE(exists("\\readme.txt"));

	EXPECT_EQ(vol_->move(root_id_, "\\Docs", root_id_, "\\Docs\\Inside", m),
	          ERROR_ACCESS_DENIED);
}

TEST_F(HostFsTest, EnumerationIsCurrent)
{
	uint16_t e = 0;
	ASSERT_EQ(vol_->enum_begin(root_id_, e), OK);
	std::vector<std::string> names;
	Item it;
	while (vol_->enum_next(e, it) == OK) {
		names.emplace_back(it.long_name);
	}
	ASSERT_EQ(names.size(), 2u);
	EXPECT_EQ(names[0], "My Documents");
	EXPECT_EQ(names[1], "readme.txt");

	write_text(root_ / "z.txt", "z");
	ASSERT_EQ(vol_->enum_begin(root_id_, e), OK);
	int count = 0;
	while (vol_->enum_next(e, it) == OK) {
		++count;
	}
	EXPECT_EQ(count, 3);

	fs::create_directories(root_ / "Empty");
	const Item empty = must("\\Empty");
	EXPECT_EQ(vol_->enum_begin(empty.id, e), ERROR_NO_MORE_FILES);
}

TEST_F(HostFsTest, IdsSurviveRestart)
{
	const uint32_t id = must("\\My Documents\\Geos Document").id;
	vol_ = std::make_unique<Volume>(root_, "HOST");
	EXPECT_EQ(must("\\Docs\\DOC00001.000").id, id);
}

TEST(HostFsConfig, MountLines)
{
	std::string name, path;
	ASSERT_TRUE(parse_mount_line("HOST=C:\\Users\\me\\Geos", name, path));
	EXPECT_EQ(name, "HOST");
	EXPECT_EQ(path, "C:\\Users\\me\\Geos");
	ASSERT_TRUE(parse_mount_line("  My Data =  /home/me/my data  ", name, path));
	EXPECT_EQ(name, "My Data");
	EXPECT_EQ(path, "/home/me/my data");
	ASSERT_TRUE(parse_mount_line("X=\" /odd path \"", name, path));
	EXPECT_EQ(path, " /odd path ");
	EXPECT_FALSE(parse_mount_line("no equals sign", name, path));
	EXPECT_FALSE(parse_mount_line("=/path", name, path));
	EXPECT_FALSE(parse_mount_line("NAME=", name, path));
	EXPECT_FALSE(parse_mount_line(std::string(40, 'N') + "=/p", name, path));
}

TEST(HostFsConfig, AllowList)
{
	const fs::path base = fs::temp_directory_path() / "hostfs_allow_test";
	fs::create_directories(base / "ok" / "deeper");
	fs::create_directories(base / "okay");
	fs::create_directories(base / "other");
	const std::string allow = (base / "ok").string() + ";" + (base / "nonexistent").string();
	EXPECT_TRUE(path_allowed(base / "anything", ""));
	EXPECT_TRUE(path_allowed(base / "ok", allow));
	EXPECT_TRUE(path_allowed(base / "ok" / "deeper", allow));
	EXPECT_FALSE(path_allowed(base / "okay", allow)); // prefix, not a parent
	EXPECT_FALSE(path_allowed(base / "other", allow));
	EXPECT_FALSE(path_allowed(base / "ok" / ".." / "other", allow));
	fs::remove_all(base);
}

TEST(HostFsConfig, VolumesHaveDistinctIds)
{
	const fs::path base = fs::temp_directory_path() / "hostfs_seed_test";
	fs::create_directories(base);
	Volume a(base, "A", path_id("A")), b(base, "B", path_id("B"));
	EXPECT_NE(a.root_id(), b.root_id());
	fs::remove_all(base);
}

namespace {
std::vector<ChangeEvent> drain(Volume& v)
{
	v.poll();
	std::vector<ChangeEvent> out;
	ChangeEvent e;
	while (v.next_change(e)) {
		out.push_back(e);
	}
	return out;
}
} // namespace

TEST_F(HostFsTest, HostChangesAreReported)
{
	// Nothing is watched until GEOS looks at a directory
	write_text(root_ / "unseen.txt", "x");
	EXPECT_TRUE(drain(*vol_).empty());

	std::vector<Item> items;
	ASSERT_EQ(vol_->list(root_id_, items), OK); // GEOS looks at the root
	EXPECT_TRUE(drain(*vol_).empty());

	write_text(root_ / "new file.txt", "1");
	auto ev = drain(*vol_);
	ASSERT_EQ(ev.size(), 1u);
	EXPECT_EQ(ev[0].type, ChangeEvent::Created);
	EXPECT_EQ(ev[0].id, root_id_);
	EXPECT_EQ(ev[0].name, "new file.txt");
	EXPECT_TRUE(drain(*vol_).empty()); // reported once

	const uint32_t id = must("\\new file.txt").id;
	write_text(root_ / "new file.txt", "longer now");
	ev = drain(*vol_);
	ASSERT_EQ(ev.size(), 1u);
	EXPECT_EQ(ev[0].type, ChangeEvent::Contents);
	EXPECT_EQ(ev[0].id, id);

	fs::remove(root_ / "new file.txt");
	ev = drain(*vol_);
	ASSERT_EQ(ev.size(), 1u);
	EXPECT_EQ(ev[0].type, ChangeEvent::Deleted);
	EXPECT_EQ(ev[0].id, id);

	// A rename on the host is a delete plus a create
	fs::rename(root_ / "readme.txt", root_ / "renamed.txt");
	ev = drain(*vol_);
	ASSERT_EQ(ev.size(), 2u);
}

TEST_F(HostFsTest, OwnChangesAreNotReported)
{
	std::vector<Item> items;
	ASSERT_EQ(vol_->list(root_id_, items), OK);

	uint16_t h = 0;
	Item it;
	ASSERT_EQ(vol_->create(root_id_, "\\by geos", 0, FCF_NATIVE, 2, h, it), OK);
	uint16_t put = 0;
	vol_->write(h, reinterpret_cast<const uint8_t*>("data"), 4, put);
	EXPECT_TRUE(drain(*vol_).empty()); // open in GEOS: no Contents event
	bool closed = false, dirty = false;
	uint32_t id = 0;
	vol_->close(h, closed, dirty, id);
	EXPECT_TRUE(drain(*vol_).empty());

	Item d;
	ASSERT_EQ(vol_->make_dir(root_id_, "\\New Folder", d), OK);
	ASSERT_EQ(vol_->remove_file(root_id_, "\\by geos", id), OK);
	EXPECT_TRUE(drain(*vol_).empty());
}

TEST_F(HostFsTest, WatchedDirectoriesAreLimited)
{
	// Look up the directories first (this uses the root)...
	std::vector<uint32_t> ids;
	for (size_t i = 0; i < Volume::MaxWatchedDirs + 5; ++i) {
		const std::string name = "d" + std::to_string(i);
		fs::create_directories(root_ / name);
		ids.push_back(must("\\" + name).id);
	}
	// ...then read only them, so the root becomes the oldest watch
	for (const auto id : ids) {
		std::vector<Item> items;
		ASSERT_EQ(vol_->list(id, items), OK);
	}
	// The root was used first and is no longer watched...
	write_text(root_ / "late.txt", "x");
	auto ev = drain(*vol_);
	EXPECT_TRUE(ev.empty());
	// ...until GEOS looks at it again
	std::vector<Item> items;
	vol_->list(root_id_, items);
	write_text(root_ / "later.txt", "x");
	ev = drain(*vol_);
	ASSERT_EQ(ev.size(), 1u);
	EXPECT_EQ(ev[0].name, "later.txt");
}

TEST_F(HostFsTest, NamesAreStable)
{
	const std::string longA = "A very long host file name that goes on and on.png";
	write_text(root_ / longA, "a");
	write_text(root_ / "Another rather long file name here.txt", "b");

	std::vector<Item> before;
	ASSERT_EQ(vol_->list(root_id_, before), OK);
	auto find = [](const std::vector<Item>& v, const std::string& host) {
		for (const auto& i : v) {
			if (i.host_name == host) {
				return i;
			}
		}
		return Item{};
	};
	const Item a = find(before, longA);
	const std::string ln(a.long_name);
	EXPECT_EQ(ln.size(), LongNameLength);
	EXPECT_EQ(ln.substr(ln.size() - 4), ".png"); // extension kept
	EXPECT_NE(ln.find('~'), std::string::npos);
	const std::string dos(a.dos_name, DosNameLength);
	EXPECT_EQ(dos.substr(0, 2), "A~");
	EXPECT_EQ(dos.substr(8), "PNG");

	// Files with similar names appearing (sorting before it) change nothing
	write_text(root_ / "A very long host file name that goes on and before.png", "c");
	write_text(root_ / "A very long host file name that goes AAA.png", "d");
	std::vector<Item> after;
	ASSERT_EQ(vol_->list(root_id_, after), OK);
	const Item a2 = find(after, longA);
	EXPECT_EQ(std::string(a2.long_name), ln);
	EXPECT_EQ(std::string(a2.dos_name, DosNameLength), dos);

	// Valid 8.3 host names are their own short name
	EXPECT_EQ(std::string(find(after, "readme.txt").dos_name, DosNameLength), "README  TXT");

	// Short and long names reach the file
	EXPECT_TRUE(exists("\\" + ln));
	std::string short_name = dos.substr(0, 8);
	short_name.erase(short_name.find_last_not_of(' ') + 1);
	EXPECT_TRUE(exists("\\" + short_name + ".PNG"));
}

TEST_F(HostFsTest, NewNamesDontCollideWithShortNames)
{
	const std::string longA = "A very long host file name that goes on and on.png";
	write_text(root_ / longA, "a");
	std::vector<Item> items;
	ASSERT_EQ(vol_->list(root_id_, items), OK);
	std::string alias;
	for (const auto& i : items) {
		if (i.host_name == longA) {
			alias = std::string(i.dos_name, 8);
			alias.erase(alias.find_last_not_of(' ') + 1);
			alias += ".PNG";
		}
	}
	ASSERT_FALSE(alias.empty());
	EXPECT_TRUE(exists("\\" + alias)); // the alias finds the long file...

	uint16_t h = 0;
	Item it;
	// ...but creating a file with exactly that name is allowed
	ASSERT_EQ(vol_->create(root_id_, "\\" + alias, 0, FCF_NATIVE, 2, h, it), OK);
	bool closed = false, dirty = false;
	uint32_t id = 0;
	vol_->close(h, closed, dirty, id);
	EXPECT_TRUE(fs::exists(root_ / alias));
	EXPECT_TRUE(fs::exists(root_ / longA));
}

TEST_F(HostFsTest, HeadersAreReadOnceUntilFilesChange)
{
	for (int i = 0; i < 50; ++i) {
		write_file(root_ / ("G" + std::to_string(i) + ".000"),
		           geos_header("Doc " + std::to_string(i), GFT_VM, "TEST"));
	}
	std::vector<Item> items;
	ASSERT_EQ(vol_->list(root_id_, items), OK);
	const size_t first = vol_->header_reads();
	EXPECT_GE(first, 50u);

	// Repeated lookups and listings don't open the files again
	for (int i = 0; i < 20; ++i) {
		EXPECT_TRUE(exists("\\Doc 7"));
		ASSERT_EQ(vol_->list(root_id_, items), OK);
	}
	EXPECT_EQ(vol_->header_reads(), first);

	// A file changed in place is read again; its new GEOS name shows up
	// with the next change check (or after ListingMaxAgeMs), since
	// rewriting a file doesn't change its directory's time
	const fs::path p = root_ / "G7.000";
	write_file(p, geos_header("Renamed On Host", GFT_VM, "TEST"));
	fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(10));
	vol_->poll();
	EXPECT_TRUE(exists("\\Renamed On Host"));
	EXPECT_FALSE(exists("\\Doc 7"));
	EXPECT_EQ(vol_->header_reads(), first + 1);

	// Small files are never opened
	const size_t before_small = vol_->header_reads();
	write_text(root_ / "tiny.txt", "x");
	ASSERT_EQ(vol_->list(root_id_, items), OK);
	EXPECT_EQ(vol_->header_reads(), before_small + 1); // counted, not opened
}
