/*
 * Copyright (C) 2023 The Android Open Source Project
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
 *
 * This file contains the vendor-apex-specific functions of apexd
 */

#include "apexd_vendor_apex.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <vintf/VintfObject.h>

#include "apex_file_repository.h"
#include "apexd_private.h"
#include "apexd_utils.h"

using android::base::Error;
using android::base::Result;
using android::base::StartsWith;

namespace android::apex {

using apexd_private::GetActiveMountPoint;

static Result<bool> HasVintfIn(std::span<const std::string> apex_mounts) {
  for (const auto& mount : apex_mounts) {
    if (OR_RETURN(PathExists(mount + "/etc/vintf"))) return true;
  }
  return false;
}

// Checks Compatibility for incoming APEXes.
//    Adds the data from apex's vintf_fragment(s) and tests compatibility.
Result<std::map<std::string, std::vector<std::string>>> CheckVintf(
    std::span<const ApexFile> apex_files,
    std::span<const std::string> mount_points) {
  std::string error;

  std::vector<std::string> current_mounts;
  for (const auto& apex : apex_files) {
    current_mounts.push_back(GetActiveMountPoint(apex.GetManifest()));
  }

  // Skip the check unless any of the current/incoming APEXes has etc/vintf.
  if (!OR_RETURN(HasVintfIn(current_mounts)) &&
      !OR_RETURN(HasVintfIn(mount_points))) {
    return {};
  }

  // Create PathReplacingFileSystem instance containing caller's path
  // substitutions
  std::map<std::string, std::string> replacements;
  CHECK(apex_files.size() == mount_points.size()) << "size mismatch";
  for (size_t i = 0; i < current_mounts.size(); i++) {
    replacements.emplace(current_mounts[i], mount_points[i]);
  }
  std::unique_ptr<vintf::FileSystem> path_replaced_fs =
      std::make_unique<vintf::details::PathReplacingFileSystem>(
          std::make_unique<vintf::details::FileSystemImpl>(),
          std::move(replacements));

  // Create a new VintfObject that uses our path-replacing FileSystem instance
  auto vintf_object = vintf::VintfObject::Builder()
                          .setFileSystem(std::move(path_replaced_fs))
                          .build();

  // Disable RuntimeInfo components. Allows callers to run check
  // without requiring read permission of restricted resources
  auto flags = vintf::CheckFlags::DEFAULT;
  flags = flags.disableRuntimeInfo();

  // checkCompatibility on vintfObj using the replacement vintf directory
  int ret = vintf_object->checkCompatibility(&error, flags);
  if (ret == vintf::INCOMPATIBLE) {
    return Error() << "CheckVintf failed: not compatible. error=" << error;
  }
  if (ret != vintf::COMPATIBLE) {
    return Error() << "CheckVintf failed: error=" << error;
  }

  // Compat check passed.
  // Collect HAL information from incoming APEXes for metrics.
  std::map<std::string, std::vector<std::string>> apex_hals;
  auto collect_hals = [&](auto manifest) {
    manifest->forEachInstance([&](const auto& instance) {
      if (instance.updatableViaApex().has_value()) {
        apex_hals[instance.updatableViaApex().value()].push_back(
            instance.nameWithVersion());
      }
      return true;  // continue
    });
  };
  collect_hals(vintf_object->getFrameworkHalManifest());
  collect_hals(vintf_object->getDeviceHalManifest());

  return apex_hals;
}

}  // namespace android::apex
