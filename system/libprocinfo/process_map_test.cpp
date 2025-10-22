/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <procinfo/process_map.h>

#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/stringprintf.h>

#include <gtest/gtest.h>

using android::procinfo::MapInfo;

TEST(process_map, ReadMapFile) {
  std::string map_file = android::base::GetExecutableDirectory() + "/testdata/maps";
  std::vector<android::procinfo::MapInfo> maps;
  ASSERT_TRUE(android::procinfo::ReadMapFile(
      map_file, [&](const android::procinfo::MapInfo& mapinfo) { maps.emplace_back(mapinfo); }));
  ASSERT_EQ(2043u, maps.size());
  ASSERT_EQ(maps[0].start, 0x12c00000ULL);
  ASSERT_EQ(maps[0].end, 0x2ac00000ULL);
  ASSERT_EQ(maps[0].flags, PROT_READ | PROT_WRITE);
  ASSERT_EQ(maps[0].pgoff, 0ULL);
  ASSERT_EQ(maps[0].inode, 10267643UL);
  ASSERT_EQ(maps[0].name, "[anon:dalvik-main space (region space)]");
  ASSERT_EQ(maps[876].start, 0x70e6c4f000ULL);
  ASSERT_EQ(maps[876].end, 0x70e6c6b000ULL);
  ASSERT_EQ(maps[876].flags, PROT_READ | PROT_EXEC);
  ASSERT_EQ(maps[876].pgoff, 0ULL);
  ASSERT_EQ(maps[876].inode, 2407UL);
  ASSERT_EQ(maps[876].name, "/system/lib64/libutils.so");
  ASSERT_EQ(maps[1260].start, 0x70e96fa000ULL);
  ASSERT_EQ(maps[1260].end, 0x70e96fb000ULL);
  ASSERT_EQ(maps[1260].flags, PROT_READ);
  ASSERT_EQ(maps[1260].pgoff, 0ULL);
  ASSERT_EQ(maps[1260].inode, 10266154UL);
  ASSERT_EQ(maps[1260].name,
            "[anon:dalvik-classes.dex extracted in memory from "
            "/data/app/com.google.sample.tunnel-HGGRU03Gu1Mwkf_-RnFmvw==/base.apk]");
}

TEST(process_map, ReadProcessMaps) {
  std::vector<android::procinfo::MapInfo> maps;
  ASSERT_TRUE(android::procinfo::ReadProcessMaps(
      getpid(), [&](const android::procinfo::MapInfo& mapinfo) { maps.emplace_back(mapinfo); }));
  ASSERT_GT(maps.size(), 0u);
  maps.clear();
  ASSERT_TRUE(android::procinfo::ReadProcessMaps(getpid(), &maps));
  ASSERT_GT(maps.size(), 0u);
}

extern "C" void malloc_disable();
extern "C" void malloc_enable();

struct TestMapInfo {
  TestMapInfo() = default;
  TestMapInfo(uint64_t start, uint64_t end, uint16_t flags, uint64_t pgoff, ino_t inode,
              const char* new_name, bool isShared)
      : start(start), end(end), flags(flags), pgoff(pgoff), inode(inode), isShared(isShared) {
    strcpy(name, new_name);
  }
  uint64_t start = 0;
  uint64_t end = 0;
  uint16_t flags = 0;
  uint64_t pgoff = 0;
  ino_t inode = 0;
  char name[100] = {};
  bool isShared = false;
};

void VerifyReadMapFileAsyncSafe(const char* maps_data,
                                const std::vector<TestMapInfo>& expected_info) {
  TemporaryFile tf;
  ASSERT_TRUE(android::base::WriteStringToFd(maps_data, tf.fd));

  std::vector<TestMapInfo> saved_info(expected_info.size());
  size_t num_maps = 0;

  auto callback = [&](uint64_t start, uint64_t end, uint16_t flags, uint64_t pgoff, ino_t inode,
                      const char* name, bool shared) {
    if (num_maps != saved_info.size()) {
      TestMapInfo& saved = saved_info[num_maps];
      saved.start = start;
      saved.end = end;
      saved.flags = flags;
      saved.pgoff = pgoff;
      saved.inode = inode;
      strcpy(saved.name, name);
      saved.isShared = shared;
    }
    num_maps++;
  };

  std::vector<char> buffer(64 * 1024);

#if defined(__BIONIC__)
  // Any allocations will block after this call.
  malloc_disable();
#endif

  bool parsed =
      android::procinfo::ReadMapFileAsyncSafe(tf.path, buffer.data(), buffer.size(), callback);

#if defined(__BIONIC__)
  malloc_enable();
#endif

  ASSERT_TRUE(parsed) << "Parsing of data failed:\n" << maps_data;
  ASSERT_EQ(expected_info.size(), num_maps);
  for (size_t i = 0; i < expected_info.size(); i++) {
    const TestMapInfo& expected = expected_info[i];
    const TestMapInfo& saved = saved_info[i];
    EXPECT_EQ(expected.start, saved.start);
    EXPECT_EQ(expected.end, saved.end);
    EXPECT_EQ(expected.flags, saved.flags);
    EXPECT_EQ(expected.pgoff, saved.pgoff);
    EXPECT_EQ(expected.inode, saved.inode);
    EXPECT_STREQ(expected.name, saved.name);
    EXPECT_EQ(expected.isShared, saved.isShared);
  }
}

TEST(process_map, ReadMapFileAsyncSafe_invalid) {
  std::vector<TestMapInfo> expected_info;

  VerifyReadMapFileAsyncSafe("12c00000-2ac00000", expected_info);
}

TEST(process_map, ReadMapFileAsyncSafe_single) {
  std::vector<TestMapInfo> expected_info;
  expected_info.emplace_back(0x12c00000, 0x2ac00000, PROT_READ | PROT_WRITE, 0x100, 10267643,
                             "/lib/fake.so", false);

  VerifyReadMapFileAsyncSafe("12c00000-2ac00000 rw-p 00000100 00:05 10267643 /lib/fake.so",
                             expected_info);
}

TEST(process_map, ReadMapFileAsyncSafe_single_with_newline) {
  std::vector<TestMapInfo> expected_info;
  expected_info.emplace_back(0x12c00000, 0x2ac00000, PROT_READ | PROT_WRITE, 0x100, 10267643,
                             "/lib/fake.so", false);

  VerifyReadMapFileAsyncSafe("12c00000-2ac00000 rw-p 00000100 00:05 10267643 /lib/fake.so\n",
                             expected_info);
}

TEST(process_map, ReadMapFileAsyncSafe_single_no_library) {
  std::vector<TestMapInfo> expected_info;
  expected_info.emplace_back(0xa0000, 0xc0000, PROT_READ | PROT_WRITE | PROT_EXEC, 0xb00, 101, "",
                             false);

  VerifyReadMapFileAsyncSafe("a0000-c0000 rwxp 00000b00 00:05 101", expected_info);
}

TEST(process_map, ReadMapFileAsyncSafe_multiple) {
  std::vector<TestMapInfo> expected_info;
  expected_info.emplace_back(0xa0000, 0xc0000, PROT_READ | PROT_WRITE | PROT_EXEC, 1, 100, "",
                             false);
  expected_info.emplace_back(0xd0000, 0xe0000, PROT_READ, 2, 101, "/lib/libsomething1.so", false);
  expected_info.emplace_back(0xf0000, 0x100000, PROT_WRITE, 3, 102, "/lib/libsomething2.so", false);
  expected_info.emplace_back(0x110000, 0x120000, PROT_EXEC, 4, 103, "[anon:something or another]",
                             false);
  expected_info.emplace_back(0x130000, 0x140000, PROT_READ, 5, 104, "/lib/libsomething3.so", true);

  std::string map_data =
      "0a0000-0c0000 rwxp 00000001 00:05 100\n"
      "0d0000-0e0000 r--p 00000002 00:05 101  /lib/libsomething1.so\n"
      "0f0000-100000 -w-p 00000003 00:05 102  /lib/libsomething2.so\n"
      "110000-120000 --xp 00000004 00:05 103  [anon:something or another]\n"
      "130000-140000 r--s 00000005 00:05 104  /lib/libsomething3.so\n";

  VerifyReadMapFileAsyncSafe(map_data.c_str(), expected_info);
}

TEST(process_map, ReadMapFileAsyncSafe_multiple_reads) {
  std::vector<TestMapInfo> expected_info;
  std::string map_data;
  uint64_t start = 0xa0000;
  for (size_t i = 0; i < 10000; i++) {
    map_data += android::base::StringPrintf("%" PRIx64 "-%" PRIx64 " r--p %zx 01:20 %zu fake.so\n",
                                            start, start + 0x1000, i, 1000 + i);
    expected_info.emplace_back(start, start + 0x1000, PROT_READ, i, 1000 + i, "fake.so", false);
  }

  VerifyReadMapFileAsyncSafe(map_data.c_str(), expected_info);
}

TEST(process_map, ReadMapFileAsyncSafe_buffer_nullptr) {
  size_t num_calls = 0;
  auto callback = [&](const android::procinfo::MapInfo&) { num_calls++; };

#if defined(__BIONIC__)
  // Any allocations will block after this call.
  malloc_disable();
#endif

  bool parsed = android::procinfo::ReadMapFileAsyncSafe("/proc/self/maps", nullptr, 10, callback);

#if defined(__BIONIC__)
  malloc_enable();
#endif

  ASSERT_FALSE(parsed);
  EXPECT_EQ(0UL, num_calls);
}

TEST(process_map, ReadMapFileAsyncSafe_buffer_size_zero) {
  size_t num_calls = 0;
  auto callback = [&](const android::procinfo::MapInfo&) { num_calls++; };

#if defined(__BIONIC__)
  // Any allocations will block after this call.
  malloc_disable();
#endif

  char buffer[10];
  bool parsed = android::procinfo::ReadMapFileAsyncSafe("/proc/self/maps", buffer, 0, callback);

#if defined(__BIONIC__)
  malloc_enable();
#endif

  ASSERT_FALSE(parsed);
  EXPECT_EQ(0UL, num_calls);
}

TEST(process_map, ReadMapFileAsyncSafe_buffer_too_small_no_calls) {
  size_t num_calls = 0;
  auto callback = [&](const android::procinfo::MapInfo&) { num_calls++; };

#if defined(__BIONIC__)
  // Any allocations will block after this call.
  malloc_disable();
#endif

  char buffer[10];
  bool parsed =
      android::procinfo::ReadMapFileAsyncSafe("/proc/self/maps", buffer, sizeof(buffer), callback);

#if defined(__BIONIC__)
  malloc_enable();
#endif

  ASSERT_FALSE(parsed);
  EXPECT_EQ(0UL, num_calls);
}

TEST(process_map, ReadMapFileAsyncSafe_buffer_too_small_could_parse) {
  TemporaryFile tf;
  ASSERT_TRUE(android::base::WriteStringToFd(
      "0a0000-0c0000 rwxp 00000001 00:05 100    /fake/lib.so\n", tf.fd));

  size_t num_calls = 0;
  auto callback = [&](const android::procinfo::MapInfo&) { num_calls++; };

#if defined(__BIONIC__)
  // Any allocations will block after this call.
  malloc_disable();
#endif

  char buffer[39];
  bool parsed = android::procinfo::ReadMapFileAsyncSafe(tf.path, buffer, sizeof(buffer), callback);

#if defined(__BIONIC__)
  malloc_enable();
#endif

  ASSERT_FALSE(parsed);
  EXPECT_EQ(0UL, num_calls);
}

class ProcessMapMappedFileSize : public ::testing::Test {
  protected:
    void SetUp() override {
      ASSERT_NE(tf.fd, -1) << "open failed: " << strerror(errno);
      ASSERT_EQ(ftruncate(tf.fd, kFileSize), 0) << "ftruncate failed: " << strerror(errno);
    }

    void TearDown() override {
      ASSERT_EQ(munmap(reinterpret_cast<void*>(map.start), map.end-map.start), 0)
                << "munmap failed: " << strerror(errno);
    }

    uint64_t PageAlign(uint64_t x) {
      const uint64_t kPageSize = getpagesize();
      return (x + kPageSize - 1) & ~(kPageSize - 1);
    }

    bool CreateFileMapping(uint64_t size, uint64_t offset) {
      void *addr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, tf.fd, offset);
      if (addr == MAP_FAILED) {
        return false;
      }

      map.start = reinterpret_cast<uint64_t>(addr);
      map.end = PageAlign(map.start + size);
      map.pgoff = offset;

      return true;
    }

    TemporaryFile tf;
    const size_t kFileSize = 65536;
    android::procinfo::MapInfo map = android::procinfo::MapInfo(0 /* start */, 0 /* end */,
                                                                PROT_READ, 0 /* pgoff */,
                                                                0 /* inode */, tf.path, false);
};

TEST_F(ProcessMapMappedFileSize, map_size_greater_than_file_size) {
  uint64_t size = 2 * kFileSize;
  uint64_t offset = 0;

  ASSERT_TRUE(CreateFileMapping(size, offset));
  uint64_t mapped_file_size = android::procinfo::MappedFileSize(map);

  ASSERT_EQ(mapped_file_size, kFileSize);
}

TEST_F(ProcessMapMappedFileSize, map_size_less_than_file_size) {
  uint64_t size = kFileSize / 2;
  uint64_t offset = 0;

  ASSERT_TRUE(CreateFileMapping(size, offset));
  uint64_t mapped_file_size = android::procinfo::MappedFileSize(map);

  ASSERT_EQ(mapped_file_size, size);
}

TEST_F(ProcessMapMappedFileSize, map_size_equal_file_size) {
  uint64_t size = kFileSize;
  uint64_t offset = 0;

  ASSERT_TRUE(CreateFileMapping(size, offset));
  uint64_t mapped_file_size = android::procinfo::MappedFileSize(map);

  ASSERT_EQ(mapped_file_size, kFileSize);

}

TEST_F(ProcessMapMappedFileSize, offset_greater_than_file_size) {
  uint64_t size = kFileSize;
  uint64_t offset = kFileSize * 2;

  ASSERT_TRUE(CreateFileMapping(size, offset));
  uint64_t mapped_file_size = android::procinfo::MappedFileSize(map);

  ASSERT_EQ(mapped_file_size, 0UL);
}

TEST_F(ProcessMapMappedFileSize, invalid_map_name) {
  uint64_t size = kFileSize;
  uint64_t offset = 0;

  ASSERT_TRUE(CreateFileMapping(size, offset));

  // Name is empty
  map.name = "";
  uint64_t mapped_file_size = android::procinfo::MappedFileSize(map);
  ASSERT_EQ(mapped_file_size, 0UL);

  // Is device path
  map.name = "/dev/";
  mapped_file_size = android::procinfo::MappedFileSize(map);
  ASSERT_EQ(mapped_file_size, 0UL);

  // Does not start with '/'
  map.name = "[anon:bss]";
  mapped_file_size = android::procinfo::MappedFileSize(map);
  ASSERT_EQ(mapped_file_size, 0UL);

  // File non-existent
  map.name = "/tmp/non_existent_file";
  mapped_file_size = android::procinfo::MappedFileSize(map);
  ASSERT_EQ(mapped_file_size, 0UL);
}

static MapInfo CreateMapWithOnlyName(const char* name) {
  return MapInfo(0, 0, 0, UINT64_MAX, 0, name, false);
}

TEST(process_map, TaggedMappingNames) {
  MapInfo info = CreateMapWithOnlyName(
      "[anon:mt:/data/local/tmp/debuggerd_test/arm64/debuggerd_test64+108000]");
  ASSERT_EQ(info.name, "/data/local/tmp/debuggerd_test/arm64/debuggerd_test64");
  ASSERT_EQ(info.pgoff, 0x108000ull);

  info = CreateMapWithOnlyName("[anon:mt:/data/local/tmp/debuggerd_test/arm64/debuggerd_test64+0]");
  ASSERT_EQ(info.name, "/data/local/tmp/debuggerd_test/arm64/debuggerd_test64");
  ASSERT_EQ(info.pgoff, 0x0ull);

  info =
      CreateMapWithOnlyName("[anon:mt:/data/local/tmp/debuggerd_test/arm64/debuggerd_test64+0000]");
  ASSERT_EQ(info.name, "/data/local/tmp/debuggerd_test/arm64/debuggerd_test64");
  ASSERT_EQ(info.pgoff, 0x0ull);

  info = CreateMapWithOnlyName(
      "[anon:mt:...ivetest64/bionic-unit-tests/bionic-loader-test-libs/libdlext_test.so+e000]");
  ASSERT_EQ(info.name, "...ivetest64/bionic-unit-tests/bionic-loader-test-libs/libdlext_test.so");
  ASSERT_EQ(info.pgoff, 0xe000ull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/x+e000]");
  ASSERT_EQ(info.name, "/bin/x");
  ASSERT_EQ(info.pgoff, 0xe000ull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/x+0]");
  ASSERT_EQ(info.name, "/bin/x");
  ASSERT_EQ(info.pgoff, 0x0ull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/x+1]");
  ASSERT_EQ(info.name, "/bin/x");
  ASSERT_EQ(info.pgoff, 0x1ull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/x+f]");
  ASSERT_EQ(info.name, "/bin/x");
  ASSERT_EQ(info.pgoff, 0xfull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/with/plus+/x+f]");
  ASSERT_EQ(info.name, "/bin/with/plus+/x");
  ASSERT_EQ(info.pgoff, 0xfull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/+with/mu+ltiple/plus+/x+f]");
  ASSERT_EQ(info.name, "/bin/+with/mu+ltiple/plus+/x");
  ASSERT_EQ(info.pgoff, 0xfull);

  info = CreateMapWithOnlyName("[anon:mt:/bin/trailing/plus++f]");
  ASSERT_EQ(info.name, "/bin/trailing/plus+");
  ASSERT_EQ(info.pgoff, 0xfull);

  info = CreateMapWithOnlyName("[anon:mt:++f]");
  ASSERT_EQ(info.name, "+");
  ASSERT_EQ(info.pgoff, 0xfull);
}

TEST(process_map, AlmostTaggedMappingNames) {
  for (const char* almost_tagged_name :
       {"[anon:mt:/bin/x+]",
        "[anon:mt:/bin/x]"
        "[anon:mt:+]",
        "[anon:mt", "[anon:mt:/bin/x+1", "[anon:mt:/bin/x+e000",
        "anon:mt:/data/local/tmp/debuggerd_test/arm64/debuggerd_test64+e000]"}) {
    MapInfo info = CreateMapWithOnlyName(almost_tagged_name);
    ASSERT_EQ(info.name, almost_tagged_name);
    ASSERT_EQ(info.pgoff, UINT64_MAX) << almost_tagged_name;
  }
}
