/*
 * Copyright (C) 2019 The Android Open Source Project
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

#pragma once

#include <android-base/result.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "apex_constants.h"
#include "apex_file.h"

namespace android::apex {

using ApexFileRef = std::reference_wrapper<const android::apex::ApexFile>;

struct ApexPath {
  std::string path;
  ApexPartition partition;
};

struct ApexFileAndPartition {
  ApexFile apex_file;
  ApexPartition partition;
};

// This class serves as a ApexFile repository for all apexes on device. It also
// provides information about the ApexFiles it hosts, such as which are
// pre-installed and which are data. Such information can be used, for example,
// to verify validity of an apex before trying to mount it.
//
// It's expected to have a single instance of this class in a process that
// mounts apexes (e.g. apexd, otapreopt_chroot).
class ApexFileRepository final {
 public:
  // c-tors and d-tor are exposed for testing.
  explicit ApexFileRepository(
      const std::string& decompression_dir = kApexDecompressedDir)
      : decompression_dir_(decompression_dir) {}
  explicit ApexFileRepository(
      bool enforce_multi_install_partition,
      const std::vector<std::string>& multi_install_select_prop_prefixes)
      : multi_install_select_prop_prefixes_(multi_install_select_prop_prefixes),
        enforce_multi_install_partition_(enforce_multi_install_partition) {}

  // Returns a singletone instance of this class.
  static ApexFileRepository& GetInstance();

  // Populate instance by collecting pre-installed apex files from the given
  // |partition_to_prebuilt_dirs|.
  // Note: this call is **not thread safe** and is expected to be performed in a
  // single thread during initialization of apexd. After initialization is
  // finished, all queries to the instance are thread safe.
  android::base::Result<void> AddPreInstalledApex(
      const std::unordered_map<ApexPartition, std::string>&
          partition_to_prebuilt_dirs);

  // Populate instance by collecting pre-installed apex files from the given
  // |partition_to_prebuilt_dirs|.
  // The difference between this function and |AddPreInstalledApex| is that this
  // function opens pre-installed apex files in parallel. Note: this call is
  // **not thread safe** and is expected to be performed in a single thread
  // during initialization of apexd. After initialization is finished, all
  // queries to the instance are thread safe.
  android::base::Result<void> AddPreInstalledApexParallel(
      const std::unordered_map<ApexPartition, std::string>&
          partition_to_prebuilt_dirs);

  // Populate instance by collecting host-provided apex files via
  // |metadata_partition|. Host can provide its apexes to a VM instance via the
  // virtual disk image which has partitions: (see
  // /packages/modules/Virtualization/microdroid for the details)
  //  - metadata partition(/dev/block/vd*1) should be accessed by
  //  setting the system property apexd.payload_metadata.prop. On microdroid,
  //  this is /dev/block/by-name/payload-metadata.
  //  - each subsequence partition(/dev/block/vd*{2,3,..}) represents an APEX
  //  archive.
  // It will fail if there is more than one apex with the same name in
  // pre-installed and block apexes. Note: this call is **not thread safe** and
  // is expected to be performed in a single thread during initialization of
  // apexd. After initialization is finished, all queries to the instance are
  // thread safe.
  // This will return the number of block apexes that were added.
  android::base::Result<int> AddBlockApex(
      const std::string& metadata_partition);

  // Populate instance by collecting data apex files from the given |data_dir|.
  // Note: this call is **not thread safe** and is expected to be performed in a
  // single thread during initialization of apexd. After initialization is
  // finished, all queries to the instance are thread safe.
  android::base::Result<void> AddDataApex(const std::string& data_dir);

  // Populates instance by collecting pre-installed credential files (.avbpubkey
  // for now) and blocklist files from the given directories. They are needed
  // specifically for brand-new APEX.
  // Note: this call is **not thread safe** and
  // is expected to be performed in a single thread during initialization of
  // apexd. After initialization is finished, all queries to the instance are
  // thread safe.
  android::base::Result<void> AddBrandNewApexCredentialAndBlocklist(
      const std::unordered_map<ApexPartition, std::string>&
          partition_to_dir_map);

  // Returns the mapping partition of a specific apex.
  // For pre-installed APEX, it is the partition where the pre-installed package
  // resides. For brand-new APEX, it is the partition where the
  // credentials to verify the package reside.
  android::base::Result<ApexPartition> GetPartition(const ApexFile& apex) const;

  // Returns trusted public key for an apex with the given |name|.
  android::base::Result<const std::string> GetPublicKey(
      const std::string& name) const;

  // Returns path to the pre-installed version of an apex with the given |name|.
  // For brand-new APEX, returns Error.
  // For block APEX which is not set as factory, returns Error.
  android::base::Result<const std::string> GetPreinstalledPath(
      const std::string& name) const;

  // Returns root digest of an apex with the given |path| for block apexes.
  std::optional<std::string> GetBlockApexRootDigest(
      const std::string& path) const;

  // Returns timestamp to be used for the block apex of the given |path|.
  std::optional<int64_t> GetBlockApexLastUpdateSeconds(
      const std::string& path) const;

  // Checks whether there is a pre-installed version of an apex with the given
  // |name|.
  bool HasPreInstalledVersion(const std::string& name) const;

  // Checks whether there is a data version of an apex with the given |name|.
  bool HasDataVersion(const std::string& name) const;

  // Checks if given |apex| is pre-installed.
  bool IsPreInstalledApex(const ApexFile& apex) const;

  // Checks if given |apex| is decompressed from a pre-installed APEX
  bool IsDecompressedApex(const ApexFile& apex) const;

  // Checks if given |apex| is loaded from block device.
  bool IsBlockApex(const ApexFile& apex) const;

  // Returns reference to all pre-installed APEX on device
  std::vector<ApexFileRef> GetPreInstalledApexFiles() const;

  // Returns reference to all data APEX on device
  std::vector<ApexFileRef> GetDataApexFiles() const;

  // Returns the partition of the pre-installed public key which exactly matches
  // the |public_key|.
  std::optional<ApexPartition> GetBrandNewApexPublicKeyPartition(
      const std::string& public_key) const;

  // Returns the blocked version number of a specific brand-new APEX in a
  // specific partition. The brand-new APEX is only allowed when its version is
  // larger than the blocked version.
  // Returns |std::nullopt| if the |apex_name| is not configured in blocklist.
  std::optional<int64_t> GetBrandNewApexBlockedVersion(
      ApexPartition partition, const std::string& apex_name) const;

  // Group all ApexFiles on device by their package name
  std::unordered_map<std::string, std::vector<ApexFileRef>> AllApexFilesByName()
      const;

  // Returns a pre-installed version of apex with the given name. Caller is
  // expected to check if there is a pre-installed apex with the given name
  // using |HasPreinstalledVersion| function.
  ApexFileRef GetPreInstalledApex(const std::string& name) const;
  // Returns a data version of apex with the given name. Caller is
  // expected to check if there is a data apex with the given name
  // using |HasDataVersion| function.
  ApexFileRef GetDataApex(const std::string& name) const;

  // Returns if installation of brand-new APEX is enabled.
  static inline bool IsBrandNewApexEnabled() { return enable_brand_new_apex_; };

  // Enables installation of brand-new APEX.
  static inline void EnableBrandNewApex() { enable_brand_new_apex_ = true; };

  // Clears ApexFileRepostiry.
  // Only use in tests.
  void Reset(const std::string& decompression_dir = kApexDecompressedDir) {
    pre_installed_store_.clear();
    data_store_.clear();
    partition_store_.clear();
    brand_new_apex_blocked_version_.clear();
    brand_new_apex_pubkeys_.clear();
    block_apex_overrides_.clear();
    decompression_dir_ = decompression_dir;
    block_disk_path_.reset();
    enable_brand_new_apex_ = false;
  }

 private:
  // Non-copyable && non-moveable.
  ApexFileRepository(const ApexFileRepository&) = delete;
  ApexFileRepository& operator=(const ApexFileRepository&) = delete;
  ApexFileRepository& operator=(ApexFileRepository&&) = delete;
  ApexFileRepository(ApexFileRepository&&) = delete;

  // Stores the given single apex data into |pre_installed_store_| and
  // |partition_store_|.
  void StorePreInstalledApex(ApexFile&& apex_file, ApexPartition partition);

  // Scans and returns apexes in the given directories.
  android::base::Result<std::vector<ApexPath>> CollectPreInstalledApex(
      const std::unordered_map<ApexPartition, std::string>&
          partition_to_prebuilt_dirs);

  // Opens and returns the apexes in the given paths.
  android::base::Result<std::vector<ApexFileAndPartition>> OpenApexFiles(
      const std::vector<ApexPath>& apex_paths);

  std::unordered_map<std::string, ApexFile> pre_installed_store_, data_store_;

  // Map from APEX name to their partition. For pre-installed APEX, this is the
  // partition where it is pre-installed. For brand-new APEX, this is the
  // partition where its credential is pre-installed.
  std::unordered_map<std::string, ApexPartition> partition_store_;

  // Blocked versions for brand-new APEX mapped by their holding partition.
  std::unordered_map<ApexPartition, std::unordered_map<std::string, int64_t>>
      brand_new_apex_blocked_version_;

  // Map from trusted public keys for brand-new APEX to their holding partition.
  std::unordered_map<std::string, ApexPartition> brand_new_apex_pubkeys_;

  // Multi-installed APEX name -> all encountered public keys for this APEX.
  std::unordered_map<std::string, std::unordered_set<std::string>>
      multi_install_public_keys_;

  // Prefixes used when looking for multi-installed APEX sysprops.
  // Order matters: the first non-empty prop value is returned.
  std::vector<std::string> multi_install_select_prop_prefixes_ =
      kMultiApexSelectPrefix;

  // Allows multi-install APEXes outside of expected partitions.
  // Only set false in tests.
  bool enforce_multi_install_partition_ = true;

  // Disallows installation of brand-new APEX by default.
  inline static bool enable_brand_new_apex_ = false;

  // Decompression directory which will be used to determine if apex is
  // decompressed or not
  std::string decompression_dir_;

  // Disk path where block apexes are read from. AddBlockApex() sets this.
  std::optional<std::string> block_disk_path_;

  // Information from the metadata for block apexes, overriding the file data.
  struct BlockApexOverride {
    // Root digest for the APEX. When specified in block apex config, it
    // should be used/checked when activating the apex to avoid
    // TOCTOU(time-of-check to time-of-use).
    std::optional<std::string> block_apex_root_digest;
    // The last update time of the APEX.
    std::optional<int64_t> last_update_seconds;
  };

  // Use "path" as key instead of APEX name because there can be multiple
  // versions of sharedlibs APEXes.
  std::unordered_map<std::string, BlockApexOverride> block_apex_overrides_;
};

}  // namespace android::apex
