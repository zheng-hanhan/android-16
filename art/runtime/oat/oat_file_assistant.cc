/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "oat_file_assistant.h"

#include <sys/stat.h>

#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/properties.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "arch/instruction_set.h"
#include "base/array_ref.h"
#include "base/compiler_filter.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/logging.h"  // For VLOG.
#include "base/macros.h"
#include "base/os.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "dex/art_dex_file_loader.h"
#include "dex/dex_file_loader.h"
#include "exec_utils.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "image.h"
#include "oat.h"
#include "oat/oat_file.h"
#include "oat_file_assistant_context.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "vdex_file.h"
#include "zlib.h"

namespace art HIDDEN {

using ::android::base::ConsumePrefix;
using ::android::base::StringPrintf;

static constexpr const char* kAnonymousDexPrefix = "Anonymous-DexFile@";

std::ostream& operator<<(std::ostream& stream, const OatFileAssistant::OatStatus status) {
  switch (status) {
    case OatFileAssistant::kOatCannotOpen:
      stream << "kOatCannotOpen";
      break;
    case OatFileAssistant::kOatDexOutOfDate:
      stream << "kOatDexOutOfDate";
      break;
    case OatFileAssistant::kOatBootImageOutOfDate:
      stream << "kOatBootImageOutOfDate";
      break;
    case OatFileAssistant::kOatUpToDate:
      stream << "kOatUpToDate";
      break;
    case OatFileAssistant::kOatContextOutOfDate:
      stream << "kOatContextOutOfDate";
      break;
  }

  return stream;
}

OatFileAssistant::OatFileAssistant(const char* dex_location,
                                   const InstructionSet isa,
                                   ClassLoaderContext* context,
                                   bool load_executable,
                                   bool only_load_trusted_executable,
                                   OatFileAssistantContext* ofa_context)
    : OatFileAssistant(dex_location,
                       isa,
                       context,
                       load_executable,
                       only_load_trusted_executable,
                       ofa_context,
                       /*vdex_fd=*/-1,
                       /*oat_fd=*/-1,
                       /*zip_fd=*/-1) {}

OatFileAssistant::OatFileAssistant(const char* dex_location,
                                   const InstructionSet isa,
                                   ClassLoaderContext* context,
                                   bool load_executable,
                                   bool only_load_trusted_executable,
                                   OatFileAssistantContext* ofa_context,
                                   int vdex_fd,
                                   int oat_fd,
                                   int zip_fd)
    : context_(context),
      isa_(isa),
      load_executable_(load_executable),
      only_load_trusted_executable_(only_load_trusted_executable),
      zip_fd_(zip_fd) {
  CHECK(dex_location != nullptr) << "OatFileAssistant: null dex location";
  CHECK_IMPLIES(load_executable, context != nullptr) << "Loading executable without a context";

  if (zip_fd < 0) {
    CHECK_LE(oat_fd, 0) << "zip_fd must be provided with valid oat_fd. zip_fd=" << zip_fd
                        << " oat_fd=" << oat_fd;
    CHECK_LE(vdex_fd, 0) << "zip_fd must be provided with valid vdex_fd. zip_fd=" << zip_fd
                         << " vdex_fd=" << vdex_fd;
    CHECK(!UseFdToReadFiles());
  } else {
    CHECK(UseFdToReadFiles());
  }

  dex_location_.assign(dex_location);

  Runtime* runtime = Runtime::Current();

  if (load_executable_ && runtime == nullptr) {
    LOG(WARNING) << "OatFileAssistant: Load executable specified, "
                 << "but no active runtime is found. Will not attempt to load executable.";
    load_executable_ = false;
  }

  if (load_executable_ && isa != kRuntimeQuickCodeISA) {
    LOG(WARNING) << "OatFileAssistant: Load executable specified, "
                 << "but isa is not kRuntimeQuickCodeISA. Will not attempt to load executable.";
    load_executable_ = false;
  }

  if (ofa_context == nullptr) {
    CHECK(runtime != nullptr) << "runtime_options is not provided, and no active runtime is found.";
    ofa_context_ = std::make_unique<OatFileAssistantContext>(runtime);
  } else {
    ofa_context_ = ofa_context;
  }

  if (runtime == nullptr) {
    // We need `MemMap` for mapping files. We don't have to initialize it when there is a runtime
    // because the runtime initializes it.
    MemMap::Init();
  }

  // Get the odex filename.
  std::string error_msg;
  std::string odex_file_name;
  if (!DexLocationToOdexFilename(dex_location_, isa_, &odex_file_name, &error_msg)) {
    LOG(WARNING) << "Failed to determine odex file name: " << error_msg;
  }

  // Get the oat filename.
  std::string oat_file_name;
  if (!UseFdToReadFiles()) {
    if (!DexLocationToOatFilename(dex_location_,
                                  isa_,
                                  GetRuntimeOptions().deny_art_apex_data_files,
                                  &oat_file_name,
                                  &error_msg)) {
      if (kIsTargetAndroid) {
        // No need to warn on host. We are probably in oatdump, where we only need OatFileAssistant
        // to validate BCP checksums.
        LOG(WARNING) << "Failed to determine oat file name for dex location " << dex_location_
                     << ": " << error_msg;
      }
    }
  }

  if (!oat_file_name.empty() && !UseFdToReadFiles()) {
    // The oat location. This is for apps on readonly filesystems (typically, system apps and
    // incremental apps). This must be prioritized over the odex location, because the odex location
    // probably has the dexpreopt artifacts for such apps.
    info_list_.push_back(std::make_unique<OatFileInfoBackedByOat>(this,
                                                                  oat_file_name,
                                                                  /*is_oat_location=*/true,
                                                                  /*use_fd=*/false));
    info_list_.push_back(
        std::make_unique<OatFileInfoBackedBySdm>(this,
                                                 GetSdmFilename(dex_location_, isa),
                                                 /*is_oat_location=*/true,
                                                 GetDmFilename(dex_location_),
                                                 GetSdcFilename(oat_file_name)));
  }

  if (!odex_file_name.empty()) {
    // The odex location, which is the most common.
    info_list_.push_back(std::make_unique<OatFileInfoBackedByOat>(this,
                                                                  odex_file_name,
                                                                  /*is_oat_location=*/false,
                                                                  UseFdToReadFiles(),
                                                                  zip_fd,
                                                                  vdex_fd,
                                                                  oat_fd));
    info_list_.push_back(
        std::make_unique<OatFileInfoBackedBySdm>(this,
                                                 GetSdmFilename(dex_location_, isa),
                                                 /*is_oat_location=*/false,
                                                 GetDmFilename(dex_location_),
                                                 GetSdcFilename(odex_file_name)));
  }

  // When there is no odex/oat available (e.g., they are both out of date), we look for a useable
  // vdex file.

  if (!oat_file_name.empty() && !UseFdToReadFiles()) {
    // The vdex-only file next to 'oat_`.
    info_list_.push_back(std::make_unique<OatFileInfoBackedByVdex>(this,
                                                                   GetVdexFilename(oat_file_name),
                                                                   /*is_oat_location=*/true,
                                                                   /*use_fd=*/false));
  }

  if (!odex_file_name.empty()) {
    // The vdex-only file next to `odex_`.
    // We dup FDs as the odex_ will claim ownership.
    info_list_.push_back(std::make_unique<OatFileInfoBackedByVdex>(this,
                                                                   GetVdexFilename(odex_file_name),
                                                                   /*is_oat_location=*/false,
                                                                   UseFdToReadFiles(),
                                                                   DupCloexec(zip_fd),
                                                                   DupCloexec(vdex_fd)));
  }

  if (!UseFdToReadFiles()) {
    // A .dm file may be available, look for it.
    info_list_.push_back(
        std::make_unique<OatFileInfoBackedByDm>(this, GetDmFilename(dex_location_)));
  }
}

// Must be defined outside of the class, to prevent inlining, which causes callers to access hidden
// symbols used by the destructor. `NOINLINE` doesn't work.
OatFileAssistant::~OatFileAssistant() = default;

std::unique_ptr<OatFileAssistant> OatFileAssistant::Create(
    const std::string& filename,
    const std::string& isa_str,
    const std::optional<std::string>& context_str,
    bool load_executable,
    bool only_load_trusted_executable,
    OatFileAssistantContext* ofa_context,
    /*out*/ std::unique_ptr<ClassLoaderContext>* context,
    /*out*/ std::string* error_msg) {
  InstructionSet isa = GetInstructionSetFromString(isa_str.c_str());
  if (isa == InstructionSet::kNone) {
    *error_msg = StringPrintf("Instruction set '%s' is invalid", isa_str.c_str());
    return nullptr;
  }

  std::unique_ptr<ClassLoaderContext> tmp_context = nullptr;
  if (context_str.has_value()) {
    tmp_context = ClassLoaderContext::Create(context_str.value());
    if (tmp_context == nullptr) {
      *error_msg = StringPrintf("Class loader context '%s' is invalid", context_str->c_str());
      return nullptr;
    }

    if (!tmp_context->OpenDexFiles(android::base::Dirname(filename),
                                   /*context_fds=*/{},
                                   /*only_read_checksums=*/true)) {
      *error_msg =
          StringPrintf("Failed to load class loader context files for '%s' with context '%s'",
                       filename.c_str(),
                       context_str->c_str());
      return nullptr;
    }
  }

  auto assistant = std::make_unique<OatFileAssistant>(filename.c_str(),
                                                      isa,
                                                      tmp_context.get(),
                                                      load_executable,
                                                      only_load_trusted_executable,
                                                      ofa_context);

  *context = std::move(tmp_context);
  return assistant;
}

bool OatFileAssistant::UseFdToReadFiles() { return zip_fd_ >= 0; }

bool OatFileAssistant::IsInBootClassPath() {
  // Note: We check the current boot class path, regardless of the ISA
  // specified by the user. This is okay, because the boot class path should
  // be the same for all ISAs.
  // TODO: Can we verify the boot class path is the same for all ISAs?
  for (const std::string& boot_class_path_location :
       GetRuntimeOptions().boot_class_path_locations) {
    if (boot_class_path_location == dex_location_) {
      VLOG(oat) << "Dex location " << dex_location_ << " is in boot class path";
      return true;
    }
  }
  return false;
}

OatFileAssistant::DexOptTrigger OatFileAssistant::GetDexOptTrigger(
    CompilerFilter::Filter target_compiler_filter, bool profile_changed, bool downgrade) {
  if (downgrade) {
    // The caller's intention is to downgrade the compiler filter. We should only re-compile if the
    // target compiler filter is worse than the current one.
    return DexOptTrigger{.targetFilterIsWorse = true};
  }

  // This is the usual case. The caller's intention is to see if a better oat file can be generated.
  DexOptTrigger dexopt_trigger{
      .targetFilterIsBetter = true, .primaryBootImageBecomesUsable = true, .needExtraction = true};
  if (profile_changed && CompilerFilter::DependsOnProfile(target_compiler_filter)) {
    // Since the profile has been changed, we should re-compile even if the compilation does not
    // make the compiler filter better.
    dexopt_trigger.targetFilterIsSame = true;
  }
  return dexopt_trigger;
}

int OatFileAssistant::GetDexOptNeeded(CompilerFilter::Filter target_compiler_filter,
                                      bool profile_changed,
                                      bool downgrade) {
  OatFileInfo& info = GetBestInfo();
  DexOptNeeded dexopt_needed = info.GetDexOptNeeded(
      target_compiler_filter, GetDexOptTrigger(target_compiler_filter, profile_changed, downgrade));
  if (dexopt_needed != kNoDexOptNeeded &&
      (info.GetType() == OatFileType::kDm || info.GetType() == OatFileType::kSdm)) {
    // The usable vdex file is in the DM file. This information cannot be encoded in the integer.
    // Return kDex2OatFromScratch so that neither the vdex in the "oat" location nor the vdex in the
    // "odex" location will be picked by installd.
    return kDex2OatFromScratch;
  }
  if (info.IsOatLocation() || dexopt_needed == kDex2OatFromScratch) {
    return dexopt_needed;
  }
  return -dexopt_needed;
}

bool OatFileAssistant::GetDexOptNeeded(CompilerFilter::Filter target_compiler_filter,
                                       DexOptTrigger dexopt_trigger,
                                       /*out*/ DexOptStatus* dexopt_status) {
  OatFileInfo& info = GetBestInfo();
  DexOptNeeded dexopt_needed = info.GetDexOptNeeded(target_compiler_filter, dexopt_trigger);
  dexopt_status->location_ = GetLocation(info);
  return dexopt_needed != kNoDexOptNeeded;
}

bool OatFileAssistant::IsUpToDate() { return GetBestInfo().Status() == kOatUpToDate; }

std::unique_ptr<OatFile> OatFileAssistant::GetBestOatFile() {
  return GetBestInfo().ReleaseFileForUse();
}

std::vector<std::unique_ptr<const DexFile>> OatFileAssistant::LoadDexFiles(
    const OatFile& oat_file, const char* dex_location) {
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (LoadDexFiles(oat_file, dex_location, &dex_files)) {
    return dex_files;
  } else {
    return std::vector<std::unique_ptr<const DexFile>>();
  }
}

bool OatFileAssistant::LoadDexFiles(const OatFile& oat_file,
                                    const std::string& dex_location,
                                    std::vector<std::unique_ptr<const DexFile>>* out_dex_files) {
  // Load the main dex file.
  std::string error_msg;
  const OatDexFile* oat_dex_file = oat_file.GetOatDexFile(dex_location.c_str(), &error_msg);
  if (oat_dex_file == nullptr) {
    LOG(WARNING) << error_msg;
    return false;
  }

  std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
  if (dex_file.get() == nullptr) {
    LOG(WARNING) << "Failed to open dex file from oat dex file: " << error_msg;
    return false;
  }
  out_dex_files->push_back(std::move(dex_file));

  // Load the rest of the multidex entries
  for (size_t i = 1;; i++) {
    std::string multidex_dex_location = DexFileLoader::GetMultiDexLocation(i, dex_location.c_str());
    oat_dex_file = oat_file.GetOatDexFile(multidex_dex_location.c_str());
    if (oat_dex_file == nullptr) {
      // There are no more multidex entries to load.
      break;
    }

    dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file.get() == nullptr) {
      LOG(WARNING) << "Failed to open dex file from oat dex file: " << error_msg;
      return false;
    }
    out_dex_files->push_back(std::move(dex_file));
  }
  return true;
}

std::optional<bool> OatFileAssistant::HasDexFiles(std::string* error_msg) {
  ScopedTrace trace("HasDexFiles");
  std::optional<std::uint32_t> checksum;
  if (!GetRequiredDexChecksum(&checksum, error_msg)) {
    return std::nullopt;
  }
  return checksum.has_value();
}

OatFileAssistant::OatStatus OatFileAssistant::OdexFileStatus() {
  for (const std::unique_ptr<OatFileInfo>& info : info_list_) {
    if (info->GetType() == OatFileType::kOat && !info->IsOatLocation()) {
      return info->Status();
    }
  }
  return kOatCannotOpen;
}

OatFileAssistant::OatStatus OatFileAssistant::OatFileStatus() {
  for (const std::unique_ptr<OatFileInfo>& info : info_list_) {
    if (info->GetType() == OatFileType::kOat && info->IsOatLocation()) {
      return info->Status();
    }
  }
  return kOatCannotOpen;
}

bool OatFileAssistant::DexChecksumUpToDate(const OatFile& file, std::string* error_msg) {
  if (!file.ContainsDexCode()) {
    // We've already checked during oat file creation that the dex files loaded
    // from external files have the same checksums as the ones in the vdex file.
    return true;
  }
  ScopedTrace trace("DexChecksumUpToDate");
  std::optional<std::uint32_t> dex_checksum;
  if (!GetRequiredDexChecksum(&dex_checksum, error_msg)) {
    return false;
  }
  if (!dex_checksum.has_value()) {
    LOG(WARNING) << "Required dex checksums not found. Assuming dex checksums are up to date.";
    return true;
  }

  std::vector<const OatDexFile*> oat_dex_files;
  uint32_t number_of_dex_files = file.GetOatHeader().GetDexFileCount();
  for (uint32_t i = 0; i < number_of_dex_files; i++) {
    std::string dex = DexFileLoader::GetMultiDexLocation(i, dex_location_.c_str());
    const OatDexFile* oat_dex_file = file.GetOatDexFile(dex.c_str());
    if (oat_dex_file == nullptr) {
      *error_msg = StringPrintf("failed to find %s in %s", dex.c_str(), file.GetLocation().c_str());
      return false;
    }
    oat_dex_files.push_back(oat_dex_file);
  }
  uint32_t oat_checksum = DexFileLoader::GetMultiDexChecksum(oat_dex_files);

  CHECK(dex_checksum.has_value());
  if (dex_checksum != oat_checksum) {
    VLOG(oat) << "Checksum does not match: " << std::hex << file.GetLocation() << " ("
              << oat_checksum << ") vs " << dex_location_ << " (" << *dex_checksum << ")";
    return false;
  }

  return true;
}

OatFileAssistant::OatStatus OatFileAssistant::GivenOatFileStatus(const OatFile& file,
                                                                 /*out*/ std::string* error_msg) {
  // Verify the ART_USE_READ_BARRIER state.
  // TODO: Don't fully reject files due to read barrier state. If they contain
  // compiled code and are otherwise okay, we should return something like
  // kOatRelocationOutOfDate. If they don't contain compiled code, the read
  // barrier state doesn't matter.
  if (file.GetOatHeader().IsConcurrentCopying() != gUseReadBarrier) {
    *error_msg = "Read barrier state mismatch";
    return kOatCannotOpen;
  }

  // Verify the dex checksum.
  if (!DexChecksumUpToDate(file, error_msg)) {
    LOG(ERROR) << *error_msg;
    return kOatDexOutOfDate;
  }

  CompilerFilter::Filter current_compiler_filter = file.GetCompilerFilter();

  // Verify the image checksum
  if (!file.IsBackedByVdexOnly() &&
      CompilerFilter::DependsOnImageChecksum(current_compiler_filter)) {
    if (!ValidateBootClassPathChecksums(file, error_msg)) {
      return kOatBootImageOutOfDate;
    }
    if (!gc::space::ImageSpace::ValidateApexVersions(
            file, GetOatFileAssistantContext()->GetApexVersions(), error_msg)) {
      return kOatBootImageOutOfDate;
    }
  }

  // The constraint is only enforced if the zip has uncompressed dex code.
  if (only_load_trusted_executable_ &&
      !LocationIsTrusted(file.GetLocation(), !GetRuntimeOptions().deny_art_apex_data_files) &&
      file.ContainsDexCode() && ZipFileOnlyContainsUncompressedDex()) {
    *error_msg = "Oat file has dex code, but APK has uncompressed dex code";
    LOG(ERROR) << "Not loading " << dex_location_ << ": " << *error_msg;
    return kOatDexOutOfDate;
  }

  if (!ClassLoaderContextIsOkay(file, error_msg)) {
    return kOatContextOutOfDate;
  }

  return kOatUpToDate;
}

bool OatFileAssistant::AnonymousDexVdexLocation(const std::vector<const DexFile::Header*>& headers,
                                                InstructionSet isa,
                                                /* out */ std::string* dex_location,
                                                /* out */ std::string* vdex_filename) {
  // Normally, OatFileAssistant should not assume that there is an active runtime. However, we
  // reference the runtime here. This is okay because we are in a static function that is unrelated
  // to other parts of OatFileAssistant.
  DCHECK(Runtime::Current() != nullptr);

  uint32_t checksum = adler32(0L, Z_NULL, 0);
  for (const DexFile::Header* header : headers) {
    checksum = adler32_combine(
        checksum, header->checksum_, header->file_size_ - DexFile::kNumNonChecksumBytes);
  }

  const std::string& data_dir = Runtime::Current()->GetProcessDataDirectory();
  if (data_dir.empty() || Runtime::Current()->IsZygote()) {
    *dex_location = StringPrintf("%s%u", kAnonymousDexPrefix, checksum);
    return false;
  }
  *dex_location = StringPrintf("%s/%s%u.jar", data_dir.c_str(), kAnonymousDexPrefix, checksum);

  std::string odex_filename;
  std::string error_msg;
  if (!DexLocationToOdexFilename(*dex_location, isa, &odex_filename, &error_msg)) {
    LOG(WARNING) << "Could not get odex filename for " << *dex_location << ": " << error_msg;
    return false;
  }

  *vdex_filename = GetVdexFilename(odex_filename);
  return true;
}

bool OatFileAssistant::IsAnonymousVdexBasename(const std::string& basename) {
  DCHECK(basename.find('/') == std::string::npos);
  // `basename` must have format: <kAnonymousDexPrefix><checksum><kVdexExtension>
  if (basename.size() < strlen(kAnonymousDexPrefix) + strlen(kVdexExtension) + 1 ||
      !basename.starts_with(kAnonymousDexPrefix) ||
      !basename.ends_with(kVdexExtension)) {
    return false;
  }
  // Check that all characters between the prefix and extension are decimal digits.
  for (size_t i = strlen(kAnonymousDexPrefix); i < basename.size() - strlen(kVdexExtension); ++i) {
    if (!std::isdigit(basename[i])) {
      return false;
    }
  }
  return true;
}

bool OatFileAssistant::DexLocationToOdexFilename(const std::string& location,
                                                 InstructionSet isa,
                                                 std::string* odex_filename,
                                                 std::string* error_msg) {
  CHECK(odex_filename != nullptr);
  CHECK(error_msg != nullptr);

  // For a DEX file on /apex, check if there is an odex file on /system. If so, and the file exists,
  // use it.
  if (LocationIsOnApex(location)) {
    const std::string system_file = GetSystemOdexFilenameForApex(location, isa);
    if (OS::FileExists(system_file.c_str(), /*check_file_type=*/true)) {
      *odex_filename = system_file;
      return true;
    } else if (errno != ENOENT) {
      PLOG(ERROR) << "Could not check odex file " << system_file;
    }
  }

  // The odex file name is formed by replacing the dex_location extension with
  // .odex and inserting an oat/<isa> directory. For example:
  //   location = /foo/bar/baz.jar
  //   odex_location = /foo/bar/oat/<isa>/baz.odex

  // Find the directory portion of the dex location and add the oat/<isa>
  // directory.
  size_t pos = location.rfind('/');
  if (pos == std::string::npos) {
    *error_msg = "Dex location " + location + " has no directory.";
    return false;
  }
  std::string dir = location.substr(0, pos + 1);
  // Add the oat directory.
  dir += "oat";

  // Add the isa directory
  dir += "/" + std::string(GetInstructionSetString(isa));

  // Get the base part of the file without the extension.
  std::string file = location.substr(pos + 1);
  pos = file.rfind('.');
  std::string base = pos != std::string::npos ? file.substr(0, pos) : file;

  *odex_filename = dir + "/" + base + kOdexExtension;
  return true;
}

bool OatFileAssistant::DexLocationToOatFilename(const std::string& location,
                                                InstructionSet isa,
                                                std::string* oat_filename,
                                                std::string* error_msg) {
  DCHECK(Runtime::Current() != nullptr);
  return DexLocationToOatFilename(
      location, isa, Runtime::Current()->DenyArtApexDataFiles(), oat_filename, error_msg);
}

bool OatFileAssistant::DexLocationToOatFilename(const std::string& location,
                                                InstructionSet isa,
                                                bool deny_art_apex_data_files,
                                                std::string* oat_filename,
                                                std::string* error_msg) {
  CHECK(oat_filename != nullptr);
  CHECK(error_msg != nullptr);

  // Check if `location` could have an oat file in the ART APEX data directory. If so, and the
  // file exists, use it.
  const std::string apex_data_file = GetApexDataOdexFilename(location, isa);
  if (!apex_data_file.empty() && !deny_art_apex_data_files) {
    if (OS::FileExists(apex_data_file.c_str(), /*check_file_type=*/true)) {
      *oat_filename = apex_data_file;
      return true;
    } else if (errno != ENOENT) {
      PLOG(ERROR) << "Could not check odex file " << apex_data_file;
    }
  }

  // If ANDROID_DATA is not set, return false instead of aborting.
  // This can occur for preopt when using a class loader context.
  if (GetAndroidDataSafe(error_msg).empty()) {
    *error_msg = "GetAndroidDataSafe failed: " + *error_msg;
    return false;
  }

  std::string dalvik_cache;
  bool have_android_data = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = false;
  GetDalvikCache(GetInstructionSetString(isa),
                 /*create_if_absent=*/true,
                 &dalvik_cache,
                 &have_android_data,
                 &dalvik_cache_exists,
                 &is_global_cache);
  if (!dalvik_cache_exists) {
    *error_msg = "Dalvik cache directory does not exist";
    return false;
  }

  // TODO: The oat file assistant should be the definitive place for
  // determining the oat file name from the dex location, not
  // GetDalvikCacheFilename.
  return GetDalvikCacheFilename(location, dalvik_cache, oat_filename, error_msg);
}

bool OatFileAssistant::GetRequiredDexChecksum(std::optional<uint32_t>* checksum,
                                              std::string* error) {
  if (!required_dex_checksums_attempted_) {
    required_dex_checksums_attempted_ = true;

    File file(zip_fd_, /*check_usage=*/false);
    ArtDexFileLoader dex_loader(&file, dex_location_);
    std::optional<uint32_t> checksum2;
    std::string error2;
    if (dex_loader.GetMultiDexChecksum(
            &checksum2, &error2, &zip_file_only_contains_uncompressed_dex_)) {
      cached_required_dex_checksums_ = checksum2;
      cached_required_dex_checksums_error_ = std::nullopt;
    } else {
      cached_required_dex_checksums_ = std::nullopt;
      cached_required_dex_checksums_error_ = error2;
    }
    file.Release();  // Don't close the file yet (we have only read the checksum).
  }

  if (cached_required_dex_checksums_error_.has_value()) {
    *error = cached_required_dex_checksums_error_.value();
    DCHECK(!error->empty());
    return false;
  }

  if (!cached_required_dex_checksums_.has_value()) {
    // The only valid case here is for APKs without dex files.
    VLOG(oat) << "No dex file found in " << dex_location_;
  }
  *checksum = cached_required_dex_checksums_;
  return true;
}

bool OatFileAssistant::ValidateBootClassPathChecksums(OatFileAssistantContext* ofa_context,
                                                      InstructionSet isa,
                                                      std::string_view oat_checksums,
                                                      std::string_view oat_boot_class_path,
                                                      /*out*/ std::string* error_msg) {
  const std::vector<std::string>& bcp_locations =
      ofa_context->GetRuntimeOptions().boot_class_path_locations;

  if (oat_checksums.empty() || oat_boot_class_path.empty()) {
    *error_msg = oat_checksums.empty() ? "Empty checksums" : "Empty boot class path";
    return false;
  }

  size_t oat_bcp_size = gc::space::ImageSpace::CheckAndCountBCPComponents(
      oat_boot_class_path, ArrayRef<const std::string>(bcp_locations), error_msg);
  if (oat_bcp_size == static_cast<size_t>(-1)) {
    DCHECK(!error_msg->empty());
    return false;
  }
  DCHECK_LE(oat_bcp_size, bcp_locations.size());

  size_t bcp_index = 0;
  size_t boot_image_index = 0;
  bool found_d = false;

  while (bcp_index < oat_bcp_size) {
    static_assert(gc::space::ImageSpace::kImageChecksumPrefix == 'i', "Format prefix check");
    static_assert(gc::space::ImageSpace::kDexFileChecksumPrefix == 'd', "Format prefix check");
    if (oat_checksums.starts_with("i") && !found_d) {
      const std::vector<OatFileAssistantContext::BootImageInfo>& boot_image_info_list =
          ofa_context->GetBootImageInfoList(isa);
      if (boot_image_index >= boot_image_info_list.size()) {
        *error_msg = StringPrintf("Missing boot image for %s, remaining checksums: %s",
                                  bcp_locations[bcp_index].c_str(),
                                  std::string(oat_checksums).c_str());
        return false;
      }

      const OatFileAssistantContext::BootImageInfo& boot_image_info =
          boot_image_info_list[boot_image_index];
      if (!ConsumePrefix(&oat_checksums, boot_image_info.checksum)) {
        *error_msg = StringPrintf("Image checksum mismatch, expected %s to start with %s",
                                  std::string(oat_checksums).c_str(),
                                  boot_image_info.checksum.c_str());
        return false;
      }

      bcp_index += boot_image_info.component_count;
      boot_image_index++;
    } else if (oat_checksums.starts_with("d")) {
      found_d = true;
      const std::vector<std::string>* bcp_checksums =
          ofa_context->GetBcpChecksums(bcp_index, error_msg);
      if (bcp_checksums == nullptr) {
        return false;
      }
      oat_checksums.remove_prefix(1u);
      for (const std::string& checksum : *bcp_checksums) {
        if (!ConsumePrefix(&oat_checksums, checksum)) {
          *error_msg = StringPrintf(
              "Dex checksum mismatch for bootclasspath file %s, expected %s to start with %s",
              bcp_locations[bcp_index].c_str(),
              std::string(oat_checksums).c_str(),
              checksum.c_str());
          return false;
        }
      }

      bcp_index++;
    } else {
      *error_msg = StringPrintf("Unexpected checksums, expected %s to start with %s",
                                std::string(oat_checksums).c_str(),
                                found_d ? "'d'" : "'i' or 'd'");
      return false;
    }

    if (bcp_index < oat_bcp_size) {
      if (!ConsumePrefix(&oat_checksums, ":")) {
        if (oat_checksums.empty()) {
          *error_msg =
              StringPrintf("Checksum too short, missing %zu components", oat_bcp_size - bcp_index);
        } else {
          *error_msg = StringPrintf("Missing ':' separator at start of %s",
                                    std::string(oat_checksums).c_str());
        }
        return false;
      }
    }
  }

  if (!oat_checksums.empty()) {
    *error_msg =
        StringPrintf("Checksum too long, unexpected tail: %s", std::string(oat_checksums).c_str());
    return false;
  }

  return true;
}

bool OatFileAssistant::ValidateBootClassPathChecksums(const OatFile& oat_file,
                                                      /*out*/ std::string* error_msg) {
  // Get the checksums and the BCP from the oat file.
  const char* oat_boot_class_path_checksums =
      oat_file.GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
  const char* oat_boot_class_path =
      oat_file.GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathKey);
  if (oat_boot_class_path_checksums == nullptr || oat_boot_class_path == nullptr) {
    *error_msg = "Missing boot image information from oat file";
    return false;
  }

  return ValidateBootClassPathChecksums(GetOatFileAssistantContext(),
                                        isa_,
                                        oat_boot_class_path_checksums,
                                        oat_boot_class_path,
                                        error_msg);
}

bool OatFileAssistant::IsPrimaryBootImageUsable() {
  return !GetOatFileAssistantContext()->GetBootImageInfoList(isa_).empty();
}

OatFileAssistant::OatFileInfo& OatFileAssistant::GetBestInfo() {
  ScopedTrace trace("GetBestInfo");

  for (const std::unique_ptr<OatFileInfo>& info : info_list_) {
    if (VLOG_IS_ON(oat) && info->FileExists()) {
      std::string error_msg;
      OatStatus status = info->Status(&error_msg);
      std::string message = ART_FORMAT("GetBestInfo: {} ({}) is {}",
                                       info->GetLocationDebugString(),
                                       info->DisplayFilename(),
                                       fmt::streamed(status));
      const OatFile* file = info->GetFile();
      if (file != nullptr) {
        message += ART_FORMAT(" with filter '{}' executable '{}'",
                              fmt::streamed(file->GetCompilerFilter()),
                              file->IsExecutable());
      }
      if (!info->IsUseable()) {
        message += ": " + error_msg;
      }
      VLOG(oat) << message;
    }

    if (info->IsUseable()) {
      return *info;
    }
  }

  // No usable artifact. Pick the oat or odex if they exist, or empty info if not.
  VLOG(oat) << ART_FORMAT("GetBestInfo: {} has no usable artifacts", dex_location_);
  for (const std::unique_ptr<OatFileInfo>& info : info_list_) {
    if (info->GetType() == OatFileType::kOat && info->Status() != kOatCannotOpen) {
      return *info;
    }
  }
  return empty_info_;
}

std::unique_ptr<gc::space::ImageSpace> OatFileAssistant::OpenImageSpace(const OatFile* oat_file) {
  DCHECK(oat_file != nullptr);
  std::string art_file = ReplaceFileExtension(oat_file->GetLocation(), kArtExtension);
  if (art_file.empty()) {
    return nullptr;
  }
  std::string error_msg;
  std::unique_ptr<gc::space::ImageSpace> ret =
      gc::space::ImageSpace::CreateFromAppImage(art_file.c_str(), oat_file, &error_msg);
  if (ret == nullptr && (VLOG_IS_ON(image) || OS::FileExists(art_file.c_str()))) {
    LOG(INFO) << "Failed to open app image " << art_file.c_str() << " " << error_msg;
  }
  return ret;
}

bool OatFileAssistant::OatFileInfo::IsOatLocation() const { return is_oat_location_; }

const std::string* OatFileAssistant::OatFileInfo::Filename() const { return &filename_; }

const char* OatFileAssistant::OatFileInfo::DisplayFilename() const {
  return !filename_.empty() ? filename_.c_str() : "unknown";
}

bool OatFileAssistant::OatFileInfo::IsUseable() {
  ScopedTrace trace("IsUseable");
  switch (Status()) {
    case kOatCannotOpen:
    case kOatDexOutOfDate:
    case kOatContextOutOfDate:
    case kOatBootImageOutOfDate:
      return false;

    case kOatUpToDate:
      return true;
  }
}

OatFileAssistant::OatStatus OatFileAssistant::OatFileInfo::Status(/*out*/ std::string* error_msg) {
  ScopedTrace trace("Status");
  if (!status_.has_value()) {
    std::string temp_error_msg;
    const OatFile* file = GetFile(&temp_error_msg);
    if (file == nullptr) {
      status_ = std::make_pair(kOatCannotOpen, std::move(temp_error_msg));
    } else {
      status_ = std::make_pair(oat_file_assistant_->GivenOatFileStatus(*file, &temp_error_msg),
                               std::move(temp_error_msg));
    }
  }
  if (error_msg != nullptr) {
    *error_msg = status_->second;
  }
  return status_->first;
}

OatFileAssistant::DexOptNeeded OatFileAssistant::OatFileInfo::GetDexOptNeeded(
    CompilerFilter::Filter target_compiler_filter, const DexOptTrigger dexopt_trigger) {
  if (IsUseable()) {
    return ShouldRecompileForFilter(target_compiler_filter, dexopt_trigger) ? kDex2OatForFilter :
                                                                              kNoDexOptNeeded;
  }

  // In this case, the oat file is not usable. If the caller doesn't seek for a better compiler
  // filter (e.g., the caller wants to downgrade), then we should not recompile.
  if (!dexopt_trigger.targetFilterIsBetter) {
    return kNoDexOptNeeded;
  }

  if (Status() == kOatBootImageOutOfDate) {
    return kDex2OatForBootImage;
  }

  std::string error_msg;
  std::optional<bool> has_dex_files = oat_file_assistant_->HasDexFiles(&error_msg);
  if (has_dex_files.has_value()) {
    if (*has_dex_files) {
      return kDex2OatFromScratch;
    } else {
      // No dex file, so there is nothing we need to do.
      return kNoDexOptNeeded;
    }
  } else {
    // Unable to open the dex file, so there is nothing we can do.
    LOG(WARNING) << error_msg;
    return kNoDexOptNeeded;
  }
}

bool OatFileAssistant::OatFileInfo::FileExists() const {
  return !filename_.empty() && OS::FileExists(filename_.c_str());
}

bool OatFileAssistant::OatFileInfoBackedByOat::FileExists() const {
  return use_fd_ || OatFileInfo::FileExists();
}

bool OatFileAssistant::OatFileInfoBackedBySdm::FileExists() const {
  return OatFileInfo::FileExists() && OS::FileExists(sdc_filename_.c_str());
}

bool OatFileAssistant::OatFileInfoBackedByVdex::FileExists() const {
  return use_fd_ || OatFileInfo::FileExists();
}

const OatFile* OatFileAssistant::OatFileInfo::GetFile(/*out*/ std::string* error_msg) {
  CHECK(!file_released_) << "GetFile called after oat file released.";

  if (!file_.has_value()) {
    if (LocationIsOnArtApexData(filename_) &&
        oat_file_assistant_->GetRuntimeOptions().deny_art_apex_data_files) {
      file_ = std::make_pair(nullptr, "ART apexdata is untrusted");
      LOG(WARNING) << "OatFileAssistant rejected file " << filename_ << ": " << file_->second;
    } else {
      std::string temp_error_msg;
      file_ = std::make_pair(LoadFile(&temp_error_msg), std::move(temp_error_msg));
    }
  }

  if (error_msg != nullptr) {
    *error_msg = file_->second;
  }
  return file_->first.get();
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfoBackedByOat::LoadFile(
    std::string* error_msg) const {
  bool executable = oat_file_assistant_->load_executable_;
  if (executable && oat_file_assistant_->only_load_trusted_executable_) {
    executable = LocationIsTrusted(filename_, /*trust_art_apex_data_files=*/true);
  }

  if (use_fd_) {
    if (oat_fd_ < 0 || vdex_fd_ < 0) {
      *error_msg = "oat_fd or vdex_fd not provided";
      return nullptr;
    }
    ArrayRef<const std::string> dex_locations(&oat_file_assistant_->dex_location_,
                                              /*size=*/1u);
    return std::unique_ptr<OatFile>(OatFile::Open(zip_fd_,
                                                  vdex_fd_,
                                                  oat_fd_,
                                                  filename_,
                                                  executable,
                                                  /*low_4gb=*/false,
                                                  dex_locations,
                                                  /*dex_files=*/{},
                                                  /*reservation=*/nullptr,
                                                  error_msg));
  } else {
    return std::unique_ptr<OatFile>(OatFile::Open(/*zip_fd=*/-1,
                                                  filename_,
                                                  filename_,
                                                  executable,
                                                  /*low_4gb=*/false,
                                                  oat_file_assistant_->dex_location_,
                                                  error_msg));
  }
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfoBackedBySdm::LoadFile(
    std::string* error_msg) const {
  bool executable = oat_file_assistant_->load_executable_;
  if (executable && oat_file_assistant_->only_load_trusted_executable_) {
    executable = LocationIsTrusted(filename_, /*trust_art_apex_data_files=*/true);
  }

  return std::unique_ptr<OatFile>(OatFile::OpenFromSdm(filename_,
                                                       sdc_filename_,
                                                       dm_filename_,
                                                       oat_file_assistant_->dex_location_,
                                                       executable,
                                                       error_msg));
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfoBackedByVdex::LoadFile(
    std::string* error_msg) const {
  // Check to see if there is a vdex file we can make use of.
  std::unique_ptr<VdexFile> vdex;
  if (use_fd_) {
    if (vdex_fd_ < 0) {
      *error_msg = "vdex_fd not provided";
      return nullptr;
    }
    struct stat s;
    if (fstat(vdex_fd_, &s) < 0) {
      *error_msg = ART_FORMAT("Failed getting length of the vdex file: {}", strerror(errno));
      return nullptr;
    }
    vdex = VdexFile::Open(vdex_fd_,
                          s.st_size,
                          filename_,
                          /*low_4gb=*/false,
                          error_msg);
  } else {
    vdex = VdexFile::Open(filename_,
                          /*low_4gb=*/false,
                          error_msg);
  }
  if (vdex == nullptr) {
    *error_msg = ART_FORMAT("Unable to open vdex file: {}", *error_msg);
    return nullptr;
  }
  return std::unique_ptr<OatFile>(OatFile::OpenFromVdex(zip_fd_,
                                                        std::move(vdex),
                                                        oat_file_assistant_->dex_location_,
                                                        oat_file_assistant_->context_,
                                                        error_msg));
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfoBackedByDm::LoadFile(
    std::string* error_msg) const {
  // Check to see if there is a vdex file we can make use of.
  std::unique_ptr<ZipArchive> dm_file(ZipArchive::Open(filename_.c_str(), error_msg));
  if (dm_file == nullptr) {
    return nullptr;
  }
  std::unique_ptr<VdexFile> vdex(VdexFile::OpenFromDm(filename_, *dm_file, error_msg));
  if (vdex == nullptr) {
    return nullptr;
  }
  return std::unique_ptr<OatFile>(OatFile::OpenFromVdex(/*zip_fd=*/-1,
                                                        std::move(vdex),
                                                        oat_file_assistant_->dex_location_,
                                                        oat_file_assistant_->context_,
                                                        error_msg));
}

bool OatFileAssistant::OatFileInfo::ShouldRecompileForFilter(CompilerFilter::Filter target,
                                                             const DexOptTrigger dexopt_trigger) {
  const OatFile* file = GetFile();
  DCHECK(file != nullptr);

  CompilerFilter::Filter current = file->GetCompilerFilter();
  if (dexopt_trigger.targetFilterIsBetter && CompilerFilter::IsBetter(target, current)) {
    VLOG(oat) << ART_FORMAT("Should recompile: targetFilterIsBetter (current: {}, target: {})",
                            CompilerFilter::NameOfFilter(current),
                            CompilerFilter::NameOfFilter(target));
    return true;
  }
  if (dexopt_trigger.targetFilterIsSame && current == target) {
    VLOG(oat) << ART_FORMAT("Should recompile: targetFilterIsSame (current: {}, target: {})",
                            CompilerFilter::NameOfFilter(current),
                            CompilerFilter::NameOfFilter(target));
    return true;
  }
  if (dexopt_trigger.targetFilterIsWorse && CompilerFilter::IsBetter(current, target)) {
    VLOG(oat) << ART_FORMAT("Should recompile: targetFilterIsWorse (current: {}, target: {})",
                            CompilerFilter::NameOfFilter(current),
                            CompilerFilter::NameOfFilter(target));
    return true;
  }

  // Don't regress the compiler filter for the triggers handled below.
  if (CompilerFilter::IsBetter(current, target)) {
    VLOG(oat) << "Should not recompile: current filter is better";
    return false;
  }

  if (dexopt_trigger.primaryBootImageBecomesUsable &&
      CompilerFilter::IsAotCompilationEnabled(current)) {
    // If the oat file has been compiled without an image, and the runtime is
    // now running with an image loaded from disk, return that we need to
    // re-compile. The recompilation will generate a better oat file, and with an app
    // image for profile guided compilation.
    // However, don't recompile for "verify". Although verification depends on the boot image, the
    // penalty of being verified without a boot image is low. Consider the case where a dex file
    // is verified by "ab-ota", we don't want it to be re-verified by "boot-after-ota".
    const char* oat_boot_class_path_checksums =
        file->GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
    if (oat_boot_class_path_checksums != nullptr &&
        oat_boot_class_path_checksums[0] != 'i' &&
        oat_file_assistant_->IsPrimaryBootImageUsable()) {
      DCHECK(!file->GetOatHeader().RequiresImage());
      VLOG(oat) << "Should recompile: primaryBootImageBecomesUsable";
      return true;
    }
  }

  if (dexopt_trigger.needExtraction && !file->ContainsDexCode() &&
      !oat_file_assistant_->ZipFileOnlyContainsUncompressedDex()) {
    VLOG(oat) << "Should recompile: needExtraction";
    return true;
  }

  VLOG(oat) << "Should not recompile";
  return false;
}

bool OatFileAssistant::ClassLoaderContextIsOkay(const OatFile& oat_file,
                                                /*out*/ std::string* error_msg) const {
  if (context_ == nullptr) {
    // The caller requests to skip the check.
    return true;
  }

  if (oat_file.IsBackedByVdexOnly()) {
    // Only a vdex file, we don't depend on the class loader context.
    return true;
  }

  if (!CompilerFilter::IsVerificationEnabled(oat_file.GetCompilerFilter())) {
    // If verification is not enabled we don't need to verify the class loader context and we
    // assume it's ok.
    return true;
  }

  ClassLoaderContext::VerificationResult matches =
      context_->VerifyClassLoaderContextMatch(oat_file.GetClassLoaderContext(),
                                              /*verify_names=*/true,
                                              /*verify_checksums=*/true);
  if (matches == ClassLoaderContext::VerificationResult::kMismatch) {
    *error_msg =
        ART_FORMAT("ClassLoaderContext check failed. Context was {}. The expected context is {}",
                   oat_file.GetClassLoaderContext(),
                   context_->EncodeContextForOatFile(android::base::Dirname(dex_location_)));
    return false;
  }
  return true;
}

bool OatFileAssistant::OatFileInfo::IsExecutable() {
  const OatFile* file = GetFile();
  return (file != nullptr && file->IsExecutable());
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfo::ReleaseFile() {
  file_released_ = true;
  return std::move(file_->first);
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfo::ReleaseFileForUse() {
  ScopedTrace trace("ReleaseFileForUse");
  if (Status() == kOatUpToDate) {
    return ReleaseFile();
  }

  return std::unique_ptr<OatFile>();
}

// TODO(calin): we could provide a more refined status here
// (e.g. run from uncompressed apk, run with vdex but not oat etc). It will allow us to
// track more experiments but adds extra complexity.
void OatFileAssistant::GetOptimizationStatus(const std::string& filename,
                                             InstructionSet isa,
                                             std::string* out_compilation_filter,
                                             std::string* out_compilation_reason,
                                             OatFileAssistantContext* ofa_context) {
  // It may not be possible to load an oat file executable (e.g., selinux restrictions). Load
  // non-executable and check the status manually.
  OatFileAssistant oat_file_assistant(filename.c_str(),
                                      isa,
                                      /*context=*/nullptr,
                                      /*load_executable=*/false,
                                      /*only_load_trusted_executable=*/false,
                                      ofa_context);
  std::string out_odex_location;  // unused
  std::string out_odex_status;    // unused
  Location out_location;          // unused
  oat_file_assistant.GetOptimizationStatus(&out_odex_location,
                                           out_compilation_filter,
                                           out_compilation_reason,
                                           &out_odex_status,
                                           &out_location);
}

void OatFileAssistant::GetOptimizationStatus(std::string* out_odex_location,
                                             std::string* out_compilation_filter,
                                             std::string* out_compilation_reason,
                                             std::string* out_odex_status,
                                             Location* out_location) {
  OatFileInfo& oat_file_info = GetBestInfo();
  const OatFile* oat_file = oat_file_info.GetFile();
  *out_location = GetLocation(oat_file_info);

  if (oat_file == nullptr) {
    std::string error_msg;
    std::optional<bool> has_dex_files = HasDexFiles(&error_msg);
    if (!has_dex_files.has_value()) {
      *out_odex_location = "error";
      *out_compilation_filter = "unknown";
      *out_compilation_reason = "unknown";
      // This happens when we cannot open the APK/JAR.
      *out_odex_status = "io-error-no-apk";
    } else if (!has_dex_files.value()) {
      *out_odex_location = "none";
      *out_compilation_filter = "unknown";
      *out_compilation_reason = "unknown";
      // This happens when the APK/JAR doesn't contain any DEX file.
      *out_odex_status = "no-dex-code";
    } else {
      *out_odex_location = "error";
      *out_compilation_filter = "run-from-apk";
      *out_compilation_reason = "unknown";
      // This mostly happens when we cannot open the oat file.
      // Note that it's different than kOatCannotOpen.
      // TODO: The design of getting the BestInfo is not ideal, as it's not very clear what's the
      // difference between a nullptr and kOatcannotOpen. The logic should be revised and improved.
      *out_odex_status = "io-error-no-oat";
    }
    return;
  }

  *out_odex_location = oat_file->GetLocation();
  OatStatus status = oat_file_info.Status();
  const char* reason = oat_file->GetCompilationReason();
  *out_compilation_reason = reason == nullptr ? "unknown" : reason;

  // If the oat file is invalid, the vdex file will be picked, so the status is `kOatUpToDate`. If
  // the vdex file is also invalid, then either `oat_file` is nullptr, or `status` is
  // `kOatDexOutOfDate`.
  DCHECK(status == kOatUpToDate || status == kOatDexOutOfDate);

  switch (status) {
    case kOatUpToDate:
      *out_compilation_filter = CompilerFilter::NameOfFilter(oat_file->GetCompilerFilter());
      *out_odex_status = "up-to-date";
      return;

    case kOatCannotOpen:
    case kOatBootImageOutOfDate:
    case kOatContextOutOfDate:
      // These should never happen, but be robust.
      *out_compilation_filter = "unexpected";
      *out_compilation_reason = "unexpected";
      *out_odex_status = "unexpected";
      return;

    case kOatDexOutOfDate:
      *out_compilation_filter = "run-from-apk-fallback";
      *out_odex_status = "apk-more-recent";
      return;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

bool OatFileAssistant::ZipFileOnlyContainsUncompressedDex() {
  // zip_file_only_contains_uncompressed_dex_ is only set during fetching the dex checksums.
  std::optional<uint32_t> checksum;
  std::string error_msg;
  if (!GetRequiredDexChecksum(&checksum, &error_msg)) {
    LOG(ERROR) << error_msg;
  }
  return zip_file_only_contains_uncompressed_dex_;
}

OatFileAssistant::Location OatFileAssistant::GetLocation(OatFileInfo& info) {
  if (info.IsUseable()) {
    if (info.GetType() == OatFileType::kSdm) {
      if (info.IsOatLocation()) {
        return kLocationSdmOat;
      } else {
        return kLocationSdmOdex;
      }
    } else if (info.GetType() == OatFileType::kDm) {
      return kLocationDm;
    } else if (info.IsOatLocation()) {
      return kLocationOat;
    } else {
      return kLocationOdex;
    }
  } else {
    return kLocationNoneOrError;
  }
}

}  // namespace art
