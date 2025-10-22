/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "apexd.h"

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/result-gmock.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libdm/dm.h>
#include <microdroid/metadata.h>
#include <selinux/selinux.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "apex_constants.h"
#include "apex_database.h"
#include "apex_file.h"
#include "apex_file_repository.h"
#include "apex_manifest.pb.h"
#include "apexd_checkpoint.h"
#include "apexd_image_manager.h"
#include "apexd_loop.h"
#include "apexd_metrics.h"
#include "apexd_session.h"
#include "apexd_test_utils.h"
#include "apexd_utils.h"
#include "com_android_apex.h"
#include "gmock/gmock-matchers.h"

namespace android {
namespace apex {

using namespace std::literals;
namespace fs = std::filesystem;

using MountedApexData = MountedApexDatabase::MountedApexData;
using android::apex::testing::ApexFileEq;
using android::base::GetExecutableDirectory;
using android::base::GetProperty;
using android::base::Join;
using android::base::make_scope_guard;
using android::base::ReadFileToString;
using android::base::ReadFully;
using android::base::RemoveFileIfExists;
using android::base::Result;
using android::base::Split;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::base::WriteStringToFile;
using android::base::testing::HasError;
using android::base::testing::HasValue;
using android::base::testing::Ok;
using android::base::testing::WithCode;
using android::base::testing::WithMessage;
using android::dm::DeviceMapper;
using ::apex::proto::SessionState;
using com::android::apex::testing::ApexInfoXmlEq;
using ::testing::ByRef;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::EndsWith;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pointwise;
using ::testing::Property;
using ::testing::StartsWith;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;
using ::testing::internal::CaptureStderr;
using ::testing::internal::GetCapturedStderr;

static int64_t GetMTime(const std::string& path) {
  struct stat st_buf;
  if (stat(path.c_str(), &st_buf) != 0) {
    PLOG(ERROR) << "Failed to stat " << path;
    return 0;
  }
  return st_buf.st_mtime;
}

static int64_t GetSizeByBlocks(const std::string& path) {
  struct stat st_buf;
  if (stat(path.c_str(), &st_buf) != 0) {
    PLOG(ERROR) << "Failed to stat " << path;
    return 0;
  }
  return st_buf.st_blocks * st_buf.st_blksize;
}

static Result<ApexFile> GetActivePackage(const std::string& packageName) {
  std::vector<ApexFile> packages = GetActivePackages();
  for (ApexFile& apex : packages) {
    if (apex.GetManifest().name() == packageName) {
      return std::move(apex);
    }
  }

  return base::ErrnoError()
         << "Cannot find matching package for: " << packageName;
}

// A very basic mock of CheckpointInterface.
class MockCheckpointInterface : public CheckpointInterface {
 public:
  Result<bool> SupportsFsCheckpoints() override {
    return supports_fs_checkpoint_;
  }

  Result<bool> NeedsCheckpoint() override { return needs_checkpoint_; }

  Result<bool> NeedsRollback() override { return needs_rollback_; }

  Result<void> AbortChanges(const std::string& msg, bool retry) override {
    return {};
  }

  void SetSupportsCheckpoint(bool value) { supports_fs_checkpoint_ = value; }

  void SetNeedsCheckpoint(bool value) { needs_checkpoint_ = value; }

  void SetNeedsRollback(bool value) { needs_rollback_ = value; }

 private:
  bool supports_fs_checkpoint_, needs_checkpoint_, needs_rollback_;
};

static constexpr const char* kTestApexdStatusSysprop = "apexd.status.test";
static constexpr const char* kTestVmPayloadMetadataPartitionProp =
    "apexd.vm.payload_metadata_partition.test";

static constexpr const char* kTestActiveApexSelinuxCtx =
    "u:object_r:shell_data_file:s0";

// A test fixture that provides frequently required temp directories for tests
class ApexdUnitTest : public ::testing::Test {
 public:
  ApexdUnitTest() {
    built_in_dir_ = StringPrintf("%s/pre-installed-apex", td_.path);
    partition_ = ApexPartition::System;
    partition_string_ = "SYSTEM";
    block_partition_string_ = "SYSTEM";
    data_dir_ = StringPrintf("%s/data-apex", td_.path);
    decompression_dir_ = StringPrintf("%s/decompressed-apex", td_.path);
    ota_reserved_dir_ = StringPrintf("%s/ota-reserved", td_.path);
    staged_session_dir_ = StringPrintf("%s/staged-session-dir", td_.path);

    sessions_metadata_dir_ =
        StringPrintf("%s/metadata-staged-session-dir", td_.path);
    session_manager_ = ApexSessionManager::Create(sessions_metadata_dir_);

    metadata_images_dir_ = StringPrintf("%s/metadata-images", td_.path);
    data_images_dir_ = StringPrintf("%s/data-images", td_.path);
    image_manager_ =
        ApexImageManager::Create(metadata_images_dir_, data_images_dir_);

    config_ = ApexdConfig{
        kTestApexdStatusSysprop,
        {{partition_, built_in_dir_}},
        data_dir_.c_str(),
        decompression_dir_.c_str(),
        ota_reserved_dir_.c_str(),
        staged_session_dir_.c_str(),
        kTestVmPayloadMetadataPartitionProp,
        kTestActiveApexSelinuxCtx,
        false, /*mount_before_data*/
    };
  }

  const std::string& GetBuiltInDir() { return built_in_dir_; }
  ApexPartition GetPartition() { return partition_; }
  const std::string& GetPartitionString() { return partition_string_; }
  const std::string& GetBlockPartitionString() {
    return block_partition_string_;
  }
  const std::string& GetDataDir() { return data_dir_; }
  const std::string& GetDecompressionDir() { return decompression_dir_; }
  const std::string& GetOtaReservedDir() { return ota_reserved_dir_; }
  const std::string GetStagedDir(int session_id) {
    return StringPrintf("%s/session_%d", staged_session_dir_.c_str(),
                        session_id);
  }
  ApexSessionManager* GetSessionManager() { return session_manager_.get(); }

  std::string GetRootDigest(const ApexFile& apex) {
    if (apex.IsCompressed()) {
      return "";
    }
    auto digest = apex.VerifyApexVerity(apex.GetBundledPublicKey());
    if (!digest.ok()) {
      return "";
    }
    return digest->root_digest;
  }

  std::string AddPreInstalledApex(const std::string& apex_name) {
    fs::copy(GetTestFile(apex_name), built_in_dir_);
    return StringPrintf("%s/%s", built_in_dir_.c_str(), apex_name.c_str());
  }

  std::string AddDataApex(const std::string& apex_name) {
    fs::copy(GetTestFile(apex_name), data_dir_);
    return StringPrintf("%s/%s", data_dir_.c_str(), apex_name.c_str());
  }

  std::string AddDataApex(const std::string& apex_name,
                          const std::string& target_name) {
    fs::copy(GetTestFile(apex_name), data_dir_ + "/" + target_name);
    return StringPrintf("%s/%s", data_dir_.c_str(), target_name.c_str());
  }

  std::string AddDecompressedApex(const std::string& apex_name) {
    auto apex_file = ApexFile::Open(GetTestFile(apex_name));
    CHECK(apex_file.ok());
    std::string target_name =
        apex_file->GetManifest().name() + "@" +
        std::to_string(apex_file->GetManifest().version()) +
        std::string(kDecompressedApexPackageSuffix);
    fs::copy(GetTestFile(apex_name), decompression_dir_ + "/" + target_name);
    return StringPrintf("%s/%s", decompression_dir_.c_str(),
                        target_name.c_str());
  }

  // Copies the compressed apex to |built_in_dir| and decompresses it to
  // |decompressed_dir| and returns both paths as tuple.
  std::tuple<std::string, std::string> PrepareCompressedApex(
      const std::string& name, const std::string& built_in_dir) {
    fs::copy(GetTestFile(name), built_in_dir);
    auto compressed_file_path =
        StringPrintf("%s/%s", built_in_dir.c_str(), name.c_str());
    auto compressed_apex = ApexFile::Open(compressed_file_path);
    std::vector<ApexFileRef> compressed_apex_list;
    compressed_apex_list.emplace_back(std::cref(*compressed_apex));
    auto decompressed =
        ProcessCompressedApex(compressed_apex_list, /*is_ota_chroot*/ false);
    CHECK(decompressed.size() == 1);
    return std::make_tuple(compressed_file_path, decompressed[0].GetPath());
  }

  std::tuple<std::string, std::string> PrepareCompressedApex(
      const std::string& name) {
    return PrepareCompressedApex(name, built_in_dir_);
  }

  std::string PrepareStagedSession(const std::string& apex_name,
                                   int session_id) {
    auto session_dir = GetStagedDir(session_id);
    CreateDirIfNeeded(session_dir, 0755);
    fs::copy(GetTestFile(apex_name), session_dir);
    return session_dir + "/" + apex_name;
  }

  Result<ApexSession> CreateStagedSession(const std::string& apex_name,
                                          int session_id) {
    PrepareStagedSession(apex_name, session_id);
    auto result = session_manager_->CreateSession(session_id);
    if (!result.ok()) {
      return result.error();
    }
    result->SetBuildFingerprint(GetProperty("ro.build.fingerprint", ""));
    return result;
  }

 protected:
  void SetUp() override {
    SetConfig(config_);
    ApexFileRepository::GetInstance().Reset(decompression_dir_);
    ASSERT_EQ(mkdir(built_in_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(data_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(decompression_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(ota_reserved_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(staged_session_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(sessions_metadata_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(metadata_images_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(data_images_dir_.c_str(), 0755), 0);

    // We don't really need for all the test cases, but until we refactor apexd
    // to use dependency injection instead of this SetConfig approach, it is not
    // trivial to figure out which test cases need the session manager, so we
    // initialize it for all of them.
    InitializeSessionManager(GetSessionManager());
    DeleteDirContent(GetSessionsDir());

    InitializeImageManager(image_manager_.get());
  }

  void TearDown() override {
    DeleteDirContent(GetSessionsDir());
    // Reset vold; some tests changing this might affect other tests.
    InitializeVold(nullptr);
  }

 protected:
  TemporaryDir td_;
  std::string built_in_dir_;
  ApexPartition partition_;
  std::string partition_string_;
  std::string block_partition_string_;
  std::string data_dir_;
  std::string decompression_dir_;
  std::string ota_reserved_dir_;

  std::string staged_session_dir_;
  std::string sessions_metadata_dir_;
  std::unique_ptr<ApexSessionManager> session_manager_;

  std::string metadata_images_dir_;
  std::string data_images_dir_;
  std::unique_ptr<ApexImageManager> image_manager_;

  ApexdConfig config_;
};

TEST_F(ApexdUnitTest, SelectApexForActivationSuccess) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("com.android.apex.cts.shim.apex");
  auto shared_lib_1 = ApexFile::Open(AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex"));
  auto& instance = ApexFileRepository::GetInstance();
  // Pre-installed data needs to be present so that we can add data apex
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto apexd_test_file = ApexFile::Open(AddDataApex("apex.apexd_test.apex"));
  auto shim_v1 = ApexFile::Open(AddDataApex("com.android.apex.cts.shim.apex"));
  // Normally both pre-installed and data apex would be activated for a shared
  // libs apex, but if they are the same version only the data apex will be.
  auto shared_lib_2 = ApexFile::Open(
      AddDataApex("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"));
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());

  auto result = SelectApexForActivation();
  ASSERT_EQ(result.size(), 3u);
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1)),
                                           ApexFileEq(ByRef(*shared_lib_2))));
}

// Higher version gets priority when selecting for activation
TEST_F(ApexdUnitTest, HigherVersionOfApexIsSelected) {
  auto apexd_test_file_v2 =
      ApexFile::Open(AddPreInstalledApex("apex.apexd_test_v2.apex"));
  AddPreInstalledApex("com.android.apex.cts.shim.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  TemporaryDir data_dir;
  AddDataApex("apex.apexd_test.apex");
  auto shim_v2 =
      ApexFile::Open(AddDataApex("com.android.apex.cts.shim.v2.apex"));
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());

  auto result = SelectApexForActivation();
  ASSERT_EQ(result.size(), 2u);

  ASSERT_THAT(result,
              UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file_v2)),
                                   ApexFileEq(ByRef(*shim_v2))));
}

// When versions are equal, non-pre-installed version gets priority
TEST_F(ApexdUnitTest, DataApexGetsPriorityForSameVersions) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("com.android.apex.cts.shim.apex");
  // Initialize pre-installed APEX information
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto apexd_test_file = ApexFile::Open(AddDataApex("apex.apexd_test.apex"));
  auto shim_v1 = ApexFile::Open(AddDataApex("com.android.apex.cts.shim.apex"));
  // Initialize ApexFile repo
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());

  auto result = SelectApexForActivation();
  ASSERT_EQ(result.size(), 2u);

  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1))));
}

// Both versions of shared libs can be selected when preinstalled version is
// lower than data version
TEST_F(ApexdUnitTest, SharedLibsCanHaveBothVersionSelected) {
  auto shared_lib_v1 = ApexFile::Open(AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex"));
  // Initialize pre-installed APEX information
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto shared_lib_v2 = ApexFile::Open(
      AddDataApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex"));
  // Initialize data APEX information
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());

  auto result = SelectApexForActivation();
  ASSERT_EQ(result.size(), 2u);

  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*shared_lib_v1)),
                                           ApexFileEq(ByRef(*shared_lib_v2))));
}

// Data version of shared libs should not be selected if lower than
// preinstalled version
TEST_F(ApexdUnitTest, SharedLibsDataVersionDeletedIfLower) {
  auto shared_lib_v2 = ApexFile::Open(AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v2.libvY.apex"));
  // Initialize pre-installed APEX information
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto shared_lib_v1 = ApexFile::Open(
      AddDataApex("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"));
  // Initialize data APEX information
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());

  auto result = SelectApexForActivation();
  ASSERT_EQ(result.size(), 1u);

  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*shared_lib_v2))));
}

TEST_F(ApexdUnitTest, ProcessCompressedApex) {
  auto compressed_apex = ApexFile::Open(
      AddPreInstalledApex("com.android.apex.compressed.v1.capex"));

  std::vector<ApexFileRef> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);

  std::string decompressed_file_path = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  // Assert output path is not empty
  auto exists = PathExists(decompressed_file_path);
  ASSERT_THAT(exists, HasValue(true));

  // Assert that return value contains decompressed APEX
  auto decompressed_apex = ApexFile::Open(decompressed_file_path);
  ASSERT_THAT(return_value,
              UnorderedElementsAre(ApexFileEq(ByRef(*decompressed_apex))));
}

TEST_F(ApexdUnitTest, ProcessCompressedApexRunsVerification) {
  auto compressed_apex_mismatch_key = ApexFile::Open(AddPreInstalledApex(
      "com.android.apex.compressed_key_mismatch_with_original.capex"));
  auto compressed_apex_version_mismatch = ApexFile::Open(
      AddPreInstalledApex("com.android.apex.compressed.v1_with_v2_apex.capex"));

  std::vector<ApexFileRef> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex_mismatch_key));
  compressed_apex_list.emplace_back(
      std::cref(*compressed_apex_version_mismatch));
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(return_value.size(), 0u);
}

TEST_F(ApexdUnitTest, ValidateDecompressedApex) {
  auto capex = ApexFile::Open(
      AddPreInstalledApex("com.android.apex.compressed.v1.capex"));
  auto decompressed_v1 =
      ApexFile::Open(AddDataApex("com.android.apex.compressed.v1.apex"));

  auto result =
      ValidateDecompressedApex(std::cref(*capex), std::cref(*decompressed_v1));
  ASSERT_THAT(result, Ok());

  // Validation checks version
  auto decompressed_v2 = ApexFile::Open(
      AddDataApex("com.android.apex.compressed.v2_original.apex"));
  result =
      ValidateDecompressedApex(std::cref(*capex), std::cref(*decompressed_v2));
  ASSERT_THAT(
      result,
      HasError(WithMessage(HasSubstr(
          "Compressed APEX has different version than decompressed APEX"))));

  // Validation check root digest
  auto decompressed_v1_different_digest = ApexFile::Open(AddDataApex(
      "com.android.apex.compressed.v1_different_digest_original.apex"));
  result = ValidateDecompressedApex(
      std::cref(*capex), std::cref(*decompressed_v1_different_digest));
  ASSERT_THAT(result, HasError(WithMessage(HasSubstr(
                          "does not match with expected root digest"))));

  // Validation checks key
  auto capex_different_key = ApexFile::Open(
      AddDataApex("com.android.apex.compressed_different_key.capex"));
  result = ValidateDecompressedApex(std::cref(*capex_different_key),
                                    std::cref(*decompressed_v1));
  ASSERT_THAT(
      result,
      HasError(WithMessage(HasSubstr(
          "Public key of compressed APEX is different than original"))));
}

TEST_F(ApexdUnitTest, ProcessCompressedApexCanBeCalledMultipleTimes) {
  auto compressed_apex = ApexFile::Open(
      AddPreInstalledApex("com.android.apex.compressed.v1.capex"));

  std::vector<ApexFileRef> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(return_value.size(), 1u);

  // Capture the creation time of the decompressed APEX
  std::error_code ec;
  auto decompressed_apex_path = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  auto last_write_time_1 = fs::last_write_time(decompressed_apex_path, ec);
  ASSERT_FALSE(ec) << "Failed to capture last write time of "
                   << decompressed_apex_path;

  // Now try to decompress the same capex again. It should not fail.
  return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(return_value.size(), 1u);

  // Ensure the decompressed APEX file did not change
  auto last_write_time_2 = fs::last_write_time(decompressed_apex_path, ec);
  ASSERT_FALSE(ec) << "Failed to capture last write time of "
                   << decompressed_apex_path;
  ASSERT_EQ(last_write_time_1, last_write_time_2);
}

// Test behavior of ProcessCompressedApex when is_ota_chroot is true
TEST_F(ApexdUnitTest, ProcessCompressedApexOnOtaChroot) {
  auto compressed_apex = ApexFile::Open(
      AddPreInstalledApex("com.android.apex.compressed.v1.capex"));

  std::vector<ApexFileRef> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ true);
  ASSERT_EQ(return_value.size(), 1u);

  // Decompressed APEX should be located in decompression_dir
  std::string decompressed_file_path =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);
  // Assert output path is not empty
  auto exists = PathExists(decompressed_file_path);
  ASSERT_THAT(exists, HasValue(true))
      << decompressed_file_path << " does not exist";

  // Assert that return value contains the decompressed APEX
  auto apex_file = ApexFile::Open(decompressed_file_path);
  ASSERT_THAT(return_value,
              UnorderedElementsAre(ApexFileEq(ByRef(*apex_file))));
}

// When decompressing APEX, reuse existing OTA APEX
TEST_F(ApexdUnitTest, ProcessCompressedApexReuseOtaApex) {
  // Push a compressed APEX that will fail to decompress
  auto compressed_apex = ApexFile::Open(AddPreInstalledApex(
      "com.android.apex.compressed.v1_not_decompressible.capex"));

  std::vector<ApexFileRef> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));

  // If we try to decompress capex directly, it should fail since the capex
  // pushed is faulty and cannot be decompressed
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(return_value.size(), 0u);

  // But, if there is an ota_apex present for reuse, it should reuse that
  // and avoid decompressing the faulty capex

  // Push an OTA apex that should be reused to skip decompression
  auto ota_apex_path =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.apex"), ota_apex_path);
  return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(return_value.size(), 1u);

  // Ota Apex should be cleaned up
  ASSERT_THAT(PathExists(ota_apex_path), HasValue(false));
  ASSERT_EQ(return_value[0].GetPath(),
            StringPrintf("%s/com.android.apex.compressed@1%s",
                         GetDecompressionDir().c_str(),
                         kDecompressedApexPackageSuffix));
}

TEST_F(ApexdUnitTest, ShouldAllocateSpaceForDecompression_NewApex) {
  ApexFileRepository instance;
  MountedApexDatabase db;

  // A brand new compressed APEX is being introduced: selected
  bool result = ShouldAllocateSpaceForDecompression("com.android.brand.new", 1,
                                                    instance, db);
  ASSERT_TRUE(result);
}

TEST_F(ApexdUnitTest,
       ShouldAllocateSpaceForDecompression_WasNotCompressedBefore) {
  ApexFileRepository instance;
  auto preinstalled_path = AddPreInstalledApex("apex.apexd_test.apex");
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  // An existing pre-installed APEX is now compressed in the OTA: selected
  {
    MountedApexDatabase db;
    db.AddMountedApex("com.android.apex.test_package", 1, "", preinstalled_path,
                      "mount_point", "device_name");
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.test_package", 1, instance, db);
    ASSERT_TRUE(result);
  }

  // Even if there is a data apex (lower version)
  // Include data apex within calculation now
  auto data_path = AddDataApex("apex.apexd_test_v2.apex");
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());
  {
    MountedApexDatabase db;
    db.AddMountedApex("com.android.apex.test_package", 2, "", data_path,
                      "mount_point", "device_name");
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.test_package", 3, instance, db);
    ASSERT_TRUE(result);
  }

  // But not if data apex has equal or higher version
  {
    MountedApexDatabase db;
    db.AddMountedApex("com.android.apex.test_package", 2, "", data_path,
                      "mount_point", "device_name");
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.test_package", 2, instance, db);
    ASSERT_FALSE(result);
  }
}

TEST_F(ApexdUnitTest, ShouldAllocateSpaceForDecompression_VersionCompare) {
  // Prepare fake pre-installed apex
  ApexFileRepository instance(decompression_dir_);
  auto [_, decompressed_path] =
      PrepareCompressedApex("com.android.apex.compressed.v1.capex");
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());
  // Fake mount
  MountedApexDatabase db;
  db.AddMountedApex("com.android.apex.compressed", 1, "", decompressed_path,
                    "mount_point", "device_name");

  {
    // New Compressed apex has higher version than decompressed data apex:
    // selected

    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 2, instance, db);
    ASSERT_TRUE(result)
        << "Higher version test with decompressed data returned false";
  }

  // Compare against decompressed data apex
  {
    // New Compressed apex has same version as decompressed data apex: selected
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 1, instance, db);
    ASSERT_TRUE(result) << "Even with same version, the incoming apex may have "
                           "a different size. Need to decompress";
  }

  {
    // New Compressed apex has lower version than decompressed data apex:
    // selected
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 0, instance, db);
    ASSERT_TRUE(result)
        << "lower version test with decompressed data returned false";
  }

  // Replace decompressed data apex with a higher version
  auto data_path = AddDataApex("com.android.apex.compressed.v2_original.apex");
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());
  db.Reset();
  db.AddMountedApex("com.android.apex.compressed", 2, "", data_path,
                    "mount_point", "device_name");
  {
    // New Compressed apex has higher version as data apex: selected
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 3, instance, db);
    ASSERT_TRUE(result) << "Higher version test with new data returned false";
  }

  {
    // New Compressed apex has same version as data apex: not selected
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 2, instance, db);
    ASSERT_FALSE(result) << "Same version test with new data returned true";
  }

  {
    // New Compressed apex has lower version than data apex: not selected
    bool result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 1, instance, db);
    ASSERT_FALSE(result) << "lower version test with new data returned true";
  }
}

TEST_F(ApexdUnitTest, ReserveSpaceForCompressedApexCreatesSingleFile) {
  TemporaryDir dest_dir;
  // Reserving space should create a single file in dest_dir with exact size

  ASSERT_THAT(ReserveSpaceForCompressedApex(100, dest_dir.path), Ok());
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_THAT(files, Ok());
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 100u);
  EXPECT_GE(GetSizeByBlocks((*files)[0]), 100u);
}

TEST_F(ApexdUnitTest, ReserveSpaceForCompressedApexSafeToCallMultipleTimes) {
  TemporaryDir dest_dir;
  // Calling ReserveSpaceForCompressedApex multiple times should still create
  // a single file
  ASSERT_THAT(ReserveSpaceForCompressedApex(100, dest_dir.path), Ok());
  ASSERT_THAT(ReserveSpaceForCompressedApex(100, dest_dir.path), Ok());
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_THAT(files, Ok());
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 100u);
  EXPECT_GE(GetSizeByBlocks((*files)[0]), 100u);
}

TEST_F(ApexdUnitTest, ReserveSpaceForCompressedApexShrinkAndGrow) {
  TemporaryDir dest_dir;

  // Create a 100 byte file
  ASSERT_THAT(ReserveSpaceForCompressedApex(100, dest_dir.path), Ok());

  // Should be able to shrink and grow the reserved space
  ASSERT_THAT(ReserveSpaceForCompressedApex(1000, dest_dir.path), Ok());

  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_THAT(files, Ok());
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 1000u);
  EXPECT_GE(GetSizeByBlocks((*files)[0]), 1000u);

  ASSERT_THAT(ReserveSpaceForCompressedApex(10, dest_dir.path), Ok());
  files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_THAT(files, Ok());
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 10u);
  EXPECT_GE(GetSizeByBlocks((*files)[0]), 10u);
}

TEST_F(ApexdUnitTest, ReserveSpaceForCompressedApexDeallocateIfPassedZero) {
  TemporaryDir dest_dir;

  // Create a file first
  ASSERT_THAT(ReserveSpaceForCompressedApex(100, dest_dir.path), Ok());
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_THAT(files, Ok());
  ASSERT_EQ(files->size(), 1u);

  // Should delete the reserved file if size passed is 0
  ASSERT_THAT(ReserveSpaceForCompressedApex(0, dest_dir.path), Ok());
  files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_THAT(files, Ok());
  ASSERT_EQ(files->size(), 0u);
}

TEST_F(ApexdUnitTest, ReserveSpaceForCapexCleansOtaApex) {
  TemporaryDir dest_dir;

  auto ota_apex_path = StringPrintf(
      "%s/ota_apex%s", GetDecompressionDir().c_str(), kOtaApexPackageSuffix);
  auto create_ota_apex = [&]() {
    // Create an ota_apex first
    fs::copy(GetTestFile("com.android.apex.compressed.v1.apex"), ota_apex_path);
    ASSERT_THAT(PathExists(ota_apex_path), HasValue(true));
  };
  create_ota_apex();

  // Should not delete the reserved file if size passed is negative
  ASSERT_THAT(ReserveSpaceForCompressedApex(-1, dest_dir.path), Not(Ok()));
  ASSERT_THAT(PathExists(ota_apex_path), HasValue(true));

  // Should delete the reserved file if size passed is 0
  ASSERT_THAT(ReserveSpaceForCompressedApex(0, dest_dir.path), Ok());
  ASSERT_THAT(PathExists(ota_apex_path), HasValue(false));

  create_ota_apex();
  // Should delete the reserved file if size passed is positive
  ASSERT_THAT(ReserveSpaceForCompressedApex(10, dest_dir.path), Ok());
  ASSERT_THAT(PathExists(ota_apex_path), HasValue(false));
}

TEST_F(ApexdUnitTest, ReserveSpaceForCompressedApexErrorForNegativeValue) {
  TemporaryDir dest_dir;
  // Should return error if negative value is passed
  ASSERT_THAT(ReserveSpaceForCompressedApex(-1, dest_dir.path), Not(Ok()));
}

TEST_F(ApexdUnitTest, GetStagedApexFilesNoChild) {
  // Create staged session
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  // Query for its file
  auto result = GetStagedApexFiles(123, {});

  auto apex_file = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", GetStagedDir(123).c_str()));
  ASSERT_THAT(result,
              HasValue(UnorderedElementsAre(ApexFileEq(ByRef(*apex_file)))));
}

TEST_F(ApexdUnitTest, GetStagedApexFilesOnlyStaged) {
  // Create staged session
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::VERIFIED);

  // Query for its file
  auto result = GetStagedApexFiles(123, {});

  ASSERT_THAT(
      result,
      HasError(WithMessage(HasSubstr("Session 123 is not in state STAGED"))));
}

TEST_F(ApexdUnitTest, GetStagedApexFilesChecksNumberOfApexFiles) {
  // Create staged session
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);
  auto staged_dir = GetStagedDir(123);

  {
    // Delete the staged apex file
    DeleteDirContent(staged_dir);

    // Query for its file
    auto result = GetStagedApexFiles(123, {});
    ASSERT_THAT(result, HasError(WithMessage(HasSubstr(
                            "Expected exactly one APEX file in directory"))));
    ASSERT_THAT(result, HasError(WithMessage(HasSubstr("Found: 0"))));
  }
  {
    // Copy multiple files to staged dir
    fs::copy(GetTestFile("apex.apexd_test.apex"), staged_dir);
    fs::copy(GetTestFile("apex.apexd_test_v2.apex"), staged_dir);

    // Query for its file
    auto result = GetStagedApexFiles(123, {});
    ASSERT_THAT(result, HasError(WithMessage(HasSubstr(
                            "Expected exactly one APEX file in directory"))));
    ASSERT_THAT(result, HasError(WithMessage(HasSubstr("Found: 2"))));
  }
}

TEST_F(ApexdUnitTest, GetStagedApexFilesWithChildren) {
  // Create staged session
  auto parent_apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  parent_apex_session->UpdateStateAndCommit(SessionState::STAGED);
  auto child_session_1 = CreateStagedSession("apex.apexd_test.apex", 124);
  auto child_session_2 = CreateStagedSession("apex.apexd_test.apex", 125);

  // Query for its file
  auto result = GetStagedApexFiles(123, {124, 125});

  ASSERT_THAT(result, Ok());
  auto child_apex_file_1 = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", GetStagedDir(124).c_str()));
  auto child_apex_file_2 = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", GetStagedDir(125).c_str()));
  ASSERT_THAT(*result,
              UnorderedElementsAre(ApexFileEq(ByRef(*child_apex_file_1)),
                                   ApexFileEq(ByRef(*child_apex_file_2))));
}

// A test fixture to use for tests that mount/unmount apexes.
// This also supports test-purpose BlockApex via mount.
class ApexdMountTest : public ApexdUnitTest {
 public:
  ApexdMountTest() {
    vm_payload_disk_ = StringPrintf("%s/vm-payload", td_.path);
  }

 protected:
  void SetUp() override {
    ApexdUnitTest::SetUp();
    GetApexDatabaseForTesting().Reset();
    GetChangedActiveApexesForTesting().clear();
    ASSERT_THAT(SetUpApexTestEnvironment(), Ok());
  }

  void TearDown() override {
    SetBlockApexEnabled(false);
    auto activated = std::vector<std::string>{};
    GetApexDatabaseForTesting().ForallMountedApexes(
        [&](auto pkg, auto data, auto latest) {
          activated.push_back(data.full_path);
        });
    for (const auto& apex : activated) {
      if (auto status = DeactivatePackage(apex); !status.ok()) {
        LOG(ERROR) << "Failed to unmount " << apex << " : " << status.error();
      }
    }
    InitMetrics({});  // reset
    ApexdUnitTest::TearDown();
  }

  void SetBlockApexEnabled(bool enabled) {
    // The first partition(1) is "metadata" partition
    base::SetProperty(kTestVmPayloadMetadataPartitionProp,
                      enabled ? (vm_payload_disk_ + "1") : "");
  }

  std::string AddBlockApex(const std::string& apex_name,
                           const std::string& public_key = "",
                           const std::string& root_digest = "",
                           bool is_factory = true) {
    auto apex_path = vm_payload_disk_ + std::to_string(block_device_index_++);
    auto apex_file = GetTestFile(apex_name);
    AddToMetadata(apex_name, public_key, root_digest, is_factory);
    // block_apexes_ will be disposed after each test
    auto block_apex = WriteBlockApex(apex_file, apex_path);
    if (!block_apex.ok()) {
      PLOG(ERROR) << block_apex.error();
    }
    block_apexes_.push_back(std::move(*block_apex));
    return apex_path;
  }

  void AddToMetadata(const std::string& apex_name,
                     const std::string& public_key,
                     const std::string& root_digest, bool is_factory) {
    android::microdroid::Metadata metadata;
    // The first partition is metadata partition
    auto metadata_partition = vm_payload_disk_ + "1";
    if (access(metadata_partition.c_str(), F_OK) == 0) {
      auto result = android::microdroid::ReadMetadata(metadata_partition);
      ASSERT_THAT(result, Ok());
      metadata = *result;
    }

    auto apex = metadata.add_apexes();
    apex->set_name(apex_name);
    apex->set_public_key(public_key);
    apex->set_root_digest(root_digest);
    apex->set_is_factory(is_factory);

    std::ofstream out(metadata_partition);
    ASSERT_THAT(android::microdroid::WriteMetadata(metadata, out), Ok());
  }

 private:
  MountNamespaceRestorer restorer_;

  // Block APEX specific stuff.
  std::string vm_payload_disk_;
  int block_device_index_ = 2;  // "1" is reserved for metadata;
  // This should be freed before ~MountNamespaceRestorer() because it
  // switches to the original mount namespace while block apexes are mounted
  // in test-purpose mount namespace.
  std::vector<BlockApex> block_apexes_;
};

TEST_F(ApexdMountTest, CalculateSizeForCompressedApexEmptyList) {
  int64_t result = CalculateSizeForCompressedApex({});
  ASSERT_EQ(0LL, result);
}

TEST_F(ApexdMountTest, CalculateSizeForCompressedApex) {
  auto& instance = ApexFileRepository::GetInstance();
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  std::vector<std::tuple<std::string, int64_t, int64_t>> input = {
      std::make_tuple("new_apex", 1, 1),
      std::make_tuple("new_apex_2", 1, 2),
      std::make_tuple("com.android.apex.compressed", 1, 8),
  };
  int64_t result = CalculateSizeForCompressedApex(input);
  ASSERT_EQ(1 + 2 + 8LL, result);
}

TEST_F(
    ApexdMountTest,
    CalculateSizeForCompressedApex_SkipIfDataApexIsNewerThanOrEqualToPreInstalledApex) {
  auto& instance = ApexFileRepository::GetInstance();
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  AddDataApex("com.android.apex.compressed.v2_original.apex");
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());
  ASSERT_THAT(instance.AddDataApex(GetDataDir()), Ok());

  OnStart();

  std::vector<std::tuple<std::string, int64_t, int64_t>> input = {
      std::make_tuple("new_apex", 1, 1),
      std::make_tuple("com.android.apex.compressed", 2, 8),  // ignored
  };
  int64_t result = CalculateSizeForCompressedApex(input);
  ASSERT_EQ(1LL, result);
}

// TODO(b/187864524): cover other negative scenarios.
TEST_F(ApexdMountTest, InstallPackageRejectsApexWithoutRebootlessSupport) {
  std::string file_path = AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret =
      InstallPackage(GetTestFile("apex.apexd_test.apex"), /* force= */ false);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(HasSubstr("does not support non-staged update"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsNoPreInstalledApex) {
  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v1.apex"),
                            /* force= */ false);
  ASSERT_THAT(
      ret, HasError(WithMessage(HasSubstr(
               "No active version found for package test.apex.rebootless"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsNoActiveApex) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(
      ret, HasError(WithMessage(HasSubstr(
               "No active version found for package test.apex.rebootless"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsManifestMismatch) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret =
      InstallPackage(GetTestFile("test.rebootless_apex_manifest_mismatch.apex"),
                     /* force= */ false);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(HasSubstr(
          "Manifest inside filesystem does not match manifest outside it"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsCorrupted) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_corrupted.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret,
              HasError(WithMessage(HasSubstr("Can't verify /dev/block/dm-"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsProvidesSharedLibs) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(
      GetTestFile("test.rebootless_apex_provides_sharedlibs.apex"),
      /* force= */ false);
  ASSERT_THAT(ret, HasError(WithMessage(HasSubstr(" is a shared libs APEX"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsProvidesNativeLibs) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(
      GetTestFile("test.rebootless_apex_provides_native_libs.apex"),
      /* force= */ false);
  ASSERT_THAT(ret, HasError(WithMessage(HasSubstr(" provides native libs"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsRequiresSharedApexLibs) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(
      GetTestFile("test.rebootless_apex_requires_shared_apex_libs.apex"),
      /* force= */ false);
  ASSERT_THAT(ret,
              HasError(WithMessage(HasSubstr(" requires shared apex libs"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsJniLibs) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_jni_libs.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, HasError(WithMessage(HasSubstr(" requires JNI libs"))));
}

TEST_F(ApexdMountTest, InstallPackageAcceptsAddRequiredNativeLib) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret =
      InstallPackage(GetTestFile("test.rebootless_apex_add_native_lib.apex"),
                     /* force= */ false);
  ASSERT_THAT(ret, Ok());
}

TEST_F(ApexdMountTest, InstallPackageAcceptsRemoveRequiredNativeLib) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret =
      InstallPackage(GetTestFile("test.rebootless_apex_remove_native_lib.apex"),
                     /* force= */ false);
  ASSERT_THAT(ret, Ok());
}

TEST_F(ApexdMountTest, InstallPackageRejectsAppInApex) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(
      GetTestFile("test.rebootless_apex_app_in_apex.apex"), /* force= */ false);
  ASSERT_THAT(ret, HasError(WithMessage(HasSubstr("contains app inside"))));
}

TEST_F(ApexdMountTest, InstallPackageRejectsPrivAppInApex) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret =
      InstallPackage(GetTestFile("test.rebootless_apex_priv_app_in_apex.apex"),
                     /* force= */ false);
  ASSERT_THAT(ret,
              HasError(WithMessage(HasSubstr("contains priv-app inside"))));
}

TEST_F(ApexdMountTest, InstallPackagePreInstallVersionActive) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@2"));

  // Check that /apex/test.apex.rebootless is a bind mount of
  // /apex/test.apex.rebootless@2.
  auto manifest = ReadManifest("/apex/test.apex.rebootless/apex_manifest.pb");
  ASSERT_THAT(manifest, Ok());
  ASSERT_EQ(2u, manifest->version());

  // Check that GetActivePackage correctly reports upgraded version.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), ret->GetPath());

  // Check that pre-installed APEX is still around
  ASSERT_EQ(0, access(file_path.c_str(), F_OK))
      << "Can't access " << file_path << " : " << strerror(errno);

  auto& db = GetApexDatabaseForTesting();
  // Check that upgraded APEX is mounted on top of dm-verity device.
  db.ForallMountedApexes(
      "test.apex.rebootless", [&](const MountedApexData& data, bool latest) {
        ASSERT_TRUE(latest);
        ASSERT_EQ(data.full_path, ret->GetPath());
        ASSERT_EQ(data.device_name, "test.apex.rebootless@2_1");
      });
}

TEST_F(ApexdMountTest, InstallPackagePreInstallVersionActiveSamegrade) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v1.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@1"));

  // Check that GetActivePackage correctly reports upgraded version.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), ret->GetPath());

  // Check that pre-installed APEX is still around
  ASSERT_EQ(0, access(file_path.c_str(), F_OK))
      << "Can't access " << file_path << " : " << strerror(errno);

  auto& db = GetApexDatabaseForTesting();
  // Check that upgraded APEX is mounted on top of dm-verity device.
  db.ForallMountedApexes(
      "test.apex.rebootless", [&](const MountedApexData& data, bool latest) {
        ASSERT_TRUE(latest);
        ASSERT_EQ(data.full_path, ret->GetPath());
        ASSERT_EQ(data.device_name, "test.apex.rebootless@1_1");
      });
}

TEST_F(ApexdMountTest, InstallPackageUnloadOldApex) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  bool unloaded = false;
  bool loaded = false;
  const std::string prop = "apex.test.apex.rebootless.ready";
  std::thread monitor_apex_ready_prop([&]() {
    unloaded = base::WaitForProperty(prop, "false", 10s);
    loaded = base::WaitForProperty(prop, "true", 10s);
  });

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  monitor_apex_ready_prop.join();
  ASSERT_TRUE(unloaded);
  ASSERT_TRUE(loaded);
}

TEST_F(ApexdMountTest, InstallPackageWithService) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_service_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_service_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());
  auto manifest = ReadManifest("/apex/test.apex.rebootless/apex_manifest.pb");
  ASSERT_THAT(manifest, Ok());
  ASSERT_EQ(2u, manifest->version());
}

TEST_F(ApexdMountTest, InstallPackageDataVersionActive) {
  AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  std::string file_path = AddDataApex("test.rebootless_apex_v1.apex");
  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@2"));

  // Check that /apex/test.apex.rebootless is a bind mount of
  // /apex/test.apex.rebootless@2.
  auto manifest = ReadManifest("/apex/test.apex.rebootless/apex_manifest.pb");
  ASSERT_THAT(manifest, Ok());
  ASSERT_EQ(2u, manifest->version());

  // Check that GetActivePackage correctly reports upgraded version.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), ret->GetPath());

  // Check that previously active APEX was deleted.
  ASSERT_EQ(-1, access(file_path.c_str(), F_OK));
  ASSERT_EQ(ENOENT, errno);

  auto& db = GetApexDatabaseForTesting();
  // Check that upgraded APEX is mounted on top of dm-verity device.
  db.ForallMountedApexes(
      "test.apex.rebootless", [&](const MountedApexData& data, bool latest) {
        ASSERT_TRUE(latest);
        ASSERT_EQ(data.full_path, ret->GetPath());
        ASSERT_EQ(data.device_name, "test.apex.rebootless@2_1");
      });
}

TEST_F(ApexdMountTest, InstallPackageResolvesPathCollision) {
  AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  std::string file_path = AddDataApex("test.rebootless_apex_v1.apex",
                                      "test.apex.rebootless@1_1.apex");
  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v1.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@1"));

  // Check that /apex/test.apex.rebootless is a bind mount of
  // /apex/test.apex.rebootless@2.
  auto manifest = ReadManifest("/apex/test.apex.rebootless/apex_manifest.pb");
  ASSERT_THAT(manifest, Ok());
  ASSERT_EQ(1u, manifest->version());

  // Check that GetActivePackage correctly reports upgraded version.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), ret->GetPath());

  // Check that we correctly resolved active apex path collision.
  ASSERT_EQ(active_apex->GetPath(),
            GetDataDir() + "/test.apex.rebootless@1_2.apex");

  // Check that previously active APEX was deleted.
  ASSERT_EQ(-1, access(file_path.c_str(), F_OK));
  ASSERT_EQ(ENOENT, errno);

  auto& db = GetApexDatabaseForTesting();
  // Check that upgraded APEX is mounted on top of dm-verity device.
  db.ForallMountedApexes(
      "test.apex.rebootless", [&](const MountedApexData& data, bool latest) {
        ASSERT_TRUE(latest);
        ASSERT_EQ(data.full_path, ret->GetPath());
        ASSERT_EQ(data.device_name, "test.apex.rebootless@1_2");
      });
}

TEST_F(ApexdMountTest, InstallPackageDataVersionActiveSamegrade) {
  AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  std::string file_path = AddDataApex("test.rebootless_apex_v2.apex");
  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@2"));

  // Check that /apex/test.apex.rebootless is a bind mount of
  // /apex/test.apex.rebootless@2.
  auto manifest = ReadManifest("/apex/test.apex.rebootless/apex_manifest.pb");
  ASSERT_THAT(manifest, Ok());
  ASSERT_EQ(2u, manifest->version());

  // Check that GetActivePackage correctly reports upgraded version.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), ret->GetPath());

  // Check that previously active APEX was deleted.
  ASSERT_EQ(-1, access(file_path.c_str(), F_OK));
  ASSERT_EQ(ENOENT, errno);

  auto& db = GetApexDatabaseForTesting();
  // Check that upgraded APEX is mounted on top of dm-verity device.
  db.ForallMountedApexes(
      "test.apex.rebootless", [&](const MountedApexData& data, bool latest) {
        ASSERT_TRUE(latest);
        ASSERT_EQ(data.full_path, ret->GetPath());
        ASSERT_EQ(data.device_name, "test.apex.rebootless@2_1");
      });
}

TEST_F(ApexdMountTest, InstallPackageUnmountFailsPreInstalledApexActive) {
  std::string file_path = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  unique_fd fd(open("/apex/test.apex.rebootless/apex_manifest.pb",
                    O_RDONLY | O_CLOEXEC));
  ASSERT_NE(-1, fd.get());

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Not(Ok()));

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@1"));

  // Check that GetActivePackage correctly reports upgraded version.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), file_path);

  // Check that old APEX is still around
  ASSERT_EQ(0, access(file_path.c_str(), F_OK))
      << "Can't access " << file_path << " : " << strerror(errno);

  auto& db = GetApexDatabaseForTesting();
  // Check that upgraded APEX is mounted on top of dm-verity device.
  db.ForallMountedApexes("test.apex.rebootless",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, file_path);
                         });
}

TEST_F(ApexdMountTest, InstallPackageUnmountFailedUpdatedApexActive) {
  AddPreInstalledApex("test.rebootless_apex_v1.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  std::string file_path = AddDataApex("test.rebootless_apex_v1.apex");

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  {
    auto active_apex = GetActivePackage("test.apex.rebootless");
    ASSERT_THAT(active_apex, Ok());
    ASSERT_EQ(active_apex->GetPath(), file_path);
  }

  unique_fd fd(open("/apex/test.apex.rebootless/apex_manifest.pb",
                    O_RDONLY | O_CLOEXEC));
  ASSERT_NE(-1, fd.get());

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Not(Ok()));

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/test.apex.rebootless",
                                   "/apex/test.apex.rebootless@1"));

  // Check that GetActivePackage correctly reports old apex.
  auto active_apex = GetActivePackage("test.apex.rebootless");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), file_path);

  // Check that old APEX is still around
  ASSERT_EQ(0, access(file_path.c_str(), F_OK))
      << "Can't access " << file_path << " : " << strerror(errno);

  auto& db = GetApexDatabaseForTesting();
  db.ForallMountedApexes(
      "test.apex.rebootless", [&](const MountedApexData& data, bool latest) {
        ASSERT_TRUE(latest);
        ASSERT_EQ(data.full_path, file_path);
        ASSERT_EQ(data.device_name, "test.apex.rebootless@1");
      });
}

TEST_F(ApexdMountTest, InstallPackageUpdatesApexInfoList) {
  auto apex_1 = AddPreInstalledApex("test.rebootless_apex_v1.apex");
  auto apex_2 = AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(apex_1), Ok());
  ASSERT_THAT(ActivatePackage(apex_2), Ok());

  // Call OnAllPackagesActivated to create /apex/apex-info-list.xml.
  OnAllPackagesActivated(/* is_bootstrap= */ false);
  // Check /apex/apex-info-list.xml was created.
  ASSERT_EQ(0, access("/apex/apex-info-list.xml", F_OK));

  auto ret = InstallPackage(GetTestFile("test.rebootless_apex_v2.apex"),
                            /* force= */ false);
  ASSERT_THAT(ret, Ok());

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "test.apex.rebootless",
      /* modulePath= */ apex_1,
      /* preinstalledModulePath= */ apex_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_2, /* preinstalledModulePath= */ apex_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "test.apex.rebootless",
      /* modulePath= */ ret->GetPath(),
      /* preinstalledModulePath= */ apex_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(ret->GetPath()),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));
}

TEST_F(ApexdMountTest, ActivatePackageBannedName) {
  auto status = ActivatePackage(GetTestFile("sharedlibs.apex"));
  ASSERT_THAT(status,
              HasError(WithMessage("Package name sharedlibs is not allowed.")));
}

TEST_F(ApexdMountTest, ActivatePackageNoCode) {
  std::string file_path = AddPreInstalledApex("apex.apexd_test_nocode.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  std::string mountinfo;
  ASSERT_TRUE(ReadFileToString("/proc/self/mountinfo", &mountinfo));
  bool found_apex_mountpoint = false;
  for (const auto& line : Split(mountinfo, "\n")) {
    std::vector<std::string> tokens = Split(line, " ");
    // line format:
    // mnt_id parent_mnt_id major:minor source target option propagation_type
    // ex) 33 260:19 / /apex rw,nosuid,nodev -
    if (tokens.size() >= 7 &&
        tokens[4] == "/apex/com.android.apex.test_package@1") {
      found_apex_mountpoint = true;
      // Make sure that option contains noexec
      std::vector<std::string> options = Split(tokens[5], ",");
      EXPECT_THAT(options, Contains("noexec"));
      break;
    }
  }
  EXPECT_TRUE(found_apex_mountpoint);
}

TEST_F(ApexdMountTest, ActivatePackageManifestMissmatch) {
  std::string file_path =
      AddPreInstalledApex("apex.apexd_test_manifest_mismatch.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  auto status = ActivatePackage(file_path);
  ASSERT_THAT(
      status,
      HasError(WithMessage(HasSubstr(
          "Manifest inside filesystem does not match manifest outside it"))));
}

TEST_F(ApexdMountTest, ActivatePackage) {
  std::string file_path = AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto active_apex = GetActivePackage("com.android.apex.test_package");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), file_path);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  ASSERT_THAT(DeactivatePackage(file_path), Ok());
  ASSERT_THAT(GetActivePackage("com.android.apex.test_package"), Not(Ok()));

  auto new_apex_mounts = GetApexMounts();
  ASSERT_EQ(new_apex_mounts.size(), 0u);
}

TEST_F(ApexdMountTest, ActivatePackageShowsUpInMountedApexDatabase) {
  std::string file_path = AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto active_apex = GetActivePackage("com.android.apex.test_package");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), file_path);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  // Check that mounted apex database contains information about our APEX.
  auto& db = GetApexDatabaseForTesting();
  std::optional<MountedApexData> mounted_apex;
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& d, bool active) {
                           if (active) {
                             mounted_apex.emplace(d);
                           }
                         });
  ASSERT_TRUE(mounted_apex)
      << "Haven't found com.android.apex.test_package in the database of "
      << "mounted apexes";
}

TEST_F(ApexdMountTest, DeactivePackageFreesLoopDevices) {
  AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  std::string file_path = AddDataApex("apex.apexd_test_v2.apex");
  ASSERT_THAT(ActivatePackage(file_path), Ok());

  // Get loop devices that were used to mount APEX.
  auto children = ListChildLoopDevices("com.android.apex.test_package@2");
  ASSERT_THAT(children, Ok());
  ASSERT_EQ(1u, children->size())
      << "Unexpected number of children: " << Join(*children, ",");

  ASSERT_THAT(DeactivatePackage(file_path), Ok());
  for (const auto& loop : *children) {
    struct loop_info li;
    unique_fd fd(TEMP_FAILURE_RETRY(open(loop.c_str(), O_RDWR | O_CLOEXEC)));
    EXPECT_NE(-1, fd.get())
        << "Failed to open " << loop << " : " << strerror(errno);
    EXPECT_EQ(-1, ioctl(fd.get(), LOOP_GET_STATUS, &li))
        << loop << " is still alive";
    EXPECT_EQ(ENXIO, errno) << "Unexpected errno : " << strerror(errno);
  }
}

TEST_F(ApexdMountTest, DeactivePackageTearsDownVerityDevice) {
  AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  std::string file_path = AddDataApex("apex.apexd_test_v2.apex");
  ASSERT_THAT(ActivatePackage(file_path), Ok());

  ASSERT_THAT(DeactivatePackage(file_path), Ok());
  auto& dm = DeviceMapper::Instance();
  ASSERT_EQ(dm::DmDeviceState::INVALID,
            dm.GetState("com.android.apex.test_package@2"));
}

TEST_F(ApexdMountTest, ActivateDeactivateSharedLibsApex) {
  ASSERT_EQ(mkdir("/apex/sharedlibs", 0755), 0);
  ASSERT_EQ(mkdir("/apex/sharedlibs/lib", 0755), 0);
  ASSERT_EQ(mkdir("/apex/sharedlibs/lib64", 0755), 0);
  auto deleter = make_scope_guard([]() {
    std::error_code ec;
    fs::remove_all("/apex/sharedlibs", ec);
    if (ec) {
      LOG(ERROR) << "Failed to delete /apex/sharedlibs : " << ec;
    }
  });

  std::string file_path = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto active_apex = GetActivePackage("com.android.apex.test.sharedlibs");
  ASSERT_THAT(active_apex, Ok());
  ASSERT_EQ(active_apex->GetPath(), file_path);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test.sharedlibs@1"));

  ASSERT_THAT(DeactivatePackage(file_path), Ok());
  ASSERT_THAT(GetActivePackage("com.android.apex.test.sharedlibs"), Not(Ok()));

  auto new_apex_mounts = GetApexMounts();
  ASSERT_EQ(new_apex_mounts.size(), 0u);
}

TEST_F(ApexdMountTest, RemoveInactiveDataApex) {
  AddPreInstalledApex("com.android.apex.compressed.v2.capex");
  // Add a decompressed apex that will not be mounted, so should be removed
  auto decompressed_apex = StringPrintf("%s/com.android.apex.compressed@1%s",
                                        GetDecompressionDir().c_str(),
                                        kDecompressedApexPackageSuffix);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.apex"),
           decompressed_apex);
  // Add a decompressed apex that will be mounted, so should be not be removed
  auto active_decompressed_apex = StringPrintf(
      "%s/com.android.apex.compressed@2%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  fs::copy(GetTestFile("com.android.apex.compressed.v2_original.apex"),
           active_decompressed_apex);
  // Apex that do not have kDecompressedApexPackageSuffix, should not be removed
  // from decompression_dir
  auto decompressed_different_suffix =
      StringPrintf("%s/com.android.apex.compressed@2%s",
                   GetDecompressionDir().c_str(), kApexPackageSuffix);
  fs::copy(GetTestFile("com.android.apex.compressed.v2_original.apex"),
           decompressed_different_suffix);

  AddPreInstalledApex("apex.apexd_test.apex");
  auto data_apex = AddDataApex("apex.apexd_test.apex");
  auto active_data_apex = AddDataApex("apex.apexd_test_v2.apex");

  // Activate some of the apex
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});
  ASSERT_THAT(ActivatePackage(active_decompressed_apex), Ok());
  ASSERT_THAT(ActivatePackage(active_data_apex), Ok());
  // Clean up inactive apex packages
  RemoveInactiveDataApex();

  // Verify inactive apex packages have been deleted
  ASSERT_TRUE(*PathExists(active_decompressed_apex));
  ASSERT_TRUE(*PathExists(active_data_apex));
  ASSERT_TRUE(*PathExists(decompressed_different_suffix));
  ASSERT_FALSE(*PathExists(decompressed_apex));
  ASSERT_FALSE(*PathExists(data_apex));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapOnlyPreInstalledApexes) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapFailsToScanPreInstalledApexes) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("apex.apexd_test_corrupt_superblock_apex.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 1);
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasHigherVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(apex_path_3),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasSameVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(apex_path_3),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSystemHasHigherVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test_v2.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasSameVersionButDifferentKey) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test_different_key.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapDataHasHigherVersionButDifferentKey) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 =
      AddDataApex("apex.apexd_test_different_key_v2.apex");

  {
    auto apex = ApexFile::Open(apex_path_3);
    ASSERT_THAT(apex, Ok());
    ASSERT_EQ(static_cast<uint64_t>(apex->GetManifest().version()), 2ULL);
  }

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataApexWithoutPreInstalledApex) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  AddDataApex("apex.apexd_test_different_app.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapPreInstalledSharedLibsApex) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test.sharedlibs@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test.sharedlibs",
      /* modulePath= */ apex_path_2,
      /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(apex_path_3),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));

  ASSERT_EQ(access("/apex/sharedlibs", F_OK), 0);

  // Check /apex/sharedlibs is populated properly.
  std::vector<std::string> sharedlibs;
  for (const auto& p : fs::recursive_directory_iterator("/apex/sharedlibs")) {
    if (fs::is_symlink(p)) {
      auto src = fs::read_symlink(p.path());
      ASSERT_EQ(p.path().filename(), src.filename());
      sharedlibs.push_back(p.path().parent_path().string() + "->" +
                           src.parent_path().string());
    }
  }

  std::vector<std::string> expected = {
      "/apex/sharedlibs/lib/libsharedlibtest.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libsharedlibtest.so",
      "/apex/sharedlibs/lib/libc++.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libc++.so",
  };

  // On 64bit devices we also have lib64.
  if (!GetProperty("ro.product.cpu.abilist64", "").empty()) {
    expected.push_back(
        "/apex/sharedlibs/lib64/libsharedlibtest.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libsharedlibtest.so");
    expected.push_back(
        "/apex/sharedlibs/lib64/libc++.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libc++.so");
  }
  ASSERT_THAT(sharedlibs, UnorderedElementsAreArray(expected));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSharedLibsApexBothVersions) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");
  std::string apex_path_4 =
      AddDataApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test.sharedlibs@1",
                                   "/apex/com.android.apex.test.sharedlibs@2"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test.sharedlibs",
      /* modulePath= */ apex_path_2,
      /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(apex_path_3),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_4 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test.sharedlibs",
      /* modulePath= */ apex_path_4,
      /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(apex_path_4),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3),
                                   ApexInfoXmlEq(apex_info_xml_4)));

  ASSERT_EQ(access("/apex/sharedlibs", F_OK), 0);

  // Check /apex/sharedlibs is populated properly.
  // Because we don't want to hardcode full paths (they are pretty long and have
  // a hash in them which might change if new prebuilts are dropped in), the
  // assertion logic is a little bit clunky.
  std::vector<std::string> sharedlibs;
  for (const auto& p : fs::recursive_directory_iterator("/apex/sharedlibs")) {
    if (fs::is_symlink(p)) {
      auto src = fs::read_symlink(p.path());
      ASSERT_EQ(p.path().filename(), src.filename());
      sharedlibs.push_back(p.path().parent_path().string() + "->" +
                           src.parent_path().string());
    }
  }

  std::vector<std::string> expected = {
      "/apex/sharedlibs/lib/libsharedlibtest.so->"
      "/apex/com.android.apex.test.sharedlibs@2/lib/libsharedlibtest.so",
      "/apex/sharedlibs/lib/libsharedlibtest.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libsharedlibtest.so",
      "/apex/sharedlibs/lib/libc++.so->"
      "/apex/com.android.apex.test.sharedlibs@2/lib/libc++.so",
  };
  // On 64bit devices we also have lib64.
  if (!GetProperty("ro.product.cpu.abilist64", "").empty()) {
    expected.push_back(
        "/apex/sharedlibs/lib64/libsharedlibtest.so->"
        "/apex/com.android.apex.test.sharedlibs@2/lib64/libsharedlibtest.so");
    expected.push_back(
        "/apex/sharedlibs/lib64/libsharedlibtest.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libsharedlibtest.so");
    expected.push_back(
        "/apex/sharedlibs/lib64/libc++.so->"
        "/apex/com.android.apex.test.sharedlibs@2/lib64/libc++.so");
  }

  ASSERT_THAT(sharedlibs, UnorderedElementsAreArray(expected));
}

// Test when we move from uncompressed APEX to CAPEX via ota
TEST_F(ApexdMountTest, OnOtaChrootBootstrapOnlyCompressedApexes) {
  std::string apex_path =
      AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Decompressed APEX should be mounted from decompression_dir
  std::string decompressed_apex =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(decompressed_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@1.chroot");
                         });
}

// Test we decompress only once even if OnOtaChrootBootstrap is called multiple
// times
TEST_F(ApexdMountTest, OnOtaChrootBootstrapDecompressOnlyOnceMultipleCalls) {
  std::string apex_path =
      AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Decompressed OTA APEX should be mounted
  std::string decompressed_ota_apex =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  // Capture the creation time of the OTA APEX
  std::error_code ec;
  auto last_write_time_1 = fs::last_write_time(decompressed_ota_apex, ec);
  ASSERT_FALSE(ec) << "Failed to capture last write time of "
                   << decompressed_ota_apex;

  // Call OnOtaChrootBootstrap again. Since we do not hardlink decompressed APEX
  // to /data/apex/active directory when in chroot, when selecting apex for
  // activation, we will end up selecting compressed APEX again.
  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Compare write time to ensure we did not decompress again
  auto last_write_time_2 = fs::last_write_time(decompressed_ota_apex, ec);
  ASSERT_FALSE(ec) << "Failed to capture last write time of "
                   << decompressed_ota_apex << ec.message();
  ASSERT_EQ(last_write_time_1, last_write_time_2);
}

// Test when we upgrade existing CAPEX to higher version via OTA
TEST_F(ApexdMountTest, OnOtaChrootBootstrapUpgradeCapex) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed.v1.capex",
                        previous_built_in_dir.path);
  // Place a higher version capex in current built_in_dir
  std::string apex_path =
      AddPreInstalledApex("com.android.apex.compressed.v2.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Upgraded decompressed APEX should be mounted from decompression dir
  std::string decompressed_active_apex =
      StringPrintf("%s/com.android.apex.compressed@2%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@2"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_active_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_active_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@2.chroot");
                         });
}

// Test when we update existing CAPEX to same version via OTA
TEST_F(ApexdMountTest, OnOtaChrootBootstrapSamegradeCapex) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed.v1.capex",
                        previous_built_in_dir.path);
  // Place a same version capex in current built_in_dir, under a different name
  auto apex_path =
      StringPrintf("%s/different-name.capex", GetBuiltInDir().c_str());
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"), apex_path);

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Previously decompressed APEX should be mounted from decompression_dir
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_active_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_active_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@1.chroot");
                         });
}

// Test when we update existing CAPEX to same version, but different digest
TEST_F(ApexdMountTest, OnOtaChrootBootstrapSamegradeCapexDifferentDigest) {
  TemporaryDir previous_built_in_dir;
  auto [different_digest_apex_path, _] = PrepareCompressedApex(
      "com.android.apex.compressed.v1_different_digest.capex",
      previous_built_in_dir.path);
  // Place a same version capex in current built_in_dir, which has different
  // digest
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // New decompressed ota APEX should be mounted with kOtaApexPackageSuffix
  std::string decompressed_ota_apex =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_ota_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_ota_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_ota_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@1.chroot");
                         });

  // Ensure decompressed apex has same digest as pre-installed
  auto pre_installed_apex = ApexFile::Open(apex_path);
  auto decompressed_apex = ApexFile::Open(decompressed_ota_apex);
  auto different_digest_apex = ApexFile::Open(different_digest_apex_path);
  ASSERT_EQ(
      pre_installed_apex->GetManifest().capexmetadata().originalapexdigest(),
      GetRootDigest(*decompressed_apex));
  ASSERT_NE(
      pre_installed_apex->GetManifest().capexmetadata().originalapexdigest(),
      GetRootDigest(*different_digest_apex));

  // Ensure we didn't remove previous decompressed APEX
  std::string previous_decompressed_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  auto path_exists = PathExists(previous_decompressed_apex);
  ASSERT_TRUE(*path_exists);
}

// Test when we update existing CAPEX to same version, but different key via OTA
TEST_F(ApexdMountTest, OnOtaChrootBootstrapSamegradeCapexDifferentKey) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed_different_key.capex",
                        previous_built_in_dir.path);
  // Place a same version capex in current built_in_dir, which has different key
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // New decompressed APEX should be mounted from ota_reserved directory
  std::string decompressed_active_apex =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_active_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_active_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@1.chroot");
                         });
}

// Test when we remove CAPEX via OTA
TEST_F(ApexdMountTest, OnOtaChrootBootstrapCapexToApex) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed.v1.capex",
                        previous_built_in_dir.path);
  // Place a uncompressed version apex in current built_in_dir
  std::string apex_path =
      AddPreInstalledApex("com.android.apex.compressed.v1.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // New uncompressed APEX should be mounted

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_uncompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ apex_path,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_uncompressed)));
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapDecompressedApexVersionDifferentThanCapex) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed.v2.capex",
                        previous_built_in_dir.path);
  // Place a lower version capex in current built_in_dir, so that previously
  // decompressed APEX has higher version but still doesn't get picked during
  // selection.
  std::string apex_path =
      AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Pre-installed CAPEX should be decompressed again and mounted from
  // decompression_dir
  std::string decompressed_active_apex =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_active_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_active_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
}

// Test when we update CAPEX and there is a higher version present in data
TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHigherThanCapex) {
  auto [system_apex_path, _] =
      PrepareCompressedApex("com.android.apex.compressed.v1.capex");
  auto data_apex_path =
      AddDataApex("com.android.apex.compressed.v2_original.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Data APEX should be mounted

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@2"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_data = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ data_apex_path,
      /* preinstalledModulePath= */ system_apex_path,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(data_apex_path),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_system = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ system_apex_path,
      /* preinstalledModulePath= */ system_apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(system_apex_path),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_data),
                                   ApexInfoXmlEq(apex_info_xml_system)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, data_apex_path);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@2.chroot");
                         });
}

// Test when we update CAPEX and there is a lower version present in data
TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataLowerThanCapex) {
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v2.capex");
  AddDataApex("com.android.apex.compressed.v1.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Decompressed APEX should be mounted from reserved dir
  std::string decompressed_active_apex =
      StringPrintf("%s/com.android.apex.compressed@2%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@2"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_active_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_active_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@2.chroot");
                         });
}

// Test when we update CAPEX and there is a same version present in data
TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataSameAsCapex) {
  auto [system_apex_path, _] =
      PrepareCompressedApex("com.android.apex.compressed.v1.capex");
  auto data_apex_path = AddDataApex("com.android.apex.compressed.v1.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // Data APEX should be mounted

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_data = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ data_apex_path,
      /* preinstalledModulePath= */ system_apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ false, /* isActive= */ true, GetMTime(data_apex_path),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_system = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ system_apex_path,
      /* preinstalledModulePath= */ system_apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(system_apex_path),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_data),
                                   ApexInfoXmlEq(apex_info_xml_system)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, data_apex_path);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@1.chroot");
                         });
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasDifferentKeyThanCapex) {
  AddDataApex("com.android.apex.compressed_different_key.capex");
  // Place a same version capex in current built_in_dir, which has different key
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  // New decompressed APEX should be mounted from ota_reserved directory
  std::string decompressed_active_apex =
      StringPrintf("%s/com.android.apex.compressed@1%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_decompressed = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.compressed",
      /* modulePath= */ decompressed_active_apex,
      /* preinstalledModulePath= */ apex_path,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true,
      GetMTime(decompressed_active_apex),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_decompressed)));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed@1.chroot");
                         });
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSystemDataStagedInSameVersion) {
  // The APEXes on system, data, and staged are all in the same version. The
  // staged one should be picked.
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  AddDataApex("apex.apexd_test.apex");
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);
  std::string apex_path_3 =
      GetStagedDir(apex_session->GetId()) + "/" + "apex.apexd_test.apex";

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/true), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3, /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ false,
      /* isActive= */ true, GetMTime(apex_path_3),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSystemNewerThanDataStaged) {
  // The system one is newer than the data one and the staged one. The system
  // one should be picked.
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test_v2.apex");
  AddDataApex("apex.apexd_test.apex");
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/true), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml)));
}

static std::string GetSelinuxContext(const std::string& file) {
  char* ctx;
  if (getfilecon(file.c_str(), &ctx) < 0) {
    PLOG(ERROR) << "Failed to getfilecon " << file;
    return "";
  }
  std::string result(ctx);
  freecon(ctx);
  return result;
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSelinuxLabelsAreCorrect) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  EXPECT_EQ(GetSelinuxContext("/apex/apex-info-list.xml"),
            "u:object_r:apex_info_file:s0");

  EXPECT_EQ(GetSelinuxContext("/apex/sharedlibs"),
            "u:object_r:apex_mnt_dir:s0");

  EXPECT_EQ(GetSelinuxContext("/apex/com.android.apex.test_package"),
            "u:object_r:system_file:s0");
  EXPECT_EQ(GetSelinuxContext("/apex/com.android.apex.test_package@2"),
            "u:object_r:system_file:s0");
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDmDevicesHaveCorrectName) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  MountedApexDatabase& db = GetApexDatabaseForTesting();
  // com.android.apex.test_package_2 should be mounted directly on top of loop
  // device.
  db.ForallMountedApexes("com.android.apex.test_package_2",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_THAT(data.device_name, IsEmpty());
                           ASSERT_THAT(data.loop_name, StartsWith("/dev"));
                         });
  // com.android.apex.test_package should be mounted on top of dm-verity device.
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.test_package@2.chroot");
                           ASSERT_THAT(data.loop_name, StartsWith("/dev"));
                         });
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapFailsToActivatePreInstalledApexKeepsGoing) {
  std::string apex_path_1 =
      AddPreInstalledApex("apex.apexd_test_manifest_mismatch.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 137, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapFailsToActivateDataApexFallsBackToPreInstalled) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 =
      AddDataApex("apex.apexd_test_manifest_mismatch.apex");

  ASSERT_EQ(OnOtaChrootBootstrap(/*also_include_staged_apexes=*/false), 0);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(apex_path_1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true, GetMTime(apex_path_2),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetPartitionString());

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnStartOnlyPreInstalledApexes) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));
}

TEST_F(ApexdMountTest, OnStartDataHasHigherVersion) {
  AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));
}

TEST_F(ApexdMountTest, OnStartDataHasWrongSHA) {
  std::string apex_path = AddPreInstalledApex("com.android.apex.cts.shim.apex");
  AddDataApex("com.android.apex.cts.shim.v2_wrong_sha.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Check system shim apex is activated instead of the data one.
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.cts.shim",
                                   "/apex/com.android.apex.cts.shim@1"));
}

TEST_F(ApexdMountTest, OnStartDataHasSameVersion) {
  AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from data apex, not pre-installed one.
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path_3);
                         });
}

TEST_F(ApexdMountTest, OnStartSystemHasHigherVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test_v2.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from pre-installed one.
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path_1);
                         });
}

TEST_F(ApexdMountTest, OnStartFailsToActivateApexOnDataFallsBackToBuiltIn) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test_manifest_mismatch.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from pre-installed apex.
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path_1);
                         });
}

TEST_F(ApexdMountTest, OnStartApexOnDataHasWrongKeyFallsBackToBuiltIn) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 =
      AddDataApex("apex.apexd_test_different_key_v2.apex");

  {
    auto apex = ApexFile::Open(apex_path_3);
    ASSERT_THAT(apex, Ok());
    ASSERT_EQ(static_cast<uint64_t>(apex->GetManifest().version()), 2ULL);
  }

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from pre-installed apex.
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path_1);
                         });
}

TEST_F(ApexdMountTest, OnStartOnlyPreInstalledCapexes) {
  std::string apex_path_1 =
      AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Decompressed APEX should be mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

TEST_F(ApexdMountTest, OnStartDataHasHigherVersionThanCapex) {
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  std::string apex_path_2 =
      AddDataApex("com.android.apex.compressed.v2_original.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@2"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from data apex.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path_2);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

TEST_F(ApexdMountTest, OnStartDataHasSameVersionAsCapex) {
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  std::string apex_path_2 = AddDataApex("com.android.apex.compressed.v1.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Data APEX should be mounted

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));

  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from data apex, not pre-installed one.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path_2);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

TEST_F(ApexdMountTest, OnStartSystemHasHigherVersionCapexThanData) {
  std::string apex_path_1 =
      AddPreInstalledApex("com.android.apex.compressed.v2.capex");
  AddDataApex("com.android.apex.compressed.v1.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Decompressed APEX should be mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@2%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@2"));

  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from compressed apex
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

TEST_F(ApexdMountTest, OnStartFailsToActivateApexOnDataFallsBackToCapex) {
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  AddDataApex("com.android.apex.compressed.v2_manifest_mismatch.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Decompressed APEX should be mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex. It should also be mounted
  // on dm-verity device.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

// Test scenario when we fallback to capex but it already has a decompressed
// version on data
TEST_F(ApexdMountTest, OnStartFallbackToAlreadyDecompressedCapex) {
  PrepareCompressedApex("com.android.apex.compressed.v1.capex");
  AddDataApex("com.android.apex.compressed.v2_manifest_mismatch.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Decompressed APEX should be mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

// Test scenario when we fallback to capex but it has same version as corrupt
// data apex
TEST_F(ApexdMountTest, OnStartFallbackToCapexSameVersion) {
  AddPreInstalledApex("com.android.apex.compressed.v2.capex");
  // Add data apex using the common naming convention for /data/apex/active
  // directory
  fs::copy(GetTestFile("com.android.apex.compressed.v2_manifest_mismatch.apex"),
           GetDataDir() + "/com.android.apex.compressed@2.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Decompressed APEX should be mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@2%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@2"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

TEST_F(ApexdMountTest, OnStartCapexToApex) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed.v1.capex",
                        previous_built_in_dir.path);
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Uncompressed APEX should be mounted

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from decompressed apex.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path);
                           ASSERT_THAT(data.device_name, IsEmpty());
                         });
}

// Test to ensure we do not mount decompressed APEX from /data/apex/active
TEST_F(ApexdMountTest, OnStartOrphanedDecompressedApexInActiveDirectory) {
  // Place a decompressed APEX in /data/apex/active. This apex should not
  // be mounted since it's not in correct location. Instead, the
  // pre-installed APEX should be mounted.
  auto decompressed_apex_in_active_dir =
      StringPrintf("%s/com.android.apex.compressed@1%s", GetDataDir().c_str(),
                   kDecompressedApexPackageSuffix);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.apex"),
           decompressed_apex_in_active_dir);
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Pre-installed APEX should be mounted
  auto& db = GetApexDatabaseForTesting();
  // Check that pre-installed APEX has been activated
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, apex_path);
                           ASSERT_THAT(data.device_name, IsEmpty());
                         });
}

// Test scenario when decompressed version has different version than
// pre-installed CAPEX
TEST_F(ApexdMountTest, OnStartDecompressedApexVersionDifferentThanCapex) {
  TemporaryDir previous_built_in_dir;
  PrepareCompressedApex("com.android.apex.compressed.v2.capex",
                        previous_built_in_dir.path);
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Existing higher version decompressed APEX should be ignored and new
  // pre-installed CAPEX should be decompressed and mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "starting");
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1"));
  auto& db = GetApexDatabaseForTesting();
  // Check that it was mounted from newly decompressed apex.
  db.ForallMountedApexes("com.android.apex.compressed",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.full_path, decompressed_active_apex);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.compressed");
                         });
}

// Test that ota_apex is persisted until slot switch
TEST_F(ApexdMountTest, OnStartOtaApexKeptUntilSlotSwitch) {
  // Imagine current system has v1 capex and we have v2 incoming via ota
  auto old_capex = AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  auto ota_apex_path =
      StringPrintf("%s/com.android.apex.compressed@2%s",
                   GetDecompressionDir().c_str(), kOtaApexPackageSuffix);
  fs::copy(GetTestFile("com.android.apex.compressed.v2_original.apex"),
           ota_apex_path.c_str());

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  // When we call OnStart for the first time, it will decompress v1 capex and
  // activate it, while after second call it will decompress v2 capex and
  // activate it. We need to make sure that activated APEXes are cleaned up
  // after test finishes.
  auto old_decompressed_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  auto new_decompressed_apex = StringPrintf(
      "%s/com.android.apex.compressed@2%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  // First try starting without slot switch. Since we are booting with
  // old pre-installed capex, ota_apex should not be deleted
  OnStart();
  auto path_exists = PathExists(ota_apex_path);
  ASSERT_TRUE(*path_exists);

  // When we switch slot, the pre-installed APEX will match ota_apex
  // and the ota_apex will end up getting renamed.
  RemoveFileIfExists(old_capex);
  AddPreInstalledApex("com.android.apex.compressed.v2.capex");
  ApexFileRepository::GetInstance().Reset(GetDecompressionDir());
  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());
  OnStart();
  path_exists = PathExists(ota_apex_path);
  ASSERT_FALSE(*path_exists);
}

// Test scenario when decompressed version has same version but different
// digest
TEST_F(ApexdMountTest,
       OnStartDecompressedApexVersionSameAsCapexDifferentDigest) {
  // Push a CAPEX to system without decompressing it
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  auto pre_installed_apex = ApexFile::Open(apex_path);
  // Now push an APEX with different root digest as decompressed APEX
  auto decompressed_apex_path = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  fs::copy(GetTestFile(
               "com.android.apex.compressed.v1_different_digest_original.apex"),
           decompressed_apex_path);
  auto different_digest_apex = ApexFile::Open(decompressed_apex_path);
  auto different_digest = GetRootDigest(*different_digest_apex);
  ASSERT_NE(
      pre_installed_apex->GetManifest().capexmetadata().originalapexdigest(),
      different_digest);

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Existing same version decompressed APEX with different root digest should
  // be ignored and the pre-installed CAPEX should be decompressed again.

  // Ensure decompressed apex has same digest as pre-installed
  auto decompressed_apex = ApexFile::Open(decompressed_apex_path);
  ASSERT_EQ(
      pre_installed_apex->GetManifest().capexmetadata().originalapexdigest(),
      GetRootDigest(*decompressed_apex));
  ASSERT_NE(GetRootDigest(*decompressed_apex), different_digest);
}

// Test when decompressed APEX has different key than CAPEX
TEST_F(ApexdMountTest, OnStartDecompressedApexVersionSameAsCapexDifferentKey) {
  TemporaryDir previous_built_in_dir;
  auto [different_key_apex_path, _] =
      PrepareCompressedApex("com.android.apex.compressed_different_key.capex",
                            previous_built_in_dir.path);
  // Place a same version capex in current built_in_dir, which has different key
  auto apex_path = AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Existing same version decompressed APEX should be ignored and new
  // pre-installed CAPEX should be decompressed and mounted
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  // Ensure decompressed apex has same digest as pre-installed
  auto pre_installed_apex = ApexFile::Open(apex_path);
  auto decompressed_apex = ApexFile::Open(decompressed_active_apex);
  auto different_key_apex = ApexFile::Open(different_key_apex_path);
  ASSERT_EQ(
      pre_installed_apex->GetManifest().capexmetadata().originalapexdigest(),
      GetRootDigest(*decompressed_apex));
  ASSERT_NE(
      pre_installed_apex->GetManifest().capexmetadata().originalapexdigest(),
      GetRootDigest(*different_key_apex));
}

TEST_F(ApexdMountTest, PopulateFromMountsChecksPathPrefix) {
  AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path = AddDataApex("apex.apexd_test_v2.apex");

  // Mount an apex from decomrpession_dir
  PrepareCompressedApex("com.android.apex.compressed.v1.capex");
  std::string decompressed_apex =
      StringPrintf("%s/com.android.apex.compressed@1.decompressed.apex",
                   GetDecompressionDir().c_str());

  // Mount an apex from some other directory
  TemporaryDir td;
  AddPreInstalledApex("apex.apexd_test_different_app.apex");
  fs::copy(GetTestFile("apex.apexd_test_different_app.apex"), td.path);
  std::string other_apex =
      StringPrintf("%s/apex.apexd_test_different_app.apex", td.path);

  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  ASSERT_THAT(ActivatePackage(apex_path), Ok());
  ASSERT_THAT(ActivatePackage(decompressed_apex), Ok());
  ASSERT_THAT(ActivatePackage(other_apex), Ok());

  auto& db = GetApexDatabaseForTesting();
  // Remember mount information for |other_apex|, since it won't be available in
  // the database. We will need to tear it down manually.
  std::optional<MountedApexData> other_apex_mount_data;
  db.ForallMountedApexes(
      "com.android.apex.test_package_2",
      [&other_apex_mount_data](const MountedApexData& data, bool latest) {
        if (latest) {
          other_apex_mount_data.emplace(data);
        }
      });
  ASSERT_TRUE(other_apex_mount_data.has_value());
  auto deleter = make_scope_guard([&other_apex_mount_data]() {
    if (!other_apex_mount_data.has_value()) {
      return;
    }
    if (umount2("/apex/com.android.apex.test_package_2", 0) != 0) {
      PLOG(ERROR) << "Failed to unmount /apex/com.android.apex.test_package_2";
    }
    auto res = Unmount(*other_apex_mount_data, /* deferred= */ false);
    if (!res.ok()) {
      LOG(ERROR) << res.error();
    }
  });

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  // Clear the database before calling PopulateFromMounts
  db.Reset();

  // Populate from mount
  db.PopulateFromMounts({GetDataDir(), GetDecompressionDir()});

  // Count number of package and collect package names
  int package_count = 0;
  std::vector<std::string> mounted_paths;
  db.ForallMountedApexes([&](const std::string& package,
                             const MountedApexData& data, bool latest) {
    package_count++;
    mounted_paths.push_back(data.full_path);
  });
  ASSERT_EQ(package_count, 2);
  ASSERT_THAT(mounted_paths,
              UnorderedElementsAre(apex_path, decompressed_apex));
}

TEST_F(ApexdMountTest, UnmountAll) {
  AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  // Mount an apex from decomrpession_dir
  PrepareCompressedApex("com.android.apex.compressed.v1.capex");
  std::string decompressed_apex =
      StringPrintf("%s/com.android.apex.compressed@1.decompressed.apex",
                   GetDecompressionDir().c_str());

  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  ASSERT_THAT(ActivatePackage(apex_path_2), Ok());
  ASSERT_THAT(ActivatePackage(apex_path_3), Ok());
  ASSERT_THAT(ActivatePackage(decompressed_apex), Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.compressed",
                                   "/apex/com.android.apex.compressed@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  auto& db = GetApexDatabaseForTesting();
  // UnmountAll expects apex database to empty, hence this reset.
  db.Reset();

  ASSERT_EQ(0, UnmountAll(/*also_include_staged_apexes=*/false));

  auto new_apex_mounts = GetApexMounts();
  ASSERT_EQ(new_apex_mounts.size(), 0u);
}

TEST_F(ApexdMountTest, UnmountAllSharedLibsApex) {
  ASSERT_EQ(mkdir("/apex/sharedlibs", 0755), 0);
  ASSERT_EQ(mkdir("/apex/sharedlibs/lib", 0755), 0);
  ASSERT_EQ(mkdir("/apex/sharedlibs/lib64", 0755), 0);
  auto deleter = make_scope_guard([]() {
    std::error_code ec;
    fs::remove_all("/apex/sharedlibs", ec);
    if (ec) {
      LOG(ERROR) << "Failed to delete /apex/sharedlibs : " << ec;
    }
  });

  std::string apex_path_1 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_2 =
      AddDataApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex");

  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  ASSERT_THAT(ActivatePackage(apex_path_1), Ok());
  ASSERT_THAT(ActivatePackage(apex_path_2), Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test.sharedlibs@1",
                                   "/apex/com.android.apex.test.sharedlibs@2"));

  auto& db = GetApexDatabaseForTesting();
  // UnmountAll expects apex database to empty, hence this reset.
  db.Reset();

  ASSERT_EQ(0, UnmountAll(/*also_include_staged_apexes=*/false));

  auto new_apex_mounts = GetApexMounts();
  ASSERT_EQ(new_apex_mounts.size(), 0u);
}

TEST_F(ApexdMountTest, UnmountAllDeferred) {
  AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  ASSERT_THAT(ActivatePackage(apex_path_2), Ok());
  ASSERT_THAT(ActivatePackage(apex_path_3), Ok());

  ASSERT_THAT(GetApexMounts(),
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  const std::string kDeviceName = "com.android.apex.test_package@2";
  Result<std::vector<std::string>> loop_devices =
      ListChildLoopDevices(kDeviceName);
  ASSERT_THAT(loop_devices, HasValue(Not(IsEmpty())));

  // Open a file. This should make unmounting in `UnmountAll` deferred.
  unique_fd fd(
      open("/apex/com.android.apex.test_package/etc/sample_prebuilt_file",
           O_RDONLY));
  ASSERT_GE(fd, 0) << strerror(errno);

  auto& db = GetApexDatabaseForTesting();
  // UnmountAll expects apex database to empty, hence this reset.
  db.Reset();
  // UnmountAll should succeed despite the open file.
  ASSERT_EQ(UnmountAll(/*also_include_staged_apexes=*/false), 0);

  // The mount should still be there, but it should be detached from the
  // filesystem, so the mount point should be gone.
  EXPECT_THAT(GetApexMounts(), IsEmpty());
  // The DM device and the loop device should still be there.
  auto& dm = DeviceMapper::Instance();
  EXPECT_EQ(dm.GetState(kDeviceName), dm::DmDeviceState::ACTIVE);
  for (const std::string& loop_device : *loop_devices) {
    EXPECT_THAT(GetLoopDeviceStatus(loop_device), Ok());
  }

  // Close the file. Unmounting should be automatically performed after then.
  fd.reset();
  // Wait for the kernel to clean things up.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // The DM device and the loop device should be gone.
  EXPECT_EQ(dm.GetState(kDeviceName), dm::DmDeviceState::INVALID);
  for (const std::string& loop_device : *loop_devices) {
    EXPECT_THAT(GetLoopDeviceStatus(loop_device), HasError(WithCode(ENXIO)));
  }
}

TEST_F(ApexdMountTest, UnmountAllStaged) {
  // Both a pre-installed apex and a staged apex are mounted. UnmountAll should
  // unmount both.
  AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test_v2.apex");
  auto apex_session = CreateStagedSession("apex.apexd_test_v2.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);
  std::string apex_path_3 =
      GetStagedDir(apex_session->GetId()) + "/" + "apex.apexd_test_v2.apex";

  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  ASSERT_THAT(ActivatePackage(apex_path_2), Ok());
  ASSERT_THAT(ActivatePackage(apex_path_3), Ok());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  auto& db = GetApexDatabaseForTesting();
  // UnmountAll expects apex database to empty, hence this reset.
  db.Reset();

  ASSERT_EQ(0, UnmountAll(/*also_include_staged_apexes=*/true));
  apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts, IsEmpty());
}

TEST_F(ApexdMountTest, OnStartInVmModeActivatesPreInstalled) {
  auto path1 = AddPreInstalledApex("apex.apexd_test.apex");
  auto path2 = AddPreInstalledApex("apex.apexd_test_different_app.apex");
  // In VM mode, we don't scan /data/apex
  AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(0, OnStartInVmMode());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(GetProperty(kTestApexdStatusSysprop, ""), "ready");
}

TEST_F(ApexdMountTest, OnStartInVmModeFailsWithCapex) {
  AddPreInstalledApex("com.android.apex.compressed.v2.capex");

  ASSERT_EQ(1, OnStartInVmMode());
}

TEST_F(ApexdMountTest, OnStartInVmModeActivatesBlockDevicesAsWell) {
  // Set system property to enable block apexes
  SetBlockApexEnabled(true);

  auto path1 = AddBlockApex("apex.apexd_test.apex");

  ASSERT_EQ(0, OnStartInVmMode());

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ path1,
      /* preinstalledModulePath= */ path1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true, GetMTime(path1),
      /* provideSharedApexLibs= */ false,
      /* partition= */ GetBlockPartitionString());
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1)));
}

TEST_F(ApexdMountTest, OnStartInVmModeFailsWithDuplicateNames) {
  // Set system property to enable block apexes
  SetBlockApexEnabled(true);

  AddPreInstalledApex("apex.apexd_test.apex");
  AddBlockApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(1, OnStartInVmMode());
}

TEST_F(ApexdMountTest, OnStartInVmSupportsMultipleSharedLibsApexes) {
  MockCheckpointInterface checkpoint_interface;
  InitializeVold(&checkpoint_interface);
  SetBlockApexEnabled(true);

  auto path1 =
      AddBlockApex("com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
                   /*public_key=*/"", /*root_digest=*/"", /*is_factory=*/true);
  auto path2 =
      AddBlockApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex",
                   /*public_key=*/"", /*root_digest=*/"", /*is_factory=*/false);

  ASSERT_EQ(0, OnStartInVmMode());

  // Btw, in case duplicates are sharedlibs apexes, both should be activated
  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test.sharedlibs@1",
                                   "/apex/com.android.apex.test.sharedlibs@2"));
}

TEST_F(ApexdMountTest, OnStartInVmShouldRejectInDuplicateFactoryApexes) {
  MockCheckpointInterface checkpoint_interface;
  InitializeVold(&checkpoint_interface);
  SetBlockApexEnabled(true);

  auto path1 =
      AddBlockApex("com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
                   /*public_key=*/"", /*root_digest=*/"", /*is_factory=*/true);
  auto path2 =
      AddBlockApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex",
                   /*public_key=*/"", /*root_digest=*/"", /*is_factory=*/true);

  ASSERT_EQ(1, OnStartInVmMode());
}

TEST_F(ApexdMountTest, OnStartInVmShouldRejectInDuplicateNonFactoryApexes) {
  MockCheckpointInterface checkpoint_interface;
  InitializeVold(&checkpoint_interface);
  SetBlockApexEnabled(true);

  auto path1 =
      AddBlockApex("com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
                   /*public_key=*/"", /*root_digest=*/"", /*is_factory=*/false);
  auto path2 =
      AddBlockApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex",
                   /*public_key=*/"", /*root_digest=*/"", /*is_factory=*/false);

  ASSERT_EQ(1, OnStartInVmMode());
}

TEST_F(ApexdMountTest, OnStartInVmModeFailsWithWrongPubkey) {
  // Set system property to enable block apexes
  SetBlockApexEnabled(true);

  AddBlockApex("apex.apexd_test.apex", /*public_key=*/"wrong pubkey");

  ASSERT_EQ(1, OnStartInVmMode());
}

TEST_F(ApexdMountTest, GetActivePackagesReturningBlockApexesAsWell) {
  // Set system property to enable block apexes
  SetBlockApexEnabled(true);

  auto path1 = AddBlockApex("apex.apexd_test.apex");

  ASSERT_EQ(0, OnStartInVmMode());

  auto active_apexes = GetActivePackages();
  ASSERT_EQ(1u, active_apexes.size());
  ASSERT_EQ(path1, active_apexes[0].GetPath());
}

TEST_F(ApexdMountTest, OnStartInVmModeFailsWithWrongRootDigest) {
  // Set system property to enable block apexes
  SetBlockApexEnabled(true);

  AddBlockApex("apex.apexd_test.apex", /*public_key=*/"",
               /*root_digest=*/"wrong root digest");

  ASSERT_EQ(1, OnStartInVmMode());
}

class ApexActivationFailureTests : public ApexdMountTest {};

TEST_F(ApexActivationFailureTests, BuildFingerprintDifferent) {
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  apex_session->SetBuildFingerprint("wrong fingerprint");
  ASSERT_RESULT_OK(apex_session->UpdateStateAndCommit(SessionState::STAGED));

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_THAT(apex_session->GetErrorMessage(),
              HasSubstr("APEX build fingerprint has changed"));
}

TEST_F(ApexActivationFailureTests, ApexFileMissingInStagingDirectory) {
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);
  // Delete the apex file in staging directory
  DeleteDirContent(GetStagedDir(123));

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_THAT(apex_session->GetErrorMessage(), HasSubstr("Found: 0"));
}

TEST_F(ApexActivationFailureTests, MultipleApexFileInStagingDirectory) {
  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  CreateStagedSession("com.android.apex.compressed.v1.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_THAT(apex_session->GetErrorMessage(), HasSubstr("Found: 2"));
}

TEST_F(ApexActivationFailureTests, CorruptedSuperblockApexCannotBeStaged) {
  auto apex_session =
      CreateStagedSession("apex.apexd_test_corrupt_superblock_apex.apex", 123);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);
  ASSERT_RESULT_OK(apex_session);

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_THAT(apex_session->GetErrorMessage(),
              HasSubstr("Couldn't find filesystem magic"));
}

TEST_F(ApexActivationFailureTests, CorruptedApexCannotBeStaged) {
  auto apex_session = CreateStagedSession("corrupted_b146895998.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_THAT(apex_session->GetErrorMessage(),
              HasSubstr("Activation failed for packages"));
}

TEST_F(ApexActivationFailureTests, ActivatePackageImplFails) {
  auto shim_path = AddPreInstalledApex("com.android.apex.cts.shim.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_RESULT_OK(
      instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}));

  auto apex_session =
      CreateStagedSession("com.android.apex.cts.shim.v2_wrong_sha.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_THAT(apex_session->GetErrorMessage(),
              HasSubstr("Failed to activate packages"));
  ASSERT_THAT(apex_session->GetErrorMessage(),
              HasSubstr("has unexpected SHA512 hash"));
}

TEST_F(ApexActivationFailureTests,
       StagedSessionFailsWhenNotInFsCheckpointMode) {
  MockCheckpointInterface checkpoint_interface;
  checkpoint_interface.SetSupportsCheckpoint(true);
  // Need to call InitializeVold before calling OnStart
  InitializeVold(&checkpoint_interface);

  auto pre_installed_apex = AddPreInstalledApex("apex.apexd_test.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_RESULT_OK(
      instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}));

  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_EQ(apex_session->GetState(), SessionState::ACTIVATION_FAILED);
  ASSERT_THAT(
      apex_session->GetErrorMessage(),
      HasSubstr("Cannot install apex session if not in fs-checkpoint mode"));
}

TEST_F(ApexActivationFailureTests, StagedSessionRevertsWhenInFsRollbackMode) {
  MockCheckpointInterface checkpoint_interface;
  checkpoint_interface.SetSupportsCheckpoint(true);
  checkpoint_interface.SetNeedsRollback(true);
  // Need to call InitializeVold before calling OnStart
  InitializeVold(&checkpoint_interface);

  auto pre_installed_apex = AddPreInstalledApex("apex.apexd_test.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_RESULT_OK(
      instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}));

  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 123);
  ASSERT_RESULT_OK(apex_session);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  OnStart();

  apex_session = GetSessionManager()->GetSession(123);
  ASSERT_RESULT_OK(apex_session);
  ASSERT_EQ(apex_session->GetState(), SessionState::REVERTED);
}

TEST_F(ApexdMountTest, OnBootstrapCreatesEmptyDmDevices) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");

  DeviceMapper& dm = DeviceMapper::Instance();

  auto cleaner = make_scope_guard([&]() {
    dm.DeleteDeviceIfExists("com.android.apex.test_package", 1s);
    dm.DeleteDeviceIfExists("com.android.apex.compressed", 1s);
  });

  ASSERT_EQ(0, OnBootstrap());

  ASSERT_EQ(dm::DmDeviceState::SUSPENDED,
            dm.GetState("com.android.apex.test_package"));
  ASSERT_EQ(dm::DmDeviceState::SUSPENDED,
            dm.GetState("com.android.apex.compressed"));
}

TEST_F(ApexdMountTest, OnBootstrapLoadBootstrapApexOnly) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("apex.apexd_bootstrap_test.apex");

  DeviceMapper& dm = DeviceMapper::Instance();
  auto cleaner = make_scope_guard([&]() {
    dm.DeleteDeviceIfExists("com.android.apex.test_package", 1s);
    dm.DeleteDeviceIfExists("com.android.apex.bootstrap_test_package", 1s);
  });

  ASSERT_EQ(0, OnBootstrap());

  // Check bootstrap apex was loaded
  auto active_bootstrap_apex =
      GetActivePackage("com.android.apex.bootstrap_test_package");
  ASSERT_THAT(active_bootstrap_apex, Ok());
  // Check that non-bootstrap apex was not loaded
  ASSERT_THAT(GetActivePackage("com.android.apex.test_package"), Not(Ok()));
}

TEST_F(ApexdUnitTest, StagePackagesFailKey) {
  auto status =
      StagePackages({GetTestFile("apex.apexd_test_no_inst_key.apex")});

  ASSERT_THAT(
      status,
      HasError(WithMessage(("No preinstalled apex found for unverified package "
                            "com.android.apex.test_package.no_inst_key"))));
}

TEST_F(ApexdUnitTest, StagePackagesSuccess) {
  AddPreInstalledApex("apex.apexd_test.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto status = StagePackages({GetTestFile("apex.apexd_test.apex")});
  ASSERT_THAT(status, Ok());

  auto staged_path = StringPrintf("%s/com.android.apex.test_package@1.apex",
                                  GetDataDir().c_str());
  ASSERT_EQ(0, access(staged_path.c_str(), F_OK));
}

TEST_F(ApexdUnitTest, StagePackagesClearsPreviouslyActivePackage) {
  AddPreInstalledApex("apex.apexd_test.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto current_apex = AddDataApex("apex.apexd_test.apex");
  ASSERT_EQ(0, access(current_apex.c_str(), F_OK));

  auto status = StagePackages({GetTestFile("apex.apexd_test_v2.apex")});
  ASSERT_THAT(status, Ok());

  auto staged_path = StringPrintf("%s/com.android.apex.test_package@2.apex",
                                  GetDataDir().c_str());
  ASSERT_EQ(0, access(staged_path.c_str(), F_OK));
  ASSERT_EQ(-1, access(current_apex.c_str(), F_OK));
  ASSERT_EQ(ENOENT, errno);
}

TEST_F(ApexdUnitTest, StagePackagesClearsPreviouslyActivePackageDowngrade) {
  AddPreInstalledApex("apex.apexd_test.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto current_apex = AddDataApex("apex.apexd_test_v2.apex");
  ASSERT_EQ(0, access(current_apex.c_str(), F_OK));

  auto status = StagePackages({GetTestFile("apex.apexd_test.apex")});
  ASSERT_THAT(status, Ok());

  auto staged_path = StringPrintf("%s/com.android.apex.test_package@1.apex",
                                  GetDataDir().c_str());
  ASSERT_EQ(0, access(staged_path.c_str(), F_OK));
  ASSERT_EQ(-1, access(current_apex.c_str(), F_OK));
  ASSERT_EQ(ENOENT, errno);
}

TEST_F(ApexdUnitTest, StagePackagesAlreadyStagedPackage) {
  AddPreInstalledApex("apex.apexd_test.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto status = StagePackages({GetTestFile("apex.apexd_test.apex")});
  ASSERT_THAT(status, Ok());

  auto staged_path = StringPrintf("%s/com.android.apex.test_package@1.apex",
                                  GetDataDir().c_str());
  struct stat stat1;
  ASSERT_EQ(0, stat(staged_path.c_str(), &stat1));
  ASSERT_TRUE(S_ISREG(stat1.st_mode));

  {
    auto apex = ApexFile::Open(staged_path);
    ASSERT_THAT(apex, Ok());
    ASSERT_FALSE(apex->GetManifest().nocode());
  }

  auto status2 = StagePackages({GetTestFile("apex.apexd_test_nocode.apex")});
  ASSERT_THAT(status2, Ok());

  struct stat stat2;
  ASSERT_EQ(0, stat(staged_path.c_str(), &stat2));
  ASSERT_TRUE(S_ISREG(stat2.st_mode));

  ASSERT_NE(stat1.st_ino, stat2.st_ino);

  {
    auto apex = ApexFile::Open(staged_path);
    ASSERT_THAT(apex, Ok());
    ASSERT_TRUE(apex->GetManifest().nocode());
  }
}

TEST_F(ApexdUnitTest, StagePackagesMultiplePackages) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("apex.apexd_test_different_app.apex");
  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  auto status =
      StagePackages({GetTestFile("apex.apexd_test_v2.apex"),
                     GetTestFile("apex.apexd_test_different_app.apex")});
  ASSERT_THAT(status, Ok());

  auto staged_path1 = StringPrintf("%s/com.android.apex.test_package@2.apex",
                                   GetDataDir().c_str());
  auto staged_path2 = StringPrintf("%s/com.android.apex.test_package_2@1.apex",
                                   GetDataDir().c_str());
  ASSERT_EQ(0, access(staged_path1.c_str(), F_OK));
  ASSERT_EQ(0, access(staged_path2.c_str(), F_OK));
}

TEST_F(ApexdUnitTest, UnstagePackages) {
  auto file_path1 = AddDataApex("apex.apexd_test.apex");
  auto file_path2 = AddDataApex("apex.apexd_test_different_app.apex");

  ASSERT_THAT(UnstagePackages({file_path1}), Ok());
  ASSERT_EQ(-1, access(file_path1.c_str(), F_OK));
  ASSERT_EQ(errno, ENOENT);
  ASSERT_EQ(0, access(file_path2.c_str(), F_OK));
}

TEST_F(ApexdUnitTest, UnstagePackagesEmptyInput) {
  auto file_path1 = AddDataApex("apex.apexd_test.apex");
  auto file_path2 = AddDataApex("apex.apexd_test_different_app.apex");

  ASSERT_THAT(UnstagePackages({}),
              HasError(WithMessage("Empty set of inputs")));
  ASSERT_EQ(0, access(file_path1.c_str(), F_OK));
  ASSERT_EQ(0, access(file_path2.c_str(), F_OK));
}

TEST_F(ApexdUnitTest, UnstagePackagesFail) {
  auto file_path1 = AddDataApex("apex.apexd_test.apex");
  auto bad_path = GetDataDir() + "/missing.apex";

  ASSERT_THAT(UnstagePackages({file_path1, bad_path}), Not(Ok()));
  ASSERT_EQ(0, access(file_path1.c_str(), F_OK));
}

TEST_F(ApexdUnitTest, UnstagePackagesFailPreInstalledApex) {
  auto file_path1 = AddPreInstalledApex("apex.apexd_test.apex");
  auto file_path2 = AddDataApex("apex.apexd_test_different_app.apex");

  auto& instance = ApexFileRepository::GetInstance();
  ASSERT_THAT(instance.AddPreInstalledApex({{GetPartition(), GetBuiltInDir()}}),
              Ok());

  ASSERT_THAT(UnstagePackages({file_path1, file_path2}),
              HasError(WithMessage("Can't uninstall pre-installed apex " +
                                   file_path1)));
  ASSERT_EQ(0, access(file_path1.c_str(), F_OK));
  ASSERT_EQ(0, access(file_path2.c_str(), F_OK));
}

TEST_F(ApexdUnitTest, RevertStoresCrashingNativeProcess) {
  MockCheckpointInterface checkpoint_interface;
  checkpoint_interface.SetSupportsCheckpoint(true);
  InitializeVold(&checkpoint_interface);

  auto apex_session = CreateStagedSession("apex.apexd_test.apex", 1543);
  ASSERT_THAT(apex_session, Ok());
  ASSERT_THAT(apex_session->UpdateStateAndCommit(SessionState::ACTIVATED),
              Ok());

  ASSERT_THAT(RevertActiveSessions("test_process", ""), Ok());
  apex_session = GetSessionManager()->GetSession(1543);
  ASSERT_THAT(apex_session, Ok());
  ASSERT_EQ(apex_session->GetCrashingNativeProcess(), "test_process");
}

TEST_F(ApexdUnitTest, MountAndDeriveClasspathNoJar) {
  AddPreInstalledApex("apex.apexd_test_classpath.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  // Call MountAndDeriveClassPath
  auto apex_file = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  auto package_name = apex_file->GetManifest().name();
  std::vector<ApexFile> apex_files;
  apex_files.emplace_back(std::move(*apex_file));
  auto class_path = MountAndDeriveClassPath(apex_files);
  ASSERT_THAT(class_path, Ok());
  ASSERT_THAT(class_path->HasClassPathJars(package_name), false);
}

TEST_F(ApexdUnitTest, MountAndDeriveClassPathJarsPresent) {
  AddPreInstalledApex("apex.apexd_test_classpath.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}});

  // Call MountAndDeriveClassPath
  auto apex_file =
      ApexFile::Open(GetTestFile("apex.apexd_test_classpath.apex"));
  auto package_name = apex_file->GetManifest().name();
  std::vector<ApexFile> apex_files;
  apex_files.emplace_back(std::move(*apex_file));
  auto class_path = MountAndDeriveClassPath(apex_files);
  ASSERT_THAT(class_path, Ok());
  ASSERT_THAT(class_path->HasClassPathJars(package_name), true);
}

TEST_F(ApexdUnitTest, ProcessCompressedApexWrongSELinuxContext) {
  auto compressed_apex = ApexFile::Open(
      AddPreInstalledApex("com.android.apex.compressed.v1.capex"));

  std::vector<ApexFileRef> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(return_value.size(), 1u);

  auto decompressed_apex_path = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);
  // Verify that so far it has correct context.
  ASSERT_EQ(kTestActiveApexSelinuxCtx,
            GetSelinuxContext(decompressed_apex_path));

  // Manually mess up the context
  ASSERT_EQ(0, setfilecon(decompressed_apex_path.c_str(),
                          "u:object_r:apex_data_file:s0"));
  ASSERT_EQ("u:object_r:apex_data_file:s0",
            GetSelinuxContext(decompressed_apex_path));

  auto attempt_2 =
      ProcessCompressedApex(compressed_apex_list, /* is_ota_chroot= */ false);
  ASSERT_EQ(attempt_2.size(), 1u);
  // Verify that it again has correct context.
  ASSERT_EQ(kTestActiveApexSelinuxCtx,
            GetSelinuxContext(decompressed_apex_path));
}

TEST_F(ApexdMountTest, OnStartNoApexUpdated) {
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");
  std::string apex_path_4 =
      AddDecompressedApex("com.android.apex.compressed.v1.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 0u);
  // Quick check that all apexes were mounted
  auto apex_mounts = GetApexMounts();
  ASSERT_EQ(apex_mounts.size(), 6u);
}

TEST_F(ApexdMountTest, OnStartDecompressingConsideredApexUpdate) {
  AddPreInstalledApex("com.android.apex.compressed.v1.capex");
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string decompressed_active_apex = StringPrintf(
      "%s/com.android.apex.compressed@1%s", GetDecompressionDir().c_str(),
      kDecompressedApexPackageSuffix);

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 1u);
  auto apex_file = ApexFile::Open(decompressed_active_apex);
  ASSERT_THAT(apex_file, Ok());
  ASSERT_TRUE(IsActiveApexChanged(*apex_file));
}

TEST_F(ApexdMountTest, ActivatesStagedSession) {
  std::string preinstalled_apex = AddPreInstalledApex("apex.apexd_test.apex");
  auto apex_session = CreateStagedSession("apex.apexd_test_v2.apex", 37);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  std::string active_apex =
      GetDataDir() + "/" + "com.android.apex.test_package@2.apex";

  OnStart();

  // Quick check that session was activated
  {
    auto session = GetSessionManager()->GetSession(37);
    ASSERT_THAT(session, Ok());
    ASSERT_EQ(session->GetState(), SessionState::ACTIVATED);
  }

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 1u);
  auto apex_file = ApexFile::Open(active_apex);
  ASSERT_THAT(apex_file, Ok());
  ASSERT_TRUE(IsActiveApexChanged(*apex_file));
}

TEST_F(ApexdMountTest, FailsToActivateStagedSession) {
  std::string preinstalled_apex = AddPreInstalledApex("apex.apexd_test.apex");
  auto apex_session =
      CreateStagedSession("apex.apexd_test_manifest_mismatch.apex", 73);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  // Quick check that session was activated
  {
    auto session = GetSessionManager()->GetSession(73);
    ASSERT_THAT(session, Ok());
    ASSERT_NE(session->GetState(), SessionState::ACTIVATED);
  }

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 1u);

  auto apex_file = ApexFile::Open(preinstalled_apex);
  ASSERT_THAT(apex_file, Ok());
  ASSERT_TRUE(IsActiveApexChanged(*apex_file));
}

TEST_F(ApexdMountTest, FailsToActivateApexFallbacksToSystemOne) {
  std::string preinstalled_apex = AddPreInstalledApex("apex.apexd_test.apex");
  AddDataApex("apex.apexd_test_manifest_mismatch.apex");

  ASSERT_THAT(ApexFileRepository::GetInstance().AddPreInstalledApex(
                  {{GetPartition(), GetBuiltInDir()}}),
              Ok());

  OnStart();

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 1u);

  auto apex_file = ApexFile::Open(preinstalled_apex);
  ASSERT_THAT(apex_file, Ok());
  ASSERT_TRUE(IsActiveApexChanged(*apex_file));
}

TEST_F(ApexdMountTest, SubmitSingleStagedSessionKeepsPreviousSessions) {
  MockCheckpointInterface checkpoint_interface;
  checkpoint_interface.SetSupportsCheckpoint(true);
  InitializeVold(&checkpoint_interface);

  std::string preinstalled_apex = AddPreInstalledApex("apex.apexd_test.apex");

  ASSERT_RESULT_OK(ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{GetPartition(), GetBuiltInDir()}}));

  // First simulate existence of a bunch of sessions.
  auto session1 = GetSessionManager()->CreateSession(37);
  ASSERT_RESULT_OK(session1);
  ASSERT_RESULT_OK(session1->UpdateStateAndCommit(SessionState::STAGED));

  auto session2 = GetSessionManager()->CreateSession(57);
  ASSERT_RESULT_OK(session2);
  ASSERT_RESULT_OK(session2->UpdateStateAndCommit(SessionState::STAGED));

  auto session3 = GetSessionManager()->CreateSession(73);
  ASSERT_RESULT_OK(session3);
  ASSERT_RESULT_OK(session3->UpdateStateAndCommit(SessionState::SUCCESS));

  PrepareStagedSession("apex.apexd_test.apex", 239);
  ASSERT_RESULT_OK(SubmitStagedSession(239, {}, false, false, -1));

  auto sessions = GetSessionManager()->GetSessions();
  std::sort(
      sessions.begin(), sessions.end(),
      [](const auto& s1, const auto& s2) { return s1.GetId() < s2.GetId(); });

  ASSERT_EQ(4u, sessions.size());

  ASSERT_EQ(37, sessions[0].GetId());
  ASSERT_EQ(SessionState::STAGED, sessions[0].GetState());

  ASSERT_EQ(57, sessions[1].GetId());
  ASSERT_EQ(SessionState::STAGED, sessions[1].GetState());

  ASSERT_EQ(73, sessions[2].GetId());
  ASSERT_EQ(SessionState::SUCCESS, sessions[2].GetState());

  ASSERT_EQ(239, sessions[3].GetId());
  ASSERT_EQ(SessionState::VERIFIED, sessions[3].GetState());
}

struct SpyMetrics : Metrics {
  std::vector<std::tuple<InstallType, bool, ApexFileInfo>> requested;
  std::vector<std::tuple<std::string, InstallResult>> ended;

  void SendInstallationRequested(InstallType install_type, bool is_rollback,
                                 const ApexFileInfo& info) override {
    requested.emplace_back(install_type, is_rollback, info);
  }
  void SendInstallationEnded(const std::string& file_hash,
                             InstallResult result) override {
    ended.emplace_back(file_hash, result);
  }
};

TEST_F(ApexdMountTest, SendEventOnSubmitStagedSession) {
  MockCheckpointInterface checkpoint_interface;
  checkpoint_interface.SetSupportsCheckpoint(true);
  InitializeVold(&checkpoint_interface);

  InitMetrics(std::make_unique<SpyMetrics>());

  std::string preinstalled_apex =
      AddPreInstalledApex("com.android.apex.vendor.foo.apex");

  // Test APEX is a "vendor" APEX. Preinstalled partition should be vendor.
  ASSERT_RESULT_OK(ApexFileRepository::GetInstance().AddPreInstalledApex(
      {{ApexPartition::Vendor, GetBuiltInDir()}}));

  OnStart();
  // checkvintf needs apex-info-list.xml to identify vendor APEXes.
  // OnAllPackagesActivated() generates it.
  OnAllPackagesActivated(/*bootstrap*/ false);

  PrepareStagedSession("com.android.apex.vendor.foo.with_vintf.apex", 239);
  ASSERT_RESULT_OK(SubmitStagedSession(239, {}, false, false, -1));

  auto spy = std::unique_ptr<SpyMetrics>(
      static_cast<SpyMetrics*>(InitMetrics(nullptr).release()));
  ASSERT_NE(nullptr, spy.get());

  ASSERT_EQ(1u, spy->requested.size());
  const auto& requested = spy->requested[0];
  ASSERT_EQ(InstallType::Staged, std::get<0>(requested));
  ASSERT_EQ("com.android.apex.vendor.foo"s, std::get<2>(requested).name);
  ASSERT_THAT(std::get<2>(requested).hals, ElementsAre("android.apex.foo@1"s));

  ASSERT_EQ(0u, spy->ended.size());
}

TEST(Loop, CreateWithApexFile) {
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());
  ASSERT_TRUE(apex->GetImageOffset().has_value());
  ASSERT_TRUE(apex->GetImageSize().has_value());

  auto loop = loop::CreateAndConfigureLoopDevice(apex->GetPath(),
                                                 apex->GetImageOffset().value(),
                                                 apex->GetImageSize().value());
  ASSERT_THAT(loop, Ok());
}

TEST(Loop, NoSuchFile) {
  CaptureStderr();
  {
    auto loop = loop::CreateAndConfigureLoopDevice("invalid_path", 0, 0);
    ASSERT_THAT(loop, Not(Ok()));
  }
  ASSERT_EQ(GetCapturedStderr(), "");
}

TEST_F(ApexdMountTest, SubmitStagedSessionSucceedVerifiedBrandNewApex) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  PrepareStagedSession("com.android.apex.brand.new.apex", 239);
  ASSERT_RESULT_OK(SubmitStagedSession(239, {}, false, false, -1));

  auto sessions = GetSessionManager()->GetSessions();
  ASSERT_EQ(1u, sessions.size());
  ASSERT_EQ(239, sessions[0].GetId());
  ASSERT_EQ(SessionState::VERIFIED, sessions[0].GetState());
  file_repository.Reset();
}

TEST_F(ApexdMountTest,
       SubmitStagedSessionSucceedVerifiedBrandNewApexWithActiveVersion) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir, data_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile("com.android.apex.brand.new.apex"), data_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});
  ASSERT_RESULT_OK(file_repository.AddDataApex(data_dir.path));

  PrepareStagedSession("com.android.apex.brand.new.v2.apex", 239);
  ASSERT_RESULT_OK(SubmitStagedSession(239, {}, false, false, -1));

  auto sessions = GetSessionManager()->GetSessions();
  ASSERT_EQ(1u, sessions.size());
  ASSERT_EQ(239, sessions[0].GetId());
  ASSERT_EQ(SessionState::VERIFIED, sessions[0].GetState());
  file_repository.Reset();
}

TEST_F(ApexdMountTest,
       SubmitStagedSessionFailBrandNewApexMismatchActiveVersion) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir, data_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile(
               "apexd_testdata/com.android.apex.brand.new.another.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile("com.android.apex.brand.new.apex"), data_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});
  ASSERT_RESULT_OK(file_repository.AddDataApex(data_dir.path));

  PrepareStagedSession("com.android.apex.brand.new.v2.diffkey.apex", 239);
  auto ret = SubmitStagedSession(239, {}, false, false, -1);

  ASSERT_THAT(
      ret,
      HasError(WithMessage(("Brand-new APEX public key doesn't match existing "
                            "active APEX: com.android.apex.brand.new"))));
  file_repository.Reset();
}

TEST_F(ApexdMountTest, SubmitStagedSessionFailBrandNewApexDisabled) {
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  PrepareStagedSession("com.android.apex.brand.new.apex", 239);
  auto ret = SubmitStagedSession(239, {}, false, false, -1);

  ASSERT_THAT(ret,
              HasError(WithMessage(("No preinstalled apex found for unverified "
                                    "package com.android.apex.brand.new"))));
  file_repository.Reset();
}

TEST_F(ApexdUnitTest, StagePackagesSucceedVerifiedBrandNewApex) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto status = StagePackages({GetTestFile("com.android.apex.brand.new.apex")});

  ASSERT_RESULT_OK(status);
  auto staged_path = StringPrintf("%s/com.android.apex.brand.new@1.apex",
                                  GetDataDir().c_str());
  ASSERT_EQ(0, access(staged_path.c_str(), F_OK));
  file_repository.Reset();
}

TEST_F(ApexdUnitTest, StagePackagesFailUnverifiedBrandNewApex) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile(
               "apexd_testdata/com.android.apex.brand.new.another.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto status = StagePackages({GetTestFile("com.android.apex.brand.new.apex")});

  ASSERT_THAT(status,
              HasError(WithMessage(("No preinstalled apex found for unverified "
                                    "package com.android.apex.brand.new"))));

  file_repository.Reset();
}

TEST_F(ApexdMountTest, ActivatesStagedSessionSucceedVerifiedBrandNewApex) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto apex_session =
      CreateStagedSession("com.android.apex.brand.new.apex", 37);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  std::string active_apex =
      GetDataDir() + "/" + "com.android.apex.brand.new@1.apex";

  OnStart();

  // Quick check that session was activated
  {
    auto session = GetSessionManager()->GetSession(37);
    ASSERT_THAT(session, Ok());
    ASSERT_EQ(session->GetState(), SessionState::ACTIVATED);
  }

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 1u);
  auto apex_file = ApexFile::Open(active_apex);
  ASSERT_THAT(apex_file, Ok());
  ASSERT_TRUE(IsActiveApexChanged(*apex_file));

  file_repository.Reset();
}

TEST_F(ApexdMountTest, ActivatesStagedSessionFailUnverifiedBrandNewApex) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile(
               "apexd_testdata/com.android.apex.brand.new.another.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto apex_session =
      CreateStagedSession("com.android.apex.brand.new.apex", 37);
  apex_session->UpdateStateAndCommit(SessionState::STAGED);

  std::string active_apex =
      GetDataDir() + "/" + "com.android.apex.brand.new@1.apex";

  OnStart();

  // Quick check that session was activated
  {
    auto session = GetSessionManager()->GetSession(37);
    ASSERT_THAT(session, Ok());
    ASSERT_EQ(session->GetState(), SessionState::ACTIVATION_FAILED);
  }

  auto updated_apexes = GetChangedActiveApexesForTesting();
  ASSERT_EQ(updated_apexes.size(), 0u);

  file_repository.Reset();
}

TEST_F(ApexdMountTest, NonStagedUpdateFailVerifiedBrandNewApex) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir, data_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});
  auto file_path = AddDataApex("com.android.apex.brand.new.apex");
  ASSERT_THAT(ActivatePackage(file_path), Ok());

  auto ret = InstallPackage(GetTestFile("com.android.apex.brand.new.apex"),
                            /* force= */ false);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(HasSubstr("No preinstalled apex found for package "
                                     "com.android.apex.brand.new"))));

  file_repository.Reset();
}

class SubmitStagedSessionTest : public ApexdMountTest {
 protected:
  void SetUp() override {
    ApexdMountTest::SetUp();

    MockCheckpointInterface checkpoint_interface;
    checkpoint_interface.SetSupportsCheckpoint(true);
    InitializeVold(&checkpoint_interface);

    // Has two preinstalled APEXes (for testing multi-APEX session)
    AddPreInstalledApex("apex.apexd_test.apex");
    AddPreInstalledApex("apex.apexd_test_different_app.apex");
    ApexFileRepository::GetInstance().AddPreInstalledApex(
        {{GetPartition(), GetBuiltInDir()}});

    OnStart();
  }

  void TearDown() override {
    // Should not leak temporary verity devices regardless of success.
    // Why EXPECT? Needs to call TearDown() for unmounting even when something
    // goes wrong with the test.
    std::vector<DeviceMapper::DmBlockDevice> devices;
    EXPECT_TRUE(DeviceMapper::Instance().GetAvailableDevices(&devices));
    for (const auto& device : devices) {
      EXPECT_THAT(device.name(), Not(EndsWith(".tmp")));
    }

    ApexdMountTest::TearDown();
  }
};

TEST_F(SubmitStagedSessionTest, SimpleSuccess) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);

  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());
}

TEST_F(SubmitStagedSessionTest, SuccessStoresBuildFingerprint) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);

  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());

  auto session = GetSessionManager()->GetSession(session_id);
  ASSERT_NE(session->GetBuildFingerprint(), ""s);
}

TEST_F(SubmitStagedSessionTest,
       RejectIfSamePackageIsAlreadyStaged_SameVersion) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());
  ASSERT_THAT(MarkStagedSessionReady(session_id), Ok());

  auto session_id2 = 43;
  PrepareStagedSession("apex.apexd_test.apex", session_id2);
  ASSERT_THAT(SubmitStagedSession(session_id2, {}, false, false, -1),
              HasError(WithMessage(HasSubstr("already staged"))));
}

TEST_F(SubmitStagedSessionTest,
       RejectIfSamePackageIsAlreadyStaged_DifferentVersion) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());
  ASSERT_THAT(MarkStagedSessionReady(session_id), Ok());

  auto session_id2 = 43;
  PrepareStagedSession("apex.apexd_test_v2.apex", session_id2);
  ASSERT_THAT(SubmitStagedSession(session_id2, {}, false, false, -1),
              HasError(WithMessage(HasSubstr("already staged"))));
}

TEST_F(SubmitStagedSessionTest, RejectStagingIfAnotherSessionIsBeingStaged) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());

  // MarkStagedSessionReady is not called yet.
  auto session_id2 = 43;
  PrepareStagedSession("apex.apexd_test_different_app.apex", session_id2);
  ASSERT_THAT(SubmitStagedSession(session_id2, {}, false, false, -1),
              HasError(WithMessage(HasSubstr("being staged"))));
}

TEST_F(SubmitStagedSessionTest, RejectInstallPackageForStagedPackage) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());
  ASSERT_THAT(MarkStagedSessionReady(session_id), Ok());

  ASSERT_THAT(
      InstallPackage(GetTestFile("apex.apexd_test.apex"), /* force= */ true),
      HasError(WithMessage(HasSubstr("already staged"))));
}

TEST_F(SubmitStagedSessionTest, RejectInstallIfAnotherSessionIsBeingStaged) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());

  // MarkStagedSessionReady is not called yet.
  ASSERT_THAT(InstallPackage(GetTestFile("apex.apexd_test_different_app.apex"),
                             /* force= */ true),
              HasError(WithMessage(HasSubstr("being staged"))));
}

TEST_F(SubmitStagedSessionTest, AbortedSessionDoesNotBlockNewStagingOrInstall) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());
  ASSERT_THAT(AbortStagedSession(session_id), Ok());

  auto session_id2 = 43;
  PrepareStagedSession("apex.apexd_test.apex", session_id2);
  ASSERT_THAT(SubmitStagedSession(session_id2, {}, false, false, -1), Ok());
  ASSERT_THAT(AbortStagedSession(session_id2), Ok());

  ASSERT_THAT(InstallPackage(GetTestFile("apex.apexd_test.apex"),
                             /* force= */ true),
              Ok());
}

TEST_F(SubmitStagedSessionTest, FailWithManifestMismatch) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test_manifest_mismatch.apex", session_id);

  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1),
              HasError(WithMessage(HasSubstr("does not match manifest"))));
}

TEST_F(SubmitStagedSessionTest, FailedSessionNotPersisted) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test_manifest_mismatch.apex", session_id);

  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Not(Ok()));

  auto session = GetSessionManager()->GetSession(session_id);
  ASSERT_THAT(session, Not(Ok()));
}

TEST_F(SubmitStagedSessionTest, CannotBeRollbackAndHaveRollbackEnabled) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);

  ASSERT_THAT(SubmitStagedSession(session_id, {}, /*has_rollback=*/true,
                                  /*is_rollback*/ true, -1),
              HasError(WithMessage(
                  HasSubstr("both a rollback and enabled for rollback"))));
}

TEST_F(SubmitStagedSessionTest, FailWithCorruptApex) {
  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test_corrupt_apex.apex", session_id);

  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1),
              HasError(WithMessage(HasSubstr("corrupted?"))));
}

TEST_F(SubmitStagedSessionTest, SuccessWithMultiSession) {
  auto parent_session_id = 42;
  auto child_session1_id = 43;
  auto child_session2_id = 44;
  auto file1 = PrepareStagedSession("apex.apexd_test.apex", child_session1_id);
  auto file2 = PrepareStagedSession("apex.apexd_test_different_app.apex",
                                    child_session2_id);

  auto ret = SubmitStagedSession(parent_session_id,
                                 {child_session1_id, child_session2_id}, false,
                                 false, -1);
  ASSERT_THAT(ret, HasValue(ElementsAre(Property(&ApexFile::GetPath, file1),
                                        Property(&ApexFile::GetPath, file2))));

  auto session = GetSessionManager()->GetSession(parent_session_id);
  ASSERT_THAT(session->GetChildSessionIds(),
              ElementsAre(child_session1_id, child_session2_id));
}

// Temporary test cases until the feature is fully enabled/implemented
class MountBeforeDataTest : public ApexdMountTest {
 protected:
  void SetUp() override {
    config_.mount_before_data = true;
    ApexdMountTest::SetUp();

    // preinstalled APEXes
    AddPreInstalledApex("apex.apexd_test.apex");
    AddPreInstalledApex("apex.apexd_test_different_app.apex");
  }
};

TEST_F(MountBeforeDataTest, StagingCreatesBackingImages) {
  ASSERT_EQ(0, OnBootstrap());

  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());

  auto session = GetSessionManager()->GetSession(session_id);
  ASSERT_THAT(session->GetApexImages(),
              Pointwise(Eq(), image_manager_->GetAllImages()));
}

TEST_F(MountBeforeDataTest, AbortSessionRemovesBackingImages) {
  ASSERT_EQ(0, OnBootstrap());

  auto session_id = 42;
  PrepareStagedSession("apex.apexd_test.apex", session_id);
  ASSERT_THAT(SubmitStagedSession(session_id, {}, false, false, -1), Ok());
  ASSERT_THAT(AbortStagedSession(session_id), Ok());

  ASSERT_THAT(image_manager_->GetAllImages(), IsEmpty());
}

TEST_F(MountBeforeDataTest, OnBootstrapActivatesAllApexes) {
  ASSERT_EQ(0, OnBootstrap());

  ASSERT_THAT(GetApexMounts(),
              UnorderedElementsAre("/apex/com.android.apex.test_package_2"s,
                                   "/apex/com.android.apex.test_package_2@1"s,
                                   "/apex/com.android.apex.test_package"s,
                                   "/apex/com.android.apex.test_package@1"s));
}

class LogTestToLogcat : public ::testing::EmptyTestEventListener {
  void OnTestStart(const ::testing::TestInfo& test_info) override {
#ifdef __ANDROID__
    using base::LogId;
    using base::LogSeverity;
    using base::StringPrintf;
    base::LogdLogger l;
    std::string msg =
        StringPrintf("=== %s::%s (%s:%d)", test_info.test_suite_name(),
                     test_info.name(), test_info.file(), test_info.line());
    l(LogId::MAIN, LogSeverity::INFO, "ApexTestCases", __FILE__, __LINE__,
      msg.c_str());
#else
    UNUSED(test_info);
#endif
  }
};

}  // namespace apex
}  // namespace android

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  android::base::InitLogging(argv, &android::base::StderrLogger);
  android::base::SetMinimumLogSeverity(android::base::VERBOSE);
  ::testing::UnitTest::GetInstance()->listeners().Append(
      new android::apex::LogTestToLogcat());
  return RUN_ALL_TESTS();
}
