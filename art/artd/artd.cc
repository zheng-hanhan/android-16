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

#include "artd.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ostream>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aidl/com/android/server/art/ArtConstants.h"
#include "aidl/com/android/server/art/BnArtd.h"
#include "aidl/com/android/server/art/DexoptTrigger.h"
#include "aidl/com/android/server/art/IArtdCancellationSignal.h"
#include "aidl/com/android/server/art/IArtdNotification.h"
#include "android-base/errors.h"
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/parseint.h"
#include "android-base/result.h"
#include "android-base/scopeguard.h"
#include "android-base/strings.h"
#include "android-base/unique_fd.h"
#include "android/binder_auto_utils.h"
#include "android/binder_interface_utils.h"
#include "android/binder_manager.h"
#include "android/binder_process.h"
#include "base/compiler_filter.h"
#include "base/file_magic.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/mem_map.h"
#include "base/memfd.h"
#include "base/os.h"
#include "base/pidfd.h"
#include "base/time_utils.h"
#include "base/zip_archive.h"
#include "cmdline_types.h"
#include "dex/dex_file_loader.h"
#include "exec_utils.h"
#include "file_utils.h"
#include "fstab/fstab.h"
#include "oat/oat_file_assistant.h"
#include "oat/oat_file_assistant_context.h"
#include "oat/sdc_file.h"
#include "odrefresh/odrefresh.h"
#include "path_utils.h"
#include "profman/profman_result.h"
#include "selinux/android.h"
#include "service.h"
#include "tools/binder_utils.h"
#include "tools/cmdline_builder.h"
#include "tools/tools.h"

namespace art {
namespace artd {

namespace {

using ::aidl::com::android::server::art::ArtConstants;
using ::aidl::com::android::server::art::ArtdDexoptResult;
using ::aidl::com::android::server::art::ArtifactsLocation;
using ::aidl::com::android::server::art::ArtifactsPath;
using ::aidl::com::android::server::art::CopyAndRewriteProfileResult;
using ::aidl::com::android::server::art::DexMetadataPath;
using ::aidl::com::android::server::art::DexoptOptions;
using ::aidl::com::android::server::art::DexoptTrigger;
using ::aidl::com::android::server::art::FileVisibility;
using ::aidl::com::android::server::art::FsPermission;
using ::aidl::com::android::server::art::GetDexoptNeededResult;
using ::aidl::com::android::server::art::GetDexoptStatusResult;
using ::aidl::com::android::server::art::IArtdCancellationSignal;
using ::aidl::com::android::server::art::IArtdNotification;
using ::aidl::com::android::server::art::MergeProfileOptions;
using ::aidl::com::android::server::art::OutputArtifacts;
using ::aidl::com::android::server::art::OutputProfile;
using ::aidl::com::android::server::art::OutputSecureDexMetadataCompanion;
using ::aidl::com::android::server::art::PriorityClass;
using ::aidl::com::android::server::art::ProfilePath;
using ::aidl::com::android::server::art::RuntimeArtifactsPath;
using ::aidl::com::android::server::art::SecureDexMetadataWithCompanionPaths;
using ::aidl::com::android::server::art::VdexPath;
using ::android::base::Basename;
using ::android::base::Dirname;
using ::android::base::ErrnoError;
using ::android::base::Error;
using ::android::base::Join;
using ::android::base::make_scope_guard;
using ::android::base::ParseInt;
using ::android::base::ReadFileToString;
using ::android::base::Result;
using ::android::base::Split;
using ::android::base::Tokenize;
using ::android::base::Trim;
using ::android::base::unique_fd;
using ::android::base::WriteStringToFd;
using ::android::base::WriteStringToFile;
using ::android::fs_mgr::FstabEntry;
using ::art::service::ValidateClassLoaderContext;
using ::art::service::ValidateDexPath;
using ::art::tools::CmdlineBuilder;
using ::art::tools::Fatal;
using ::art::tools::GetProcMountsAncestorsOfPath;
using ::art::tools::NonFatal;
using ::ndk::ScopedAStatus;

using PrimaryCurProfilePath = ProfilePath::PrimaryCurProfilePath;
using TmpProfilePath = ProfilePath::TmpProfilePath;
using WritableProfilePath = ProfilePath::WritableProfilePath;

constexpr const char* kServiceName = "artd";
constexpr const char* kPreRebootServiceName = "artd_pre_reboot";
constexpr const char* kArtdCancellationSignalType = "ArtdCancellationSignal";
constexpr const char* kDefaultPreRebootTmpDir = "/mnt/artd_tmp";

// Timeout for short operations, such as merging profiles.
constexpr int kShortTimeoutSec = 60;  // 1 minute.

// Timeout for long operations, such as compilation. We set it to be smaller than the Package
// Manager watchdog (PackageManagerService.WATCHDOG_TIMEOUT, 10 minutes), so that if the operation
// is called from the Package Manager's thread handler, it will be aborted before that watchdog
// would take down the system server.
constexpr int kLongTimeoutSec = 570;  // 9.5 minutes.

std::optional<int64_t> GetSize(std::string_view path) {
  std::error_code ec;
  int64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    // It is okay if the file does not exist. We don't have to log it.
    if (ec.value() != ENOENT) {
      LOG(ERROR) << ART_FORMAT("Failed to get the file size of '{}': {}", path, ec.message());
    }
    return std::nullopt;
  }
  return size;
}

bool DeleteFile(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    LOG(ERROR) << ART_FORMAT("Failed to remove '{}': {}", path, ec.message());
    return false;
  }
  return true;
}

// Deletes a file. Returns the size of the deleted file, or 0 if the deleted file is empty or an
// error occurs.
int64_t GetSizeAndDeleteFile(const std::string& path) {
  std::optional<int64_t> size = GetSize(path);
  if (!size.has_value()) {
    return 0;
  }
  if (!DeleteFile(path)) {
    return 0;
  }
  return size.value();
}

Result<CompilerFilter::Filter> ParseCompilerFilter(const std::string& compiler_filter_str) {
  CompilerFilter::Filter compiler_filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter_str.c_str(), &compiler_filter)) {
    return Errorf("Failed to parse compiler filter '{}'", compiler_filter_str);
  }
  return compiler_filter;
}

OatFileAssistant::DexOptTrigger DexOptTriggerFromAidl(int32_t aidl_value) {
  OatFileAssistant::DexOptTrigger trigger{};
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::COMPILER_FILTER_IS_BETTER)) != 0) {
    trigger.targetFilterIsBetter = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::COMPILER_FILTER_IS_SAME)) != 0) {
    trigger.targetFilterIsSame = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::COMPILER_FILTER_IS_WORSE)) != 0) {
    trigger.targetFilterIsWorse = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::PRIMARY_BOOT_IMAGE_BECOMES_USABLE)) != 0) {
    trigger.primaryBootImageBecomesUsable = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::NEED_EXTRACTION)) != 0) {
    trigger.needExtraction = true;
  }
  return trigger;
}

ArtifactsLocation ArtifactsLocationToAidl(OatFileAssistant::Location location) {
  switch (location) {
    case OatFileAssistant::Location::kLocationNoneOrError:
      return ArtifactsLocation::NONE_OR_ERROR;
    case OatFileAssistant::Location::kLocationOat:
      return ArtifactsLocation::DALVIK_CACHE;
    case OatFileAssistant::Location::kLocationOdex:
      return ArtifactsLocation::NEXT_TO_DEX;
    case OatFileAssistant::Location::kLocationDm:
      return ArtifactsLocation::DM;
    case OatFileAssistant::Location::kLocationSdmOat:
      return ArtifactsLocation::SDM_DALVIK_CACHE;
    case OatFileAssistant::Location::kLocationSdmOdex:
      return ArtifactsLocation::SDM_NEXT_TO_DEX;
      // No default. All cases should be explicitly handled, or the compilation will fail.
  }
  // This should never happen. Just in case we get a non-enumerator value.
  LOG(FATAL) << "Unexpected Location " << location;
}

Result<bool> CreateDir(const std::string& path) {
  std::error_code ec;
  bool created = std::filesystem::create_directory(path, ec);
  if (ec) {
    return Errorf("Failed to create directory '{}': {}", path, ec.message());
  }
  return created;
}

Result<void> PrepareArtifactsDir(const std::string& path, const FsPermission& fs_permission) {
  bool created = OR_RETURN(CreateDir(path));

  auto cleanup = make_scope_guard([&] {
    if (created) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  });

  if (chmod(path.c_str(), DirFsPermissionToMode(fs_permission)) != 0) {
    return ErrnoErrorf("Failed to chmod directory '{}'", path);
  }
  OR_RETURN(Chown(path, fs_permission));

  cleanup.Disable();
  return {};
}

Result<void> PrepareArtifactsDirs(const std::string& dex_path,
                                  const std::string& isa_str,
                                  const FsPermission& dir_fs_permission,
                                  /*out*/ std::string* oat_dir_path) {
  std::filesystem::path oat_path(
      OR_RETURN(BuildOatPath(dex_path, isa_str, /*is_in_dalvik_cache=*/false)));
  std::filesystem::path isa_dir = oat_path.parent_path();
  std::filesystem::path oat_dir = isa_dir.parent_path();
  DCHECK_EQ(oat_dir.filename(), "oat");

  OR_RETURN(PrepareArtifactsDir(oat_dir, dir_fs_permission));
  OR_RETURN(PrepareArtifactsDir(isa_dir, dir_fs_permission));
  *oat_dir_path = oat_dir;
  return {};
}

Result<FileVisibility> GetFileVisibility(const std::string& file) {
  std::error_code ec;
  std::filesystem::file_status status = std::filesystem::status(file, ec);
  if (!std::filesystem::status_known(status)) {
    return Errorf("Failed to get status of '{}': {}", file, ec.message());
  }
  if (!std::filesystem::exists(status)) {
    return FileVisibility::NOT_FOUND;
  }

  return (status.permissions() & std::filesystem::perms::others_read) !=
                 std::filesystem::perms::none ?
             FileVisibility::OTHER_READABLE :
             FileVisibility::NOT_OTHER_READABLE;
}

Result<ArtdCancellationSignal*> ToArtdCancellationSignal(IArtdCancellationSignal* input) {
  if (input == nullptr) {
    return Error() << "Cancellation signal must not be nullptr";
  }
  // We cannot use `dynamic_cast` because ART code is compiled with `-fno-rtti`, so we have to check
  // the magic number.
  int64_t type;
  if (!input->getType(&type).isOk() ||
      type != reinterpret_cast<intptr_t>(kArtdCancellationSignalType)) {
    // The cancellation signal must be created by `Artd::createCancellationSignal`.
    return Error() << "Invalid cancellation signal type";
  }
  return static_cast<ArtdCancellationSignal*>(input);
}

Result<void> CopyFile(const std::string& src_path, const NewFile& dst_file) {
  std::string content;
  if (!ReadFileToString(src_path, &content)) {
    return Errorf("Failed to read file '{}': {}", src_path, strerror(errno));
  }
  if (!WriteStringToFd(content, dst_file.Fd())) {
    return Errorf("Failed to write file '{}': {}", dst_file.TempPath(), strerror(errno));
  }
  if (fsync(dst_file.Fd()) != 0) {
    return Errorf("Failed to flush file '{}': {}", dst_file.TempPath(), strerror(errno));
  }
  if (lseek(dst_file.Fd(), /*offset=*/0, SEEK_SET) != 0) {
    return Errorf(
        "Failed to reset the offset for file '{}': {}", dst_file.TempPath(), strerror(errno));
  }
  return {};
}

Result<void> SetLogVerbosity() {
  std::string options =
      android::base::GetProperty("dalvik.vm.artd-verbose", /*default_value=*/"oat");
  if (options.empty()) {
    return {};
  }

  CmdlineType<LogVerbosity> parser;
  CmdlineParseResult<LogVerbosity> result = parser.Parse(options);
  if (!result.IsSuccess()) {
    return Error() << result.GetMessage();
  }

  gLogVerbosity = result.ReleaseValue();
  return {};
}

CopyAndRewriteProfileResult AnalyzeCopyAndRewriteProfileFailure(
    File* src, ProfmanResult::CopyAndUpdateResult result) {
  DCHECK(result == ProfmanResult::kCopyAndUpdateNoMatch ||
         result == ProfmanResult::kCopyAndUpdateErrorFailedToLoadProfile);

  auto bad_profile = [&](std::string_view error_msg) {
    return CopyAndRewriteProfileResult{
        .status = CopyAndRewriteProfileResult::Status::BAD_PROFILE,
        .errorMsg = ART_FORMAT("Failed to load profile '{}': {}", src->GetPath(), error_msg)};
  };
  CopyAndRewriteProfileResult no_profile{.status = CopyAndRewriteProfileResult::Status::NO_PROFILE,
                                         .errorMsg = ""};

  int64_t length = src->GetLength();
  if (length < 0) {
    return bad_profile(strerror(-length));
  }
  if (length == 0) {
    return no_profile;
  }

  std::string error_msg;
  uint32_t magic;
  if (!ReadMagicAndReset(src->Fd(), &magic, &error_msg)) {
    return bad_profile(error_msg);
  }
  if (IsZipMagic(magic)) {
    std::unique_ptr<ZipArchive> zip_archive(
        ZipArchive::OpenFromOwnedFd(src->Fd(), src->GetPath().c_str(), &error_msg));
    if (zip_archive == nullptr) {
      return bad_profile(error_msg);
    }
    std::unique_ptr<ZipEntry> zip_entry(
        zip_archive->Find(ArtConstants::DEX_METADATA_PROFILE_ENTRY, &error_msg));
    if (zip_entry == nullptr || zip_entry->GetUncompressedLength() == 0) {
      return no_profile;
    }
  }

  if (result == ProfmanResult::kCopyAndUpdateNoMatch) {
    return bad_profile(
        "The profile does not match the APK (The checksums in the profile do not match the "
        "checksums of the .dex files in the APK)");
  }
  return bad_profile("The profile is in the wrong format or an I/O error has occurred");
}

// Returns the fd on success, or an invalid fd if the dex file contains no profile, or error if any
// error occurs.
Result<File> ExtractEmbeddedProfileToFd(const std::string& dex_path) {
  std::unique_ptr<File> dex_file = OR_RETURN(OpenFileForReading(dex_path));

  std::string error_msg;
  uint32_t magic;
  if (!ReadMagicAndReset(dex_file->Fd(), &magic, &error_msg)) {
    return Error() << error_msg;
  }
  if (!IsZipMagic(magic)) {
    if (DexFileLoader::IsMagicValid(magic)) {
      // The dex file can be a plain dex file. This is expected.
      return File();
    }
    return Error() << "File is neither a zip file nor a plain dex file";
  }

  std::unique_ptr<ZipArchive> zip_archive(
      ZipArchive::OpenFromOwnedFd(dex_file->Fd(), dex_path.c_str(), &error_msg));
  if (zip_archive == nullptr) {
    return Error() << error_msg;
  }
  constexpr const char* kEmbeddedProfileEntry = "assets/art-profile/baseline.prof";
  std::unique_ptr<ZipEntry> zip_entry(zip_archive->FindOrNull(kEmbeddedProfileEntry, &error_msg));
  size_t size;
  if (zip_entry == nullptr || (size = zip_entry->GetUncompressedLength()) == 0) {
    if (!error_msg.empty()) {
      LOG(WARNING) << error_msg;
    }
    // The dex file doesn't necessarily contain a profile. This is expected.
    return File();
  }

  // The name is for debugging only.
  std::string memfd_name =
      ART_FORMAT("{} extracted in memory from {}", kEmbeddedProfileEntry, dex_path);
  File memfd(memfd_create(memfd_name.c_str(), /*flags=*/0),
             memfd_name,
             /*check_usage=*/false);
  if (!memfd.IsValid()) {
    return ErrnoError() << "Failed to create memfd";
  }
  if (ftruncate(memfd.Fd(), size) != 0) {
    return ErrnoError() << "Failed to ftruncate memfd";
  }
  // Map with MAP_SHARED because we're feeding the fd to profman.
  MemMap mem_map = MemMap::MapFile(size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   memfd.Fd(),
                                   /*start=*/0,
                                   /*low_4gb=*/false,
                                   memfd_name.c_str(),
                                   &error_msg);
  if (!mem_map.IsValid()) {
    return Errorf("Failed to mmap memfd: {}", error_msg);
  }
  if (!zip_entry->ExtractToMemory(mem_map.Begin(), &error_msg)) {
    return Errorf("Failed to extract '{}': {}", kEmbeddedProfileEntry, error_msg);
  }

  // Reopen the memfd with readonly to make SELinux happy when the fd is passed to a child process
  // who doesn't have write permission. (b/303909581)
  std::string path = ART_FORMAT("/proc/self/fd/{}", memfd.Fd());
  // NOLINTNEXTLINE - O_CLOEXEC is omitted on purpose
  File memfd_readonly(open(path.c_str(), O_RDONLY),
                      memfd_name,
                      /*check_usage=*/false,
                      /*read_only_mode=*/true);
  if (!memfd_readonly.IsOpened()) {
    return ErrnoErrorf("Failed to open file '{}' ('{}')", path, memfd_name);
  }

  return memfd_readonly;
}

class FdLogger {
 public:
  void Add(const NewFile& file) { fd_mapping_.emplace_back(file.Fd(), file.TempPath()); }
  void Add(const File& file) { fd_mapping_.emplace_back(file.Fd(), file.GetPath()); }

  std::string GetFds() {
    std::vector<int> fds;
    fds.reserve(fd_mapping_.size());
    for (const auto& [fd, path] : fd_mapping_) {
      fds.push_back(fd);
    }
    return Join(fds, ':');
  }

 private:
  std::vector<std::pair<int, std::string>> fd_mapping_;

  friend std::ostream& operator<<(std::ostream& os, const FdLogger& fd_logger);
};

std::ostream& operator<<(std::ostream& os, const FdLogger& fd_logger) {
  for (const auto& [fd, path] : fd_logger.fd_mapping_) {
    os << fd << ":" << path << ' ';
  }
  return os;
}

}  // namespace

#define RETURN_FATAL_IF_PRE_REBOOT(options)                                 \
  if ((options).is_pre_reboot) {                                            \
    return Fatal("This method is not supported in Pre-reboot Dexopt mode"); \
  }

#define RETURN_FATAL_IF_NOT_PRE_REBOOT(options)                              \
  if (!(options).is_pre_reboot) {                                            \
    return Fatal("This method is only supported in Pre-reboot Dexopt mode"); \
  }

#define RETURN_FATAL_IF_ARG_IS_PRE_REBOOT_IMPL(expected, arg, log_name)                        \
  {                                                                                            \
    auto&& __return_fatal_tmp = PreRebootFlag(arg);                                            \
    if ((expected) != __return_fatal_tmp) {                                                    \
      return Fatal(ART_FORMAT("Expected flag 'isPreReboot' in argument '{}' to be {}, got {}", \
                              log_name,                                                        \
                              expected,                                                        \
                              __return_fatal_tmp));                                            \
    }                                                                                          \
  }

#define RETURN_FATAL_IF_PRE_REBOOT_MISMATCH(options, arg, log_name) \
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT_IMPL((options).is_pre_reboot, arg, log_name)

#define RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(arg, log_name) \
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT_IMPL(false, arg, log_name)

Result<void> Restorecon(
    const std::string& path,
    const std::optional<OutputArtifacts::PermissionSettings::SeContext>& se_context,
    bool recurse) {
  if (!kIsTargetAndroid) {
    return {};
  }

  unsigned int flags = recurse ? SELINUX_ANDROID_RESTORECON_RECURSE : 0;
  int res = 0;
  if (se_context.has_value()) {
    res = selinux_android_restorecon_pkgdir(
        path.c_str(), se_context->seInfo.c_str(), se_context->uid, flags);
  } else {
    res = selinux_android_restorecon(path.c_str(), flags);
  }
  if (res != 0) {
    return ErrnoErrorf("Failed to restorecon directory '{}'", path);
  }
  return {};
}

ScopedAStatus Artd::isAlive(bool* _aidl_return) {
  *_aidl_return = true;
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::deleteArtifacts(const ArtifactsPath& in_artifactsPath, int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_artifactsPath, "artifactsPath");

  RawArtifactsPath path = OR_RETURN_FATAL(BuildArtifactsPath(in_artifactsPath));

  *_aidl_return = 0;
  *_aidl_return += GetSizeAndDeleteFile(path.oat_path);
  *_aidl_return += GetSizeAndDeleteFile(path.vdex_path);
  *_aidl_return += GetSizeAndDeleteFile(path.art_path);

  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getDexoptStatus(const std::string& in_dexFile,
                                    const std::string& in_instructionSet,
                                    const std::optional<std::string>& in_classLoaderContext,
                                    GetDexoptStatusResult* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);

  Result<OatFileAssistantContext*> ofa_context = GetOatFileAssistantContext();
  if (!ofa_context.ok()) {
    return NonFatal("Failed to get runtime options: " + ofa_context.error().message());
  }

  std::unique_ptr<ClassLoaderContext> context;
  std::string error_msg;
  auto oat_file_assistant = OatFileAssistant::Create(in_dexFile,
                                                     in_instructionSet,
                                                     in_classLoaderContext,
                                                     /*load_executable=*/false,
                                                     /*only_load_trusted_executable=*/true,
                                                     ofa_context.value(),
                                                     &context,
                                                     &error_msg);
  if (oat_file_assistant == nullptr) {
    return NonFatal("Failed to create OatFileAssistant: " + error_msg);
  }

  std::string ignored_odex_status;
  OatFileAssistant::Location location;
  oat_file_assistant->GetOptimizationStatus(&_aidl_return->locationDebugString,
                                            &_aidl_return->compilerFilter,
                                            &_aidl_return->compilationReason,
                                            &ignored_odex_status,
                                            &location);
  _aidl_return->artifactsLocation = ArtifactsLocationToAidl(location);

  // We ignore odex_status because it is not meaningful. It can only be either "up-to-date",
  // "apk-more-recent", or "io-error-no-oat", which means it doesn't give us information in addition
  // to what we can learn from compiler_filter because compiler_filter will be the actual compiler
  // filter, "run-from-apk-fallback", and "run-from-apk" in those three cases respectively.
  DCHECK(ignored_odex_status == "up-to-date" || ignored_odex_status == "apk-more-recent" ||
         ignored_odex_status == "io-error-no-oat");

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::isProfileUsable(const ProfilePath& in_profile,
                                         const std::string& in_dexFile,
                                         bool* _aidl_return) {
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_profile, "profile");

  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));

  FdLogger fd_logger;

  CmdlineBuilder art_exec_args = OR_RETURN_FATAL(GetArtExecCmdlineBuilder());

  CmdlineBuilder args;
  args.Add(OR_RETURN_FATAL(GetProfman()));

  Result<std::unique_ptr<File>> profile = OpenFileForReading(profile_path);
  if (!profile.ok()) {
    if (profile.error().code() == ENOENT) {
      *_aidl_return = false;
      return ScopedAStatus::ok();
    }
    return NonFatal(
        ART_FORMAT("Failed to open profile '{}': {}", profile_path, profile.error().message()));
  }
  args.Add("--reference-profile-file-fd=%d", profile.value()->Fd());
  fd_logger.Add(*profile.value());

  std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(in_dexFile));
  args.Add("--apk-fd=%d", dex_file->Fd());
  fd_logger.Add(*dex_file);

  art_exec_args.Add("--keep-fds=%s", fd_logger.GetFds()).Add("--").Concat(std::move(args));

  LOG(INFO) << "Running profman: " << Join(art_exec_args.Get(), /*separator=*/" ")
            << "\nOpened FDs: " << fd_logger;

  Result<int> result = ExecAndReturnCode(art_exec_args.Get(), kShortTimeoutSec);
  if (!result.ok()) {
    return NonFatal("Failed to run profman: " + result.error().message());
  }

  LOG(INFO) << ART_FORMAT("profman returned code {}", result.value());

  if (result.value() != ProfmanResult::kSkipCompilationSmallDelta &&
      result.value() != ProfmanResult::kSkipCompilationEmptyProfiles) {
    return NonFatal(ART_FORMAT("profman returned an unexpected code: {}", result.value()));
  }

  *_aidl_return = result.value() == ProfmanResult::kSkipCompilationSmallDelta;
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::CopyAndRewriteProfileImpl(File src,
                                                   OutputProfile* dst_aidl,
                                                   const std::string& dex_path,
                                                   CopyAndRewriteProfileResult* aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT_MISMATCH(options_, *dst_aidl, "dst");
  std::string dst_path = OR_RETURN_FATAL(BuildFinalProfilePath(dst_aidl->profilePath));
  OR_RETURN_FATAL(ValidateDexPath(dex_path));

  FdLogger fd_logger;

  CmdlineBuilder art_exec_args = OR_RETURN_FATAL(GetArtExecCmdlineBuilder());

  CmdlineBuilder args;
  args.Add(OR_RETURN_FATAL(GetProfman())).Add("--copy-and-update-profile-key");

  args.Add("--profile-file-fd=%d", src.Fd());
  fd_logger.Add(src);

  std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(dex_path));
  args.Add("--apk-fd=%d", dex_file->Fd());
  fd_logger.Add(*dex_file);

  std::unique_ptr<NewFile> dst =
      OR_RETURN_NON_FATAL(NewFile::Create(dst_path, dst_aidl->fsPermission));
  args.Add("--reference-profile-file-fd=%d", dst->Fd());
  fd_logger.Add(*dst);

  art_exec_args.Add("--keep-fds=%s", fd_logger.GetFds()).Add("--").Concat(std::move(args));

  LOG(INFO) << "Running profman: " << Join(art_exec_args.Get(), /*separator=*/" ")
            << "\nOpened FDs: " << fd_logger;

  Result<int> result = ExecAndReturnCode(art_exec_args.Get(), kShortTimeoutSec);
  if (!result.ok()) {
    return NonFatal("Failed to run profman: " + result.error().message());
  }

  LOG(INFO) << ART_FORMAT("profman returned code {}", result.value());

  if (result.value() == ProfmanResult::kCopyAndUpdateNoMatch ||
      result.value() == ProfmanResult::kCopyAndUpdateErrorFailedToLoadProfile) {
    *aidl_return = AnalyzeCopyAndRewriteProfileFailure(
        &src, static_cast<ProfmanResult::CopyAndUpdateResult>(result.value()));
    return ScopedAStatus::ok();
  }

  if (result.value() != ProfmanResult::kCopyAndUpdateSuccess) {
    return NonFatal(ART_FORMAT("profman returned an unexpected code: {}", result.value()));
  }

  OR_RETURN_NON_FATAL(dst->Keep());
  aidl_return->status = CopyAndRewriteProfileResult::Status::SUCCESS;
  dst_aidl->profilePath.id = dst->TempId();
  dst_aidl->profilePath.tmpPath = dst->TempPath();
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::copyAndRewriteProfile(const ProfilePath& in_src,
                                               OutputProfile* in_dst,
                                               const std::string& in_dexFile,
                                               CopyAndRewriteProfileResult* _aidl_return) {
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_src, "src");

  std::string src_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_src));

  Result<std::unique_ptr<File>> src = OpenFileForReading(src_path);
  if (!src.ok()) {
    if (src.error().code() == ENOENT) {
      _aidl_return->status = CopyAndRewriteProfileResult::Status::NO_PROFILE;
      return ScopedAStatus::ok();
    }
    return NonFatal(
        ART_FORMAT("Failed to open src profile '{}': {}", src_path, src.error().message()));
  }

  return CopyAndRewriteProfileImpl(std::move(*src.value()), in_dst, in_dexFile, _aidl_return);
}

ndk::ScopedAStatus Artd::copyAndRewriteEmbeddedProfile(OutputProfile* in_dst,
                                                       const std::string& in_dexFile,
                                                       CopyAndRewriteProfileResult* _aidl_return) {
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));

  Result<File> src = ExtractEmbeddedProfileToFd(in_dexFile);
  if (!src.ok()) {
    return NonFatal(ART_FORMAT(
        "Failed to extract profile from dex file '{}': {}", in_dexFile, src.error().message()));
  }
  if (!src->IsValid()) {
    _aidl_return->status = CopyAndRewriteProfileResult::Status::NO_PROFILE;
    return ScopedAStatus::ok();
  }

  return CopyAndRewriteProfileImpl(std::move(src.value()), in_dst, in_dexFile, _aidl_return);
}

ndk::ScopedAStatus Artd::commitTmpProfile(const TmpProfilePath& in_profile) {
  RETURN_FATAL_IF_PRE_REBOOT_MISMATCH(options_, in_profile, "profile");
  std::string tmp_profile_path = OR_RETURN_FATAL(BuildTmpProfilePath(in_profile));
  std::string ref_profile_path = OR_RETURN_FATAL(BuildFinalProfilePath(in_profile));

  std::error_code ec;
  std::filesystem::rename(tmp_profile_path, ref_profile_path, ec);
  if (ec) {
    return NonFatal(ART_FORMAT(
        "Failed to move '{}' to '{}': {}", tmp_profile_path, ref_profile_path, ec.message()));
  }

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::deleteProfile(const ProfilePath& in_profile) {
  // `in_profile` can be either a Pre-reboot path or an ordinary one.
  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));
  DeleteFile(profile_path);

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getProfileVisibility(const ProfilePath& in_profile,
                                              FileVisibility* _aidl_return) {
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_profile, "profile");
  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));
  *_aidl_return = OR_RETURN_NON_FATAL(GetFileVisibility(profile_path));
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getArtifactsVisibility(const ArtifactsPath& in_artifactsPath,
                                                FileVisibility* _aidl_return) {
  // `in_artifactsPath` can be either a Pre-reboot path or an ordinary one.
  std::string oat_path = OR_RETURN_FATAL(BuildArtifactsPath(in_artifactsPath)).oat_path;
  *_aidl_return = OR_RETURN_NON_FATAL(GetFileVisibility(oat_path));
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getDexFileVisibility(const std::string& in_dexFile,
                                              FileVisibility* _aidl_return) {
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));
  *_aidl_return = OR_RETURN_NON_FATAL(GetFileVisibility(in_dexFile));
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getDmFileVisibility(const DexMetadataPath& in_dmFile,
                                             FileVisibility* _aidl_return) {
  std::string dm_path = OR_RETURN_FATAL(BuildDexMetadataPath(in_dmFile));
  *_aidl_return = OR_RETURN_NON_FATAL(GetFileVisibility(dm_path));
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::mergeProfiles(const std::vector<ProfilePath>& in_profiles,
                                       const std::optional<ProfilePath>& in_referenceProfile,
                                       OutputProfile* in_outputProfile,
                                       const std::vector<std::string>& in_dexFiles,
                                       const MergeProfileOptions& in_options,
                                       bool* _aidl_return) {
  std::vector<std::string> profile_paths;
  for (const ProfilePath& profile : in_profiles) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(profile, "profiles");
    std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(profile));
    if (profile.getTag() == ProfilePath::dexMetadataPath) {
      return Fatal(ART_FORMAT("Does not support DM file, got '{}'", profile_path));
    }
    profile_paths.push_back(std::move(profile_path));
  }

  RETURN_FATAL_IF_PRE_REBOOT_MISMATCH(options_, *in_outputProfile, "outputProfile");
  std::string output_profile_path =
      OR_RETURN_FATAL(BuildFinalProfilePath(in_outputProfile->profilePath));
  for (const std::string& dex_file : in_dexFiles) {
    OR_RETURN_FATAL(ValidateDexPath(dex_file));
  }
  if (in_options.forceMerge + in_options.dumpOnly + in_options.dumpClassesAndMethods > 1) {
    return Fatal("Only one of 'forceMerge', 'dumpOnly', and 'dumpClassesAndMethods' can be set");
  }

  FdLogger fd_logger;

  CmdlineBuilder art_exec_args = OR_RETURN_FATAL(GetArtExecCmdlineBuilder());

  CmdlineBuilder args;
  args.Add(OR_RETURN_FATAL(GetProfman()));

  std::vector<std::unique_ptr<File>> profile_files;
  for (const std::string& profile_path : profile_paths) {
    Result<std::unique_ptr<File>> profile_file = OpenFileForReading(profile_path);
    if (!profile_file.ok()) {
      if (profile_file.error().code() == ENOENT) {
        // Skip non-existing file.
        continue;
      }
      return NonFatal(ART_FORMAT(
          "Failed to open profile '{}': {}", profile_path, profile_file.error().message()));
    }
    args.Add("--profile-file-fd=%d", profile_file.value()->Fd());
    fd_logger.Add(*profile_file.value());
    profile_files.push_back(std::move(profile_file.value()));
  }

  if (profile_files.empty()) {
    LOG(INFO) << "Merge skipped because there are no existing profiles";
    *_aidl_return = false;
    return ScopedAStatus::ok();
  }

  std::unique_ptr<NewFile> output_profile_file =
      OR_RETURN_NON_FATAL(NewFile::Create(output_profile_path, in_outputProfile->fsPermission));

  if (in_referenceProfile.has_value()) {
    if (in_options.dumpOnly || in_options.dumpClassesAndMethods) {
      return Fatal(
          "Reference profile must not be set when 'dumpOnly' or 'dumpClassesAndMethods' is set");
    }
    // `in_referenceProfile` can be either a Pre-reboot profile or an ordinary one.
    std::string reference_profile_path =
        OR_RETURN_FATAL(BuildProfileOrDmPath(*in_referenceProfile));
    if (in_referenceProfile->getTag() == ProfilePath::dexMetadataPath) {
      return Fatal(ART_FORMAT("Does not support DM file, got '{}'", reference_profile_path));
    }
    OR_RETURN_NON_FATAL(CopyFile(reference_profile_path, *output_profile_file));
  }

  if (in_options.dumpOnly || in_options.dumpClassesAndMethods) {
    args.Add("--dump-output-to-fd=%d", output_profile_file->Fd());
  } else {
    // profman is ok with this being an empty file when in_referenceProfile isn't set.
    args.Add("--reference-profile-file-fd=%d", output_profile_file->Fd());
  }
  fd_logger.Add(*output_profile_file);

  std::vector<std::unique_ptr<File>> dex_files;
  for (const std::string& dex_path : in_dexFiles) {
    std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(dex_path));
    args.Add("--apk-fd=%d", dex_file->Fd());
    fd_logger.Add(*dex_file);
    dex_files.push_back(std::move(dex_file));
  }

  if (in_options.dumpOnly || in_options.dumpClassesAndMethods) {
    args.Add(in_options.dumpOnly ? "--dump-only" : "--dump-classes-and-methods");
  } else {
    args.AddIfNonEmpty("--min-new-classes-percent-change=%s",
                       props_->GetOrEmpty("dalvik.vm.bgdexopt.new-classes-percent"))
        .AddIfNonEmpty("--min-new-methods-percent-change=%s",
                       props_->GetOrEmpty("dalvik.vm.bgdexopt.new-methods-percent"))
        .AddIf(in_options.forceMerge, "--force-merge-and-analyze")
        .AddIf(in_options.forBootImage, "--boot-image-merge");
  }

  art_exec_args.Add("--keep-fds=%s", fd_logger.GetFds()).Add("--").Concat(std::move(args));

  LOG(INFO) << "Running profman: " << Join(art_exec_args.Get(), /*separator=*/" ")
            << "\nOpened FDs: " << fd_logger;

  Result<int> result = ExecAndReturnCode(art_exec_args.Get(), kShortTimeoutSec);
  if (!result.ok()) {
    return NonFatal("Failed to run profman: " + result.error().message());
  }

  LOG(INFO) << ART_FORMAT("profman returned code {}", result.value());

  if (result.value() == ProfmanResult::kSkipCompilationSmallDelta ||
      result.value() == ProfmanResult::kSkipCompilationEmptyProfiles) {
    *_aidl_return = false;
    return ScopedAStatus::ok();
  }

  ProfmanResult::ProcessingResult expected_result =
      (in_options.dumpOnly || in_options.dumpClassesAndMethods) ? ProfmanResult::kSuccess :
                                                                  ProfmanResult::kCompile;
  if (result.value() != expected_result) {
    return NonFatal(ART_FORMAT("profman returned an unexpected code: {}", result.value()));
  }

  OR_RETURN_NON_FATAL(output_profile_file->Keep());
  *_aidl_return = true;
  in_outputProfile->profilePath.id = output_profile_file->TempId();
  in_outputProfile->profilePath.tmpPath = output_profile_file->TempPath();
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getDexoptNeeded(const std::string& in_dexFile,
                                         const std::string& in_instructionSet,
                                         const std::optional<std::string>& in_classLoaderContext,
                                         const std::string& in_compilerFilter,
                                         int32_t in_dexoptTrigger,
                                         GetDexoptNeededResult* _aidl_return) {
  Result<OatFileAssistantContext*> ofa_context = GetOatFileAssistantContext();
  if (!ofa_context.ok()) {
    return NonFatal("Failed to get runtime options: " + ofa_context.error().message());
  }

  std::unique_ptr<ClassLoaderContext> context;
  std::string error_msg;
  auto oat_file_assistant = OatFileAssistant::Create(in_dexFile,
                                                     in_instructionSet,
                                                     in_classLoaderContext,
                                                     /*load_executable=*/false,
                                                     /*only_load_trusted_executable=*/true,
                                                     ofa_context.value(),
                                                     &context,
                                                     &error_msg);
  if (oat_file_assistant == nullptr) {
    return NonFatal("Failed to create OatFileAssistant: " + error_msg);
  }

  OatFileAssistant::DexOptStatus status;
  _aidl_return->isDexoptNeeded =
      oat_file_assistant->GetDexOptNeeded(OR_RETURN_FATAL(ParseCompilerFilter(in_compilerFilter)),
                                          DexOptTriggerFromAidl(in_dexoptTrigger),
                                          &status);
  _aidl_return->isVdexUsable = status.IsVdexUsable();
  _aidl_return->artifactsLocation = ArtifactsLocationToAidl(status.GetLocation());

  std::optional<bool> has_dex_files = oat_file_assistant->HasDexFiles(&error_msg);
  if (!has_dex_files.has_value()) {
    return NonFatal("Failed to open dex file: " + error_msg);
  }
  _aidl_return->hasDexCode = *has_dex_files;

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::maybeCreateSdc(const OutputSecureDexMetadataCompanion& in_outputSdc) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);

  if (in_outputSdc.permissionSettings.seContext.has_value()) {
    // SDM files are for primary dex files.
    return Fatal("'seContext' must be null");
  }

  std::string sdm_path = OR_RETURN_FATAL(BuildSdmPath(in_outputSdc.sdcPath));
  std::string sdc_path = OR_RETURN_FATAL(BuildSdcPath(in_outputSdc.sdcPath));

  Result<std::unique_ptr<File>> sdm_file = OpenFileForReading(sdm_path);
  if (!sdm_file.ok()) {
    if (sdm_file.error().code() == ENOENT) {
      // No SDM file found. That's typical.
      return ScopedAStatus::ok();
    }
    return NonFatal(sdm_file.error().message());
  }
  struct stat sdm_st = OR_RETURN_NON_FATAL(Fstat(*sdm_file.value()));

  std::string error_msg;
  std::unique_ptr<SdcReader> sdc_reader = SdcReader::Load(sdc_path, &error_msg);
  if (sdc_reader != nullptr && sdc_reader->GetSdmTimestampNs() == TimeSpecToNs(sdm_st.st_mtim)) {
    // Already has an SDC file for the SDM file.
    return ScopedAStatus::ok();
  }

  std::string oat_dir_path;  // For restorecon, can be empty if the artifacts are in dalvik-cache.
  if (!in_outputSdc.sdcPath.isInDalvikCache) {
    OR_RETURN_NON_FATAL(PrepareArtifactsDirs(in_outputSdc.sdcPath.dexPath,
                                             in_outputSdc.sdcPath.isa,
                                             in_outputSdc.permissionSettings.dirFsPermission,
                                             &oat_dir_path));

    // Unlike the two `restorecon_` calls in `dexopt`, we only need one restorecon here because SDM
    // files are for primary dex files, whose oat directory doesn't have an MLS label.
    OR_RETURN_NON_FATAL(restorecon_(oat_dir_path, /*se_context=*/std::nullopt, /*recurse=*/true));
  }

  OatFileAssistantContext* ofa_context = OR_RETURN_NON_FATAL(GetOatFileAssistantContext());

  std::unique_ptr<NewFile> sdc_file = OR_RETURN_NON_FATAL(
      NewFile::Create(sdc_path, in_outputSdc.permissionSettings.fileFsPermission));
  SdcWriter writer(File(DupCloexec(sdc_file->Fd()), sdc_file->TempPath(), /*check_usage=*/true));

  writer.SetSdmTimestampNs(TimeSpecToNs(sdm_st.st_mtim));
  writer.SetApexVersions(ofa_context->GetApexVersions());

  if (!writer.Save(&error_msg)) {
    return NonFatal(error_msg);
  }

  OR_RETURN_NON_FATAL(sdc_file->CommitOrAbandon());

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::dexopt(
    const OutputArtifacts& in_outputArtifacts,
    const std::string& in_dexFile,
    const std::string& in_instructionSet,
    const std::optional<std::string>& in_classLoaderContext,
    const std::string& in_compilerFilter,
    const std::optional<ProfilePath>& in_profile,
    const std::optional<VdexPath>& in_inputVdex,
    const std::optional<DexMetadataPath>& in_dmFile,
    PriorityClass in_priorityClass,
    const DexoptOptions& in_dexoptOptions,
    const std::shared_ptr<IArtdCancellationSignal>& in_cancellationSignal,
    ArtdDexoptResult* _aidl_return) {
  _aidl_return->cancelled = false;

  RETURN_FATAL_IF_PRE_REBOOT_MISMATCH(options_, in_outputArtifacts, "outputArtifacts");
  RawArtifactsPath artifacts_path =
      OR_RETURN_FATAL(BuildArtifactsPath(in_outputArtifacts.artifactsPath));
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));
  // `in_profile` can be either a Pre-reboot profile or an ordinary one.
  std::optional<std::string> profile_path =
      in_profile.has_value() ?
          std::make_optional(OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile.value()))) :
          std::nullopt;
  ArtdCancellationSignal* cancellation_signal =
      OR_RETURN_FATAL(ToArtdCancellationSignal(in_cancellationSignal.get()));

  std::unique_ptr<ClassLoaderContext> context = nullptr;
  if (in_classLoaderContext.has_value()) {
    context = ClassLoaderContext::Create(in_classLoaderContext.value());
    if (context == nullptr) {
      return Fatal(
          ART_FORMAT("Class loader context '{}' is invalid", in_classLoaderContext.value()));
    }
  }

  std::string oat_dir_path;  // For restorecon, can be empty if the artifacts are in dalvik-cache.
  if (!in_outputArtifacts.artifactsPath.isInDalvikCache) {
    OR_RETURN_NON_FATAL(PrepareArtifactsDirs(in_outputArtifacts.artifactsPath.dexPath,
                                             in_outputArtifacts.artifactsPath.isa,
                                             in_outputArtifacts.permissionSettings.dirFsPermission,
                                             &oat_dir_path));

    // First-round restorecon. artd doesn't have the permission to create files with the
    // `apk_data_file` label, so we need to restorecon the "oat" directory first so that files will
    // inherit `dalvikcache_data_file` rather than `apk_data_file`.
    OR_RETURN_NON_FATAL(restorecon_(
        oat_dir_path, in_outputArtifacts.permissionSettings.seContext, /*recurse=*/true));
  }

  FdLogger fd_logger;

  CmdlineBuilder art_exec_args = OR_RETURN_FATAL(GetArtExecCmdlineBuilder());

  CmdlineBuilder args;
  args.Add(OR_RETURN_FATAL(GetDex2Oat()));

  const FsPermission& fs_permission = in_outputArtifacts.permissionSettings.fileFsPermission;

  std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(in_dexFile));
  args.Add("--zip-fd=%d", dex_file->Fd()).Add("--zip-location=%s", in_dexFile);
  fd_logger.Add(*dex_file);
  struct stat dex_st = OR_RETURN_NON_FATAL(Fstat(*dex_file));
  if ((dex_st.st_mode & S_IROTH) == 0) {
    if (fs_permission.isOtherReadable) {
      return NonFatal(ART_FORMAT(
          "Outputs cannot be other-readable because the dex file '{}' is not other-readable",
          dex_file->GetPath()));
    }
    // Negative numbers mean no `chown`. 0 means root.
    // Note: this check is more strict than it needs to be. For example, it doesn't allow the
    // outputs to belong to a group that is a subset of the dex file's group. This is for
    // simplicity, and it's okay as we don't have to handle such complicated cases in practice.
    if ((fs_permission.uid > 0 && static_cast<uid_t>(fs_permission.uid) != dex_st.st_uid) ||
        (fs_permission.gid > 0 && static_cast<gid_t>(fs_permission.gid) != dex_st.st_uid &&
         static_cast<gid_t>(fs_permission.gid) != dex_st.st_gid)) {
      return NonFatal(ART_FORMAT(
          "Outputs' owner doesn't match the dex file '{}' (outputs: {}:{}, dex file: {}:{})",
          dex_file->GetPath(),
          fs_permission.uid,
          fs_permission.gid,
          dex_st.st_uid,
          dex_st.st_gid));
    }
  }

  std::unique_ptr<NewFile> oat_file =
      OR_RETURN_NON_FATAL(NewFile::Create(artifacts_path.oat_path, fs_permission));
  args.Add("--oat-fd=%d", oat_file->Fd()).Add("--oat-location=%s", artifacts_path.oat_path);
  fd_logger.Add(*oat_file);

  std::unique_ptr<NewFile> vdex_file =
      OR_RETURN_NON_FATAL(NewFile::Create(artifacts_path.vdex_path, fs_permission));
  args.Add("--output-vdex-fd=%d", vdex_file->Fd());
  fd_logger.Add(*vdex_file);

  std::vector<NewFile*> files_to_commit{oat_file.get(), vdex_file.get()};
  std::vector<std::string_view> files_to_delete;

  std::unique_ptr<NewFile> art_file = nullptr;
  if (in_dexoptOptions.generateAppImage) {
    art_file = OR_RETURN_NON_FATAL(NewFile::Create(artifacts_path.art_path, fs_permission));
    args.Add("--app-image-fd=%d", art_file->Fd());
    args.AddIfNonEmpty("--image-format=%s", props_->GetOrEmpty("dalvik.vm.appimageformat"));
    fd_logger.Add(*art_file);
    files_to_commit.push_back(art_file.get());
  } else {
    files_to_delete.push_back(artifacts_path.art_path);
  }

  std::unique_ptr<NewFile> swap_file = nullptr;
  if (ShouldCreateSwapFileForDexopt()) {
    std::string swap_file_path = ART_FORMAT("{}.swap", artifacts_path.oat_path);
    swap_file =
        OR_RETURN_NON_FATAL(NewFile::Create(swap_file_path, FsPermission{.uid = -1, .gid = -1}));
    args.Add("--swap-fd=%d", swap_file->Fd());
    fd_logger.Add(*swap_file);
  }

  std::vector<std::unique_ptr<File>> context_files;
  if (context != nullptr) {
    std::vector<std::string> flattened_context = context->FlattenDexPaths();
    std::string dex_dir = Dirname(in_dexFile);
    std::vector<int> context_fds;
    for (const std::string& context_element : flattened_context) {
      std::string context_path = std::filesystem::path(dex_dir).append(context_element);
      OR_RETURN_FATAL(ValidateDexPath(context_path));
      std::unique_ptr<File> context_file = OR_RETURN_NON_FATAL(OpenFileForReading(context_path));
      context_fds.push_back(context_file->Fd());
      fd_logger.Add(*context_file);
      context_files.push_back(std::move(context_file));
    }
    args.AddIfNonEmpty("--class-loader-context-fds=%s", Join(context_fds, /*separator=*/':'))
        .Add("--class-loader-context=%s", in_classLoaderContext.value())
        .Add("--classpath-dir=%s", dex_dir);
  }

  std::unique_ptr<File> input_vdex_file = nullptr;
  if (in_inputVdex.has_value()) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_inputVdex.value(), "inputVdex");
    std::string input_vdex_path = OR_RETURN_FATAL(BuildVdexPath(in_inputVdex.value()));
    input_vdex_file = OR_RETURN_NON_FATAL(OpenFileForReading(input_vdex_path));
    args.Add("--input-vdex-fd=%d", input_vdex_file->Fd());
    fd_logger.Add(*input_vdex_file);
  }

  std::unique_ptr<File> dm_file = nullptr;
  if (in_dmFile.has_value()) {
    std::string dm_path = OR_RETURN_FATAL(BuildDexMetadataPath(in_dmFile.value()));
    dm_file = OR_RETURN_NON_FATAL(OpenFileForReading(dm_path));
    args.Add("--dm-fd=%d", dm_file->Fd());
    fd_logger.Add(*dm_file);
  }

  std::unique_ptr<File> profile_file = nullptr;
  if (profile_path.has_value()) {
    profile_file = OR_RETURN_NON_FATAL(OpenFileForReading(profile_path.value()));
    args.Add("--profile-file-fd=%d", profile_file->Fd());
    fd_logger.Add(*profile_file);
    struct stat profile_st = OR_RETURN_NON_FATAL(Fstat(*profile_file));
    if (fs_permission.isOtherReadable && (profile_st.st_mode & S_IROTH) == 0) {
      return NonFatal(ART_FORMAT(
          "Outputs cannot be other-readable because the profile '{}' is not other-readable",
          profile_file->GetPath()));
    }
    // TODO(b/260228411): Check uid and gid.
  }

  // Second-round restorecon. Restorecon recursively after the output files are created, so that the
  // SELinux context is applied to all of them. The SELinux context of a file is mostly inherited
  // from the parent directory upon creation, but the MLS label is not inherited, so we need to
  // restorecon every file so that they have the right MLS label. If the files are in dalvik-cache,
  // there's no need to restorecon because they inherits the SELinux context of the dalvik-cache
  // directory and they don't need to have MLS labels.
  if (!in_outputArtifacts.artifactsPath.isInDalvikCache) {
    OR_RETURN_NON_FATAL(restorecon_(
        oat_dir_path, in_outputArtifacts.permissionSettings.seContext, /*recurse=*/true));
  }

  AddBootImageFlags(args);
  AddCompilerConfigFlags(in_instructionSet, in_compilerFilter, in_dexoptOptions, args);
  AddPerfConfigFlags(in_priorityClass, art_exec_args, args);

  // For being surfaced in crash reports on crashes.
  args.Add("--comments=%s", in_dexoptOptions.comments);

  art_exec_args.Add("--keep-fds=%s", fd_logger.GetFds()).Add("--").Concat(std::move(args));

  LOG(INFO) << "Running dex2oat: " << Join(art_exec_args.Get(), /*separator=*/" ")
            << "\nOpened FDs: " << fd_logger;

  ProcessStat stat;
  std::string error_msg;
  ExecResult result = exec_utils_->ExecAndReturnResult(art_exec_args.Get(),
                                                       kLongTimeoutSec,
                                                       cancellation_signal->CreateExecCallbacks(),
                                                       /*new_process_group=*/true,
                                                       &stat,
                                                       &error_msg);
  _aidl_return->wallTimeMs = stat.wall_time_ms;
  _aidl_return->cpuTimeMs = stat.cpu_time_ms;

  auto result_info = ART_FORMAT("[status={},exit_code={},signal={}]",
                                static_cast<int>(result.status),
                                result.exit_code,
                                result.signal);
  if (result.status != ExecResult::kExited) {
    if (cancellation_signal->IsCancelled()) {
      _aidl_return->cancelled = true;
      return ScopedAStatus::ok();
    }
    return NonFatal(ART_FORMAT("Failed to run dex2oat: {} {}", error_msg, result_info));
  }

  LOG(INFO) << ART_FORMAT("dex2oat returned code {}", result.exit_code);

  if (result.exit_code != 0) {
    return NonFatal(
        ART_FORMAT("dex2oat returned an unexpected code: {} {}", result.exit_code, result_info));
  }

  int64_t size_bytes = 0;
  int64_t size_before_bytes = 0;
  for (const NewFile* file : files_to_commit) {
    size_bytes += GetSize(file->TempPath()).value_or(0);
    size_before_bytes += GetSize(file->FinalPath()).value_or(0);
  }
  for (std::string_view path : files_to_delete) {
    size_before_bytes += GetSize(path).value_or(0);
  }
  OR_RETURN_NON_FATAL(NewFile::CommitAllOrAbandon(files_to_commit, files_to_delete));

  _aidl_return->sizeBytes = size_bytes;
  _aidl_return->sizeBeforeBytes = size_before_bytes;
  return ScopedAStatus::ok();
}

ScopedAStatus ArtdCancellationSignal::cancel() {
  std::lock_guard<std::mutex> lock(mu_);
  is_cancelled_ = true;
  for (pid_t pid : pids_) {
    // Kill the whole process group.
    int res = kill_(-pid, SIGKILL);
    DCHECK_EQ(res, 0);
  }
  return ScopedAStatus::ok();
}

ScopedAStatus ArtdCancellationSignal::getType(int64_t* _aidl_return) {
  *_aidl_return = reinterpret_cast<intptr_t>(kArtdCancellationSignalType);
  return ScopedAStatus::ok();
}

ExecCallbacks ArtdCancellationSignal::CreateExecCallbacks() {
  return {
      .on_start =
          [&](pid_t pid) {
            std::lock_guard<std::mutex> lock(mu_);
            pids_.insert(pid);
            // Handle cancellation signals sent before the process starts.
            if (is_cancelled_) {
              int res = kill_(-pid, SIGKILL);
              DCHECK_EQ(res, 0);
            }
          },
      .on_end =
          [&](pid_t pid) {
            std::lock_guard<std::mutex> lock(mu_);
            // The pid should no longer receive kill signals sent by `cancellation_signal`.
            pids_.erase(pid);
          },
  };
}

bool ArtdCancellationSignal::IsCancelled() {
  std::lock_guard<std::mutex> lock(mu_);
  return is_cancelled_;
}

ScopedAStatus Artd::createCancellationSignal(
    std::shared_ptr<IArtdCancellationSignal>* _aidl_return) {
  *_aidl_return = ndk::SharedRefBase::make<ArtdCancellationSignal>(kill_);
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::cleanup(
    const std::vector<ProfilePath>& in_profilesToKeep,
    const std::vector<ArtifactsPath>& in_artifactsToKeep,
    const std::vector<VdexPath>& in_vdexFilesToKeep,
    const std::vector<SecureDexMetadataWithCompanionPaths>& in_SdmSdcFilesToKeep,
    const std::vector<RuntimeArtifactsPath>& in_runtimeArtifactsToKeep,
    bool in_keepPreRebootStagedFiles,
    int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  std::unordered_set<std::string> files_to_keep;
  for (const ProfilePath& profile : in_profilesToKeep) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(profile, "profilesToKeep");
    files_to_keep.insert(OR_RETURN_FATAL(BuildProfileOrDmPath(profile)));
  }
  for (const ArtifactsPath& artifacts : in_artifactsToKeep) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(artifacts, "artifactsToKeep");
    RawArtifactsPath path = OR_RETURN_FATAL(BuildArtifactsPath(artifacts));
    files_to_keep.insert(std::move(path.oat_path));
    files_to_keep.insert(std::move(path.vdex_path));
    files_to_keep.insert(std::move(path.art_path));
  }
  for (const VdexPath& vdex : in_vdexFilesToKeep) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(vdex, "vdexFilesToKeep");
    files_to_keep.insert(OR_RETURN_FATAL(BuildVdexPath(vdex)));
  }
  for (const SecureDexMetadataWithCompanionPaths& sdm_sdc : in_SdmSdcFilesToKeep) {
    files_to_keep.insert(OR_RETURN_FATAL(BuildSdmPath(sdm_sdc)));
    files_to_keep.insert(OR_RETURN_FATAL(BuildSdcPath(sdm_sdc)));
  }
  std::string android_data = OR_RETURN_NON_FATAL(GetAndroidDataOrError());
  std::string android_expand = OR_RETURN_NON_FATAL(GetAndroidExpandOrError());
  for (const RuntimeArtifactsPath& runtime_image_path : in_runtimeArtifactsToKeep) {
    OR_RETURN_FATAL(ValidateRuntimeArtifactsPath(runtime_image_path));
    std::vector<std::string> files =
        ListRuntimeArtifactsFiles(android_data, android_expand, runtime_image_path);
    std::move(files.begin(), files.end(), std::inserter(files_to_keep, files_to_keep.end()));
  }
  *_aidl_return = 0;
  for (const std::string& file : ListManagedFiles(android_data, android_expand)) {
    if (files_to_keep.find(file) == files_to_keep.end() &&
        (!in_keepPreRebootStagedFiles || !IsPreRebootStagedFile(file))) {
      LOG(INFO) << ART_FORMAT("Cleaning up obsolete file '{}'", file);
      *_aidl_return += GetSizeAndDeleteFile(file);
    }
  }
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::cleanUpPreRebootStagedFiles() {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  std::string android_data = OR_RETURN_NON_FATAL(GetAndroidDataOrError());
  std::string android_expand = OR_RETURN_NON_FATAL(GetAndroidExpandOrError());
  for (const std::string& file : ListManagedFiles(android_data, android_expand)) {
    if (IsPreRebootStagedFile(file)) {
      LOG(INFO) << ART_FORMAT("Cleaning up obsolete Pre-reboot staged file '{}'", file);
      DeleteFile(file);
    }
  }
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::isInDalvikCache(const std::string& in_dexFile, bool* _aidl_return) {
  // The artifacts should be in the global dalvik-cache directory if:
  // (1). the dex file is on a system partition, even if the partition is remounted read-write,
  //      or
  // (2). the dex file is in any other readonly location. (At the time of writing, this only
  //      include Incremental FS.)
  //
  // We cannot rely on access(2) because:
  // - It doesn't take effective capabilities into account, from which artd gets root access
  //   to the filesystem.
  // - The `faccessat` variant with the `AT_EACCESS` flag, which takes effective capabilities
  //   into account, is not supported by bionic.

  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));

  std::vector<FstabEntry> entries = OR_RETURN_NON_FATAL(GetProcMountsAncestorsOfPath(in_dexFile));
  // The last one controls because `/proc/mounts` reflects the sequence of `mount`.
  for (auto it = entries.rbegin(); it != entries.rend(); it++) {
    if (it->fs_type == "overlay") {
      // Ignore the overlays created by `remount`.
      continue;
    }
    // We need to special-case Incremental FS since it is tagged as read-write while it's actually
    // not.
    *_aidl_return = (it->flags & MS_RDONLY) != 0 || it->fs_type == "incremental-fs";
    return ScopedAStatus::ok();
  }

  return NonFatal(ART_FORMAT("Fstab entries not found for '{}'", in_dexFile));
}

ScopedAStatus Artd::deleteSdmSdcFiles(const SecureDexMetadataWithCompanionPaths& in_SdmSdcPaths,
                                      int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);

  std::string sdm_path = OR_RETURN_FATAL(BuildSdmPath(in_SdmSdcPaths));
  std::string sdc_path = OR_RETURN_FATAL(BuildSdcPath(in_SdmSdcPaths));

  *_aidl_return = GetSizeAndDeleteFile(sdm_path) + GetSizeAndDeleteFile(sdc_path);
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::deleteRuntimeArtifacts(const RuntimeArtifactsPath& in_runtimeArtifactsPath,
                                           int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  OR_RETURN_FATAL(ValidateRuntimeArtifactsPath(in_runtimeArtifactsPath));
  *_aidl_return = 0;
  std::string android_data = OR_LOG_AND_RETURN_OK(GetAndroidDataOrError());
  std::string android_expand = OR_LOG_AND_RETURN_OK(GetAndroidExpandOrError());
  for (const std::string& file :
       ListRuntimeArtifactsFiles(android_data, android_expand, in_runtimeArtifactsPath)) {
    *_aidl_return += GetSizeAndDeleteFile(file);
  }
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getArtifactsSize(const ArtifactsPath& in_artifactsPath, int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_artifactsPath, "artifactsPath");
  RawArtifactsPath path = OR_RETURN_FATAL(BuildArtifactsPath(in_artifactsPath));
  *_aidl_return = 0;
  *_aidl_return += GetSize(path.oat_path).value_or(0);
  *_aidl_return += GetSize(path.vdex_path).value_or(0);
  *_aidl_return += GetSize(path.art_path).value_or(0);
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getVdexFileSize(const VdexPath& in_vdexPath, int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_vdexPath, "vdexPath");
  std::string vdex_path = OR_RETURN_FATAL(BuildVdexPath(in_vdexPath));
  *_aidl_return = GetSize(vdex_path).value_or(0);
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getSdmFileSize(const SecureDexMetadataWithCompanionPaths& in_sdmPath,
                                        int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  std::string sdm_path = OR_RETURN_FATAL(BuildSdmPath(in_sdmPath));
  *_aidl_return = GetSize(sdm_path).value_or(0);
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getRuntimeArtifactsSize(const RuntimeArtifactsPath& in_runtimeArtifactsPath,
                                            int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  OR_RETURN_FATAL(ValidateRuntimeArtifactsPath(in_runtimeArtifactsPath));
  *_aidl_return = 0;
  std::string android_data = OR_LOG_AND_RETURN_OK(GetAndroidDataOrError());
  std::string android_expand = OR_LOG_AND_RETURN_OK(GetAndroidExpandOrError());
  for (const std::string& file :
       ListRuntimeArtifactsFiles(android_data, android_expand, in_runtimeArtifactsPath)) {
    *_aidl_return += GetSize(file).value_or(0);
  }
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getProfileSize(const ProfilePath& in_profile, int64_t* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(in_profile, "profile");
  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));
  *_aidl_return = GetSize(profile_path).value_or(0);
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::initProfileSaveNotification(const PrimaryCurProfilePath& in_profilePath,
                                                int in_pid,
                                                std::shared_ptr<IArtdNotification>* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);

  std::string path = OR_RETURN_FATAL(BuildPrimaryCurProfilePath(in_profilePath));

  unique_fd inotify_fd(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
  if (inotify_fd < 0) {
    return NonFatal(ART_FORMAT("Failed to inotify_init1: {}", strerror(errno)));
  }

  // Watch the dir rather than the file itself because profiles are moved in rather than updated in
  // place.
  std::string dir = Dirname(path);
  int wd = inotify_add_watch(inotify_fd, dir.c_str(), IN_MOVED_TO);
  if (wd < 0) {
    return NonFatal(ART_FORMAT("Failed to inotify_add_watch '{}': {}", dir, strerror(errno)));
  }

  unique_fd pidfd = PidfdOpen(in_pid, /*flags=*/0);
  if (pidfd < 0) {
    if (errno == ESRCH) {
      // The process has gone now.
      LOG(INFO) << ART_FORMAT("Process exited without sending notification '{}'", path);
      *_aidl_return = ndk::SharedRefBase::make<ArtdNotification>();
      return ScopedAStatus::ok();
    }
    return NonFatal(ART_FORMAT("Failed to pidfd_open {}: {}", in_pid, strerror(errno)));
  }

  *_aidl_return = ndk::SharedRefBase::make<ArtdNotification>(
      poll_, path, std::move(inotify_fd), std::move(pidfd));
  return ScopedAStatus::ok();
}

ScopedAStatus ArtdNotification::wait(int in_timeoutMs, bool* _aidl_return) {
  auto cleanup = make_scope_guard([&, this] { CleanUp(); });

  if (!mu_.try_lock()) {
    return Fatal("`wait` can be called only once");
  }
  std::lock_guard<std::mutex> lock(mu_, std::adopt_lock);
  LOG(INFO) << ART_FORMAT("Waiting for notification '{}'", path_);

  if (is_called_) {
    return Fatal("`wait` can be called only once");
  }
  is_called_ = true;

  if (done_) {
    *_aidl_return = true;
    return ScopedAStatus::ok();
  }

  struct pollfd pollfds[2]{
      {.fd = inotify_fd_.get(), .events = POLLIN},
      {.fd = pidfd_.get(), .events = POLLIN},
  };

  constexpr size_t kBufSize = sizeof(struct inotify_event) + NAME_MAX + 1;
  std::unique_ptr<uint8_t[]> buf(new (std::align_val_t(alignof(struct inotify_event)))
                                     uint8_t[kBufSize]);
  std::string basename = Basename(path_);

  uint64_t start_time = MilliTime();
  int64_t remaining_time_ms = in_timeoutMs;
  while (remaining_time_ms > 0) {
    int ret = TEMP_FAILURE_RETRY(poll_(pollfds, arraysize(pollfds), remaining_time_ms));
    if (ret < 0) {
      return NonFatal(
          ART_FORMAT("Failed to poll to wait for notification '{}': {}", path_, strerror(errno)));
    }
    if (ret == 0) {
      // Timeout.
      break;
    }
    if ((pollfds[0].revents & POLLIN) != 0) {
      ssize_t len = TEMP_FAILURE_RETRY(read(inotify_fd_, buf.get(), kBufSize));
      if (len < 0) {
        return NonFatal(ART_FORMAT(
            "Failed to read inotify fd for notification '{}': {}", path_, strerror(errno)));
      }
      const struct inotify_event* event;
      for (uint8_t* ptr = buf.get(); ptr < buf.get() + len;
           ptr += sizeof(struct inotify_event) + event->len) {
        event = (const struct inotify_event*)ptr;
        if (event->len > 0 && event->name == basename) {
          LOG(INFO) << ART_FORMAT("Received notification '{}'", path_);
          *_aidl_return = true;
          return ScopedAStatus::ok();
        }
      }
      remaining_time_ms = in_timeoutMs - (MilliTime() - start_time);
      continue;
    }
    if ((pollfds[1].revents & POLLIN) != 0) {
      LOG(INFO) << ART_FORMAT("Process exited without sending notification '{}'", path_);
      *_aidl_return = true;
      return ScopedAStatus::ok();
    }
    LOG(FATAL) << "Unreachable code";
    UNREACHABLE();
  }

  LOG(INFO) << ART_FORMAT("Timed out while waiting for notification '{}'", path_);
  *_aidl_return = false;
  return ScopedAStatus::ok();
}

ArtdNotification::~ArtdNotification() { CleanUp(); }

void ArtdNotification::CleanUp() {
  std::lock_guard<std::mutex> lock(mu_);
  inotify_fd_.reset();
  pidfd_.reset();
}

ScopedAStatus Artd::commitPreRebootStagedFiles(const std::vector<ArtifactsPath>& in_artifacts,
                                               const std::vector<WritableProfilePath>& in_profiles,
                                               bool* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);

  std::vector<std::pair<std::string, std::string>> files_to_move;
  std::vector<std::string> files_to_remove;

  for (const ArtifactsPath& artifacts : in_artifacts) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(artifacts, "artifacts");

    ArtifactsPath pre_reboot_artifacts = artifacts;
    pre_reboot_artifacts.isPreReboot = true;

    auto src_artifacts = std::make_unique<RawArtifactsPath>(
        OR_RETURN_FATAL(BuildArtifactsPath(pre_reboot_artifacts)));
    auto dst_artifacts =
        std::make_unique<RawArtifactsPath>(OR_RETURN_FATAL(BuildArtifactsPath(artifacts)));

    if (OS::FileExists(src_artifacts->oat_path.c_str())) {
      files_to_move.emplace_back(src_artifacts->oat_path, dst_artifacts->oat_path);
      files_to_move.emplace_back(src_artifacts->vdex_path, dst_artifacts->vdex_path);
      if (OS::FileExists(src_artifacts->art_path.c_str())) {
        files_to_move.emplace_back(src_artifacts->art_path, dst_artifacts->art_path);
      } else {
        files_to_remove.push_back(dst_artifacts->art_path);
      }
    }
  }

  for (const WritableProfilePath& profile : in_profiles) {
    RETURN_FATAL_IF_ARG_IS_PRE_REBOOT(profile, "profiles");

    WritableProfilePath pre_reboot_profile = profile;
    PreRebootFlag(pre_reboot_profile) = true;

    auto src_profile = std::make_unique<std::string>(
        OR_RETURN_FATAL(BuildWritableProfilePath(pre_reboot_profile)));
    auto dst_profile =
        std::make_unique<std::string>(OR_RETURN_FATAL(BuildWritableProfilePath(profile)));

    if (OS::FileExists(src_profile->c_str())) {
      files_to_move.emplace_back(*src_profile, *dst_profile);
    }
  }

  OR_RETURN_NON_FATAL(MoveAllOrAbandon(files_to_move, files_to_remove));

  for (const auto& [src_path, dst_path] : files_to_move) {
    LOG(INFO) << ART_FORMAT("Committed Pre-reboot staged file '{}' to '{}'", src_path, dst_path);
  }

  *_aidl_return = !files_to_move.empty();
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::checkPreRebootSystemRequirements(const std::string& in_chrootDir,
                                                     bool* _aidl_return) {
  RETURN_FATAL_IF_PRE_REBOOT(options_);
  BuildSystemProperties new_props =
      OR_RETURN_NON_FATAL(BuildSystemProperties::Create(in_chrootDir + "/system/build.prop"));
  std::string old_release_str = props_->GetOrEmpty("ro.build.version.release");
  int old_release;
  if (!ParseInt(old_release_str, &old_release)) {
    return NonFatal(
        ART_FORMAT("Failed to read or parse old release number, got '{}'", old_release_str));
  }
  std::string new_release_str = new_props.GetOrEmpty("ro.build.version.release");
  int new_release;
  if (!ParseInt(new_release_str, &new_release)) {
    return NonFatal(
        ART_FORMAT("Failed to read or parse new release number, got '{}'", new_release_str));
  }
  if (new_release - old_release >= 2) {
    // When the release version difference is large, there is no particular technical reason why we
    // can't run Pre-reboot Dexopt, but we cannot test and support those cases.
    LOG(WARNING) << ART_FORMAT(
        "Pre-reboot Dexopt not supported due to large difference in release versions (old_release: "
        "{}, new_release: {})",
        old_release,
        new_release);
    *_aidl_return = false;
    return ScopedAStatus::ok();
  }

  *_aidl_return = true;
  return ScopedAStatus::ok();
}

Result<void> Artd::Start() {
  OR_RETURN(SetLogVerbosity());
  MemMap::Init();

  ScopedAStatus status = ScopedAStatus::fromStatus(AServiceManager_registerLazyService(
      this->asBinder().get(), options_.is_pre_reboot ? kPreRebootServiceName : kServiceName));
  if (!status.isOk()) {
    return Error() << status.getDescription();
  }

  ABinderProcess_startThreadPool();

  return {};
}

Result<OatFileAssistantContext*> Artd::GetOatFileAssistantContext() {
  std::lock_guard<std::mutex> lock(ofa_context_mu_);

  if (ofa_context_ == nullptr) {
    ofa_context_ = std::make_unique<OatFileAssistantContext>(
        std::make_unique<OatFileAssistantContext::RuntimeOptions>(
            OatFileAssistantContext::RuntimeOptions{
                .image_locations = *OR_RETURN(GetBootImageLocations()),
                .boot_class_path = *OR_RETURN(GetBootClassPath()),
                .boot_class_path_locations = *OR_RETURN(GetBootClassPath()),
                .deny_art_apex_data_files = DenyArtApexDataFiles(),
            }));
    std::string error_msg;
    if (!ofa_context_->FetchAll(&error_msg)) {
      return Error() << error_msg;
    }
  }

  return ofa_context_.get();
}

Result<const std::vector<std::string>*> Artd::GetBootImageLocations() {
  std::lock_guard<std::mutex> lock(cache_mu_);

  if (!cached_boot_image_locations_.has_value()) {
    std::string location_str;

    if (UseJitZygoteLocked()) {
      location_str = GetJitZygoteBootImageLocation();
    } else if (std::string value = GetUserDefinedBootImageLocationsLocked(); !value.empty()) {
      location_str = std::move(value);
    } else {
      std::string error_msg;
      std::string android_root = GetAndroidRootSafe(&error_msg);
      if (!error_msg.empty()) {
        return Errorf("Failed to get ANDROID_ROOT: {}", error_msg);
      }
      location_str = GetDefaultBootImageLocation(android_root, DenyArtApexDataFilesLocked());
    }

    cached_boot_image_locations_ = Split(location_str, ":");
  }

  return &cached_boot_image_locations_.value();
}

Result<const std::vector<std::string>*> Artd::GetBootClassPath() {
  std::lock_guard<std::mutex> lock(cache_mu_);

  if (!cached_boot_class_path_.has_value()) {
    const char* env_value = getenv("BOOTCLASSPATH");
    if (env_value == nullptr || strlen(env_value) == 0) {
      return Errorf("Failed to get environment variable 'BOOTCLASSPATH'");
    }
    cached_boot_class_path_ = Split(env_value, ":");
  }

  return &cached_boot_class_path_.value();
}

bool Artd::UseJitZygote() {
  std::lock_guard<std::mutex> lock(cache_mu_);
  return UseJitZygoteLocked();
}

bool Artd::UseJitZygoteLocked() {
  if (!cached_use_jit_zygote_.has_value()) {
    cached_use_jit_zygote_ =
        props_->GetBool("persist.device_config.runtime_native_boot.profilebootclasspath",
                        "dalvik.vm.profilebootclasspath",
                        /*default_value=*/false);
  }

  return cached_use_jit_zygote_.value();
}

const std::string& Artd::GetUserDefinedBootImageLocations() {
  std::lock_guard<std::mutex> lock(cache_mu_);
  return GetUserDefinedBootImageLocationsLocked();
}

const std::string& Artd::GetUserDefinedBootImageLocationsLocked() {
  if (!cached_user_defined_boot_image_locations_.has_value()) {
    cached_user_defined_boot_image_locations_ = props_->GetOrEmpty("dalvik.vm.boot-image");
  }

  return cached_user_defined_boot_image_locations_.value();
}

bool Artd::DenyArtApexDataFiles() {
  std::lock_guard<std::mutex> lock(cache_mu_);
  return DenyArtApexDataFilesLocked();
}

bool Artd::DenyArtApexDataFilesLocked() {
  if (!cached_deny_art_apex_data_files_.has_value()) {
    cached_deny_art_apex_data_files_ =
        !props_->GetBool("odsign.verification.success", /*default_value=*/false);
  }

  return cached_deny_art_apex_data_files_.value();
}

Result<std::string> Artd::GetProfman() { return BuildArtBinPath("profman"); }

Result<CmdlineBuilder> Artd::GetArtExecCmdlineBuilder() {
  std::string art_exec_path = OR_RETURN(BuildArtBinPath("art_exec"));
  if (options_.is_pre_reboot) {
    // "/mnt/compat_env" is prepared by dexopt_chroot_setup on Android V.
    std::string compat_art_exec_path = "/mnt/compat_env" + art_exec_path;
    if (OS::FileExists(compat_art_exec_path.c_str())) {
      art_exec_path = std::move(compat_art_exec_path);
    }
  }

  CmdlineBuilder args;
  args.Add(art_exec_path)
      .Add("--drop-capabilities")
      .AddIf(options_.is_pre_reboot, "--process-name-suffix=Pre-reboot Dexopt chroot");
  return args;
}

bool Artd::ShouldUseDex2Oat64() {
  return !props_->GetOrEmpty("ro.product.cpu.abilist64").empty() &&
         props_->GetBool("dalvik.vm.dex2oat64.enabled", /*default_value=*/false);
}

bool Artd::ShouldUseDebugBinaries() {
  return props_->GetOrEmpty("persist.sys.dalvik.vm.lib.2") == "libartd.so";
}

Result<std::string> Artd::GetDex2Oat() {
  std::string binary_name = ShouldUseDebugBinaries() ?
                                (ShouldUseDex2Oat64() ? "dex2oatd64" : "dex2oatd32") :
                                (ShouldUseDex2Oat64() ? "dex2oat64" : "dex2oat32");
  return BuildArtBinPath(binary_name);
}

bool Artd::ShouldCreateSwapFileForDexopt() {
  // Create a swap file by default. Dex2oat will decide whether to use it or not.
  return props_->GetBool("dalvik.vm.dex2oat-swap", /*default_value=*/true);
}

void Artd::AddBootImageFlags(/*out*/ CmdlineBuilder& args) {
  if (UseJitZygote()) {
    args.Add("--force-jit-zygote");
  } else {
    args.AddIfNonEmpty("--boot-image=%s", GetUserDefinedBootImageLocations());
  }
}

void Artd::AddCompilerConfigFlags(const std::string& instruction_set,
                                  const std::string& compiler_filter,
                                  const DexoptOptions& dexopt_options,
                                  /*out*/ CmdlineBuilder& args) {
  args.Add("--instruction-set=%s", instruction_set);
  std::string features_prop = ART_FORMAT("dalvik.vm.isa.{}.features", instruction_set);
  args.AddIfNonEmpty("--instruction-set-features=%s", props_->GetOrEmpty(features_prop));
  std::string variant_prop = ART_FORMAT("dalvik.vm.isa.{}.variant", instruction_set);
  args.AddIfNonEmpty("--instruction-set-variant=%s", props_->GetOrEmpty(variant_prop));

  args.Add("--compiler-filter=%s", compiler_filter)
      .Add("--compilation-reason=%s", dexopt_options.compilationReason);

  args.AddIfNonEmpty("--max-image-block-size=%s",
                     props_->GetOrEmpty("dalvik.vm.dex2oat-max-image-block-size"))
      .AddIfNonEmpty("--very-large-app-threshold=%s",
                     props_->GetOrEmpty("dalvik.vm.dex2oat-very-large"))
      .AddIfNonEmpty("--resolve-startup-const-strings=%s",
                     props_->GetOrEmpty("dalvik.vm.dex2oat-resolve-startup-strings"));

  args.AddIf(dexopt_options.debuggable, "--debuggable")
      .AddIf(props_->GetBool("debug.generate-debug-info", /*default_value=*/false),
             "--generate-debug-info")
      .AddIf(props_->GetBool("dalvik.vm.dex2oat-minidebuginfo", /*default_value=*/false),
             "--generate-mini-debug-info");

  args.AddRuntimeIf(DenyArtApexDataFiles(), "-Xdeny-art-apex-data-files")
      .AddRuntime("-Xtarget-sdk-version:%d", dexopt_options.targetSdkVersion)
      .AddRuntimeIf(dexopt_options.hiddenApiPolicyEnabled, "-Xhidden-api-policy:enabled");
}

void Artd::AddPerfConfigFlags(PriorityClass priority_class,
                              /*out*/ CmdlineBuilder& art_exec_args,
                              /*out*/ CmdlineBuilder& dex2oat_args) {
  // CPU set and number of threads.
  std::string default_cpu_set_prop = "dalvik.vm.dex2oat-cpu-set";
  std::string default_threads_prop = "dalvik.vm.dex2oat-threads";
  std::string cpu_set;
  std::string threads;
  if (priority_class >= PriorityClass::BOOT) {
    cpu_set = props_->GetOrEmpty("dalvik.vm.boot-dex2oat-cpu-set");
    threads = props_->GetOrEmpty("dalvik.vm.boot-dex2oat-threads");
  } else if (priority_class >= PriorityClass::INTERACTIVE_FAST) {
    cpu_set = props_->GetOrEmpty("dalvik.vm.restore-dex2oat-cpu-set", default_cpu_set_prop);
    threads = props_->GetOrEmpty("dalvik.vm.restore-dex2oat-threads", default_threads_prop);
  } else if (priority_class <= PriorityClass::BACKGROUND) {
    cpu_set = props_->GetOrEmpty("dalvik.vm.background-dex2oat-cpu-set", default_cpu_set_prop);
    threads = props_->GetOrEmpty("dalvik.vm.background-dex2oat-threads", default_threads_prop);
  } else {
    cpu_set = props_->GetOrEmpty(default_cpu_set_prop);
    threads = props_->GetOrEmpty(default_threads_prop);
  }
  dex2oat_args.AddIfNonEmpty("--cpu-set=%s", cpu_set).AddIfNonEmpty("-j%s", threads);

  if (priority_class < PriorityClass::BOOT) {
    art_exec_args
        .Add(priority_class <= PriorityClass::BACKGROUND ? "--set-task-profile=Dex2OatBackground" :
                                                           "--set-task-profile=Dex2OatBootComplete")
        .Add("--set-priority=background");
  }

  dex2oat_args.AddRuntimeIfNonEmpty("-Xms%s", props_->GetOrEmpty("dalvik.vm.dex2oat-Xms"))
      .AddRuntimeIfNonEmpty("-Xmx%s", props_->GetOrEmpty("dalvik.vm.dex2oat-Xmx"));

  // Enable compiling dex files in isolation on low ram devices.
  // It takes longer but reduces the memory footprint.
  dex2oat_args.AddIf(props_->GetBool("ro.config.low_ram", /*default_value=*/false),
                     "--compile-individually");

  for (const std::string& flag :
       Tokenize(props_->GetOrEmpty("dalvik.vm.dex2oat-flags"), /*delimiters=*/" ")) {
    dex2oat_args.AddIfNonEmpty("%s", flag);
  }
}

Result<int> Artd::ExecAndReturnCode(const std::vector<std::string>& args,
                                    int timeout_sec,
                                    const ExecCallbacks& callbacks,
                                    ProcessStat* stat) const {
  std::string error_msg;
  // Create a new process group so that we can kill the process subtree at once by killing the
  // process group.
  ExecResult result = exec_utils_->ExecAndReturnResult(
      args, timeout_sec, callbacks, /*new_process_group=*/true, stat, &error_msg);
  if (result.status != ExecResult::kExited) {
    return Error() << error_msg;
  }
  return result.exit_code;
}

Result<struct stat> Artd::Fstat(const File& file) const {
  struct stat st;
  if (fstat_(file.Fd(), &st) != 0) {
    return Errorf("Unable to fstat file '{}'", file.GetPath());
  }
  return st;
}

Result<void> Artd::BindMountNewDir(const std::string& source, const std::string& target) const {
  OR_RETURN(CreateDir(source));
  OR_RETURN(BindMount(source, target));
  OR_RETURN(restorecon_(target, /*se_context=*/std::nullopt, /*recurse=*/false));
  return {};
}

Result<void> Artd::BindMount(const std::string& source, const std::string& target) const {
  if (mount_(source.c_str(),
             target.c_str(),
             /*fs_type=*/nullptr,
             MS_BIND | MS_PRIVATE,
             /*data=*/nullptr) != 0) {
    return ErrnoErrorf("Failed to bind-mount '{}' at '{}'", source, target);
  }
  return {};
}

ScopedAStatus Artd::preRebootInit(
    const std::shared_ptr<IArtdCancellationSignal>& in_cancellationSignal, bool* _aidl_return) {
  RETURN_FATAL_IF_NOT_PRE_REBOOT(options_);

  std::string tmp_dir = pre_reboot_tmp_dir_.value_or(kDefaultPreRebootTmpDir);
  std::string preparation_done_file = tmp_dir + "/preparation_done";
  std::string classpath_file = tmp_dir + "/classpath.txt";
  std::string art_apex_data_dir = tmp_dir + "/art_apex_data";
  std::string odrefresh_dir = tmp_dir + "/odrefresh";

  bool preparation_done = OS::FileExists(preparation_done_file.c_str());

  if (!preparation_done) {
    std::error_code ec;
    bool is_empty = std::filesystem::is_empty(tmp_dir, ec);
    if (ec) {
      return NonFatal(ART_FORMAT("Failed to check dir '{}': {}", tmp_dir, ec.message()));
    }
    if (!is_empty) {
      return Fatal(
          "preRebootInit must not be concurrently called or retried after cancellation or failure");
    }
  }

  OR_RETURN_NON_FATAL(PreRebootInitClearEnvs());
  OR_RETURN_NON_FATAL(
      PreRebootInitSetEnvFromFile(init_environ_rc_path_.value_or("/init.environ.rc")));
  if (!preparation_done) {
    OR_RETURN_NON_FATAL(PreRebootInitDeriveClasspath(classpath_file));
  }
  OR_RETURN_NON_FATAL(PreRebootInitSetEnvFromFile(classpath_file));
  if (!preparation_done) {
    OR_RETURN_NON_FATAL(BindMountNewDir(art_apex_data_dir, GetArtApexData()));
    OR_RETURN_NON_FATAL(BindMountNewDir(odrefresh_dir, "/data/misc/odrefresh"));
    ArtdCancellationSignal* cancellation_signal =
        OR_RETURN_FATAL(ToArtdCancellationSignal(in_cancellationSignal.get()));
    if (!OR_RETURN_NON_FATAL(PreRebootInitBootImages(cancellation_signal))) {
      *_aidl_return = false;
      return ScopedAStatus::ok();
    }
  }

  if (!preparation_done) {
    if (!WriteStringToFile(/*content=*/"", preparation_done_file)) {
      return NonFatal(
          ART_FORMAT("Failed to write '{}': {}", preparation_done_file, strerror(errno)));
    }
  }

  *_aidl_return = true;
  return ScopedAStatus::ok();
}

Result<void> Artd::PreRebootInitClearEnvs() {
  if (clearenv() != 0) {
    return ErrnoErrorf("Failed to clear environment variables");
  }
  return {};
}

Result<void> Artd::PreRebootInitSetEnvFromFile(const std::string& path) {
  std::regex export_line_pattern("\\s*export\\s+(.+?)\\s+(.+)");

  std::string content;
  if (!ReadFileToString(path, &content)) {
    return ErrnoErrorf("Failed to read '{}'", path);
  }
  bool found = false;
  for (const std::string& line : Split(content, "\n")) {
    if (line.find_first_of("\\\"") != std::string::npos) {
      return Errorf("Backslashes and quotes in env var file are not supported for now, got '{}'",
                    line);
    }
    std::smatch match;
    if (!std::regex_match(line, match, export_line_pattern)) {
      continue;
    }
    const std::string& key = match[1].str();
    const std::string& value = match[2].str();
    LOG(INFO) << ART_FORMAT("Setting environment variable '{}' to '{}'", key, value);
    if (setenv(key.c_str(), value.c_str(), /*replace=*/1) != 0) {
      return ErrnoErrorf("Failed to set environment variable '{}' to '{}'", key, value);
    }
    found = true;
  }
  if (!found) {
    return Errorf("Malformed env var file '{}': {}", path, content);
  }
  return {};
}

Result<void> Artd::PreRebootInitDeriveClasspath(const std::string& path) {
  std::unique_ptr<File> output(OS::CreateEmptyFile(path.c_str()));
  if (output == nullptr) {
    return ErrnoErrorf("Failed to create '{}'", path);
  }

  if (pre_reboot_build_props_ == nullptr) {
    pre_reboot_build_props_ = std::make_unique<BuildSystemProperties>(
        OR_RETURN(BuildSystemProperties::Create("/system/build.prop")));
  }
  std::string sdk_version = pre_reboot_build_props_->GetOrEmpty("ro.build.version.sdk");
  std::string codename = pre_reboot_build_props_->GetOrEmpty("ro.build.version.codename");
  std::string known_codenames =
      pre_reboot_build_props_->GetOrEmpty("ro.build.version.known_codenames");
  if (sdk_version.empty() || codename.empty() || known_codenames.empty()) {
    return Errorf("Failed to read system properties");
  }

  CmdlineBuilder args = OR_RETURN(GetArtExecCmdlineBuilder());
  args.Add("--keep-fds=%d", output->Fd())
      .Add("--")
      .Add("/apex/com.android.sdkext/bin/derive_classpath")
      .Add("--override-device-sdk-version=%s", sdk_version)
      .Add("--override-device-codename=%s", codename)
      .Add("--override-device-known-codenames=%s", known_codenames)
      .Add("/proc/self/fd/%d", output->Fd());

  LOG(INFO) << "Running derive_classpath: " << Join(args.Get(), /*separator=*/" ");

  Result<int> result = ExecAndReturnCode(args.Get(), kShortTimeoutSec);
  if (!result.ok()) {
    return Errorf("Failed to run derive_classpath: {}", result.error().message());
  }

  LOG(INFO) << ART_FORMAT("derive_classpath returned code {}", result.value());

  if (result.value() != 0) {
    return Errorf("derive_classpath returned an unexpected code: {}", result.value());
  }

  if (output->FlushClose() != 0) {
    return ErrnoErrorf("Failed to flush and close '{}'", path);
  }

  return {};
}

Result<bool> Artd::PreRebootInitBootImages(ArtdCancellationSignal* cancellation_signal) {
  CmdlineBuilder args = OR_RETURN(GetArtExecCmdlineBuilder());
  args.Add("--")
      .Add(OR_RETURN(BuildArtBinPath("odrefresh")))
      .Add("--only-boot-images")
      .Add("--compile");

  LOG(INFO) << "Running odrefresh: " << Join(args.Get(), /*separator=*/" ");

  Result<int> result =
      ExecAndReturnCode(args.Get(), kLongTimeoutSec, cancellation_signal->CreateExecCallbacks());
  if (!result.ok()) {
    if (cancellation_signal->IsCancelled()) {
      return false;
    }
    return Errorf("Failed to run odrefresh: {}", result.error().message());
  }

  LOG(INFO) << ART_FORMAT("odrefresh returned code {}", result.value());

  if (result.value() != odrefresh::ExitCode::kCompilationSuccess &&
      result.value() != odrefresh::ExitCode::kOkay) {
    return Errorf("odrefresh returned an unexpected code: {}", result.value());
  }

  return true;
}

ScopedAStatus Artd::validateDexPath(const std::string& in_dexFile,
                                    std::optional<std::string>* _aidl_return) {
  RETURN_FATAL_IF_NOT_PRE_REBOOT(options_);
  if (Result<void> result = ValidateDexPath(in_dexFile); !result.ok()) {
    *_aidl_return = result.error().message();
  } else {
    *_aidl_return = std::nullopt;
  }
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::validateClassLoaderContext(const std::string& in_dexFile,
                                               const std::string& in_classLoaderContext,
                                               std::optional<std::string>* _aidl_return) {
  RETURN_FATAL_IF_NOT_PRE_REBOOT(options_);
  if (Result<void> result = ValidateClassLoaderContext(in_dexFile, in_classLoaderContext);
      !result.ok()) {
    *_aidl_return = result.error().message();
  } else {
    *_aidl_return = std::nullopt;
  }
  return ScopedAStatus::ok();
}

Result<BuildSystemProperties> BuildSystemProperties::Create(const std::string& filename) {
  std::string content;
  if (!ReadFileToString(filename, &content)) {
    return ErrnoErrorf("Failed to read '{}'", filename);
  }
  std::regex import_pattern(R"re(import\s.*)re");
  std::unordered_map<std::string, std::string> system_properties;
  for (const std::string& raw_line : Split(content, "\n")) {
    std::string line = Trim(raw_line);
    if (line.empty() || line.starts_with('#') || std::regex_match(line, import_pattern)) {
      continue;
    }
    size_t pos = line.find('=');
    if (pos == std::string::npos || pos == 0 || (pos == 1 && line[1] == '?')) {
      return Errorf("Malformed system property line '{}' in file '{}'", line, filename);
    }
    if (line[pos - 1] == '?') {
      std::string key = line.substr(/*pos=*/0, /*n=*/pos - 1);
      if (system_properties.find(key) == system_properties.end()) {
        system_properties[key] = line.substr(pos + 1);
      }
    } else {
      system_properties[line.substr(/*pos=*/0, /*n=*/pos)] = line.substr(pos + 1);
    }
  }
  return BuildSystemProperties(std::move(system_properties));
}

std::string BuildSystemProperties::GetProperty(const std::string& key) const {
  auto it = system_properties_.find(key);
  return it != system_properties_.end() ? it->second : "";
}

}  // namespace artd
}  // namespace art
