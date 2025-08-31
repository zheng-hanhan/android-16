/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef ART_ARTD_ARTD_H_
#define ART_ARTD_ARTD_H_

#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <csignal>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aidl/com/android/server/art/BnArtd.h"
#include "aidl/com/android/server/art/BnArtdCancellationSignal.h"
#include "aidl/com/android/server/art/BnArtdNotification.h"
#include "aidl/com/android/server/art/SecureDexMetadataWithCompanionPaths.h"
#include "android-base/result.h"
#include "android-base/thread_annotations.h"
#include "android-base/unique_fd.h"
#include "android/binder_auto_utils.h"
#include "base/os.h"
#include "base/pidfd.h"
#include "exec_utils.h"
#include "oat/oat_file_assistant_context.h"
#include "tools/cmdline_builder.h"
#include "tools/system_properties.h"

namespace art {
namespace artd {

// Define these function types instead of getting them from C headers because those from glibc C
// headers contain the unwanted `noexcept`.
using KillFn = int(pid_t, int);
using FstatFn = int(int, struct stat*);
using PollFn = int(struct pollfd*, nfds_t, int);
using MountFn = int(const char*, const char*, const char*, uint32_t, const void*);

android::base::Result<void> Restorecon(
    const std::string& path,
    const std::optional<
        aidl::com::android::server::art::OutputArtifacts::PermissionSettings::SeContext>&
        se_context,
    bool recurse);

struct Options {
  // If true, this artd instance is for Pre-reboot Dexopt. It runs in a chroot environment that is
  // set up by dexopt_chroot_setup.
  bool is_pre_reboot = false;
};

class ArtdCancellationSignal : public aidl::com::android::server::art::BnArtdCancellationSignal {
 public:
  explicit ArtdCancellationSignal(std::function<KillFn> kill_func) : kill_(std::move(kill_func)) {}

  ndk::ScopedAStatus cancel() override;

  ndk::ScopedAStatus getType(int64_t* _aidl_return) override;

  // Returns callbacks to be provided to `ExecUtils`, to register/unregister the process with this
  // cancellation signal.
  ExecCallbacks CreateExecCallbacks();

  bool IsCancelled();

 private:
  std::mutex mu_;
  // True if cancellation has been signaled.
  bool is_cancelled_ GUARDED_BY(mu_) = false;
  // The pids of currently running child processes that are bound to this signal.
  std::unordered_set<pid_t> pids_ GUARDED_BY(mu_);

  std::function<KillFn> kill_;
};

class ArtdNotification : public aidl::com::android::server::art::BnArtdNotification {
 public:
  ArtdNotification() : done_(true) {}
  ArtdNotification(std::function<PollFn> poll_func,
                   const std::string& path,
                   android::base::unique_fd&& inotify_fd,
                   android::base::unique_fd&& pidfd)
      : poll_(poll_func),
        path_(std::move(path)),
        inotify_fd_(std::move(inotify_fd)),
        pidfd_(std::move(pidfd)),
        done_(false) {}

  ndk::ScopedAStatus wait(int in_timeoutMs, bool* _aidl_return) EXCLUDES(mu_) override;

  virtual ~ArtdNotification();

 private:
  void CleanUp() EXCLUDES(mu_);

  const std::function<PollFn> poll_;

  std::mutex mu_;
  std::string path_ GUARDED_BY(mu_);
  android::base::unique_fd inotify_fd_ GUARDED_BY(mu_);
  android::base::unique_fd pidfd_ GUARDED_BY(mu_);
  bool done_ GUARDED_BY(mu_);
  bool is_called_ GUARDED_BY(mu_) = false;
};

class Artd : public aidl::com::android::server::art::BnArtd {
 public:
  explicit Artd(Options&& options,
                std::unique_ptr<art::tools::SystemProperties> props =
                    std::make_unique<art::tools::SystemProperties>(),
                std::unique_ptr<ExecUtils> exec_utils = std::make_unique<ExecUtils>(),
                std::function<KillFn> kill_func = kill,
                std::function<FstatFn> fstat_func = fstat,
                std::function<PollFn> poll_func = poll,
                std::function<MountFn> mount_func = mount,
                std::function<decltype(Restorecon)> restorecon_func = Restorecon,
                std::optional<std::string> pre_reboot_tmp_dir = std::nullopt,
                std::optional<std::string> init_environ_rc_path = std::nullopt,
                std::unique_ptr<art::tools::SystemProperties> pre_reboot_build_props = nullptr)
      : options_(std::move(options)),
        props_(std::move(props)),
        exec_utils_(std::move(exec_utils)),
        kill_(std::move(kill_func)),
        fstat_(std::move(fstat_func)),
        poll_(std::move(poll_func)),
        mount_(std::move(mount_func)),
        restorecon_(std::move(restorecon_func)),
        pre_reboot_tmp_dir_(std::move(pre_reboot_tmp_dir)),
        init_environ_rc_path_(std::move(init_environ_rc_path)),
        pre_reboot_build_props_(std::move(pre_reboot_build_props)) {}

  ndk::ScopedAStatus isAlive(bool* _aidl_return) override;

  ndk::ScopedAStatus deleteArtifacts(
      const aidl::com::android::server::art::ArtifactsPath& in_artifactsPath,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus getDexoptStatus(
      const std::string& in_dexFile,
      const std::string& in_instructionSet,
      const std::optional<std::string>& in_classLoaderContext,
      aidl::com::android::server::art::GetDexoptStatusResult* _aidl_return) override;

  ndk::ScopedAStatus isProfileUsable(const aidl::com::android::server::art::ProfilePath& in_profile,
                                     const std::string& in_dexFile,
                                     bool* _aidl_return) override;

  ndk::ScopedAStatus copyAndRewriteProfile(
      const aidl::com::android::server::art::ProfilePath& in_src,
      aidl::com::android::server::art::OutputProfile* in_dst,
      const std::string& in_dexFile,
      aidl::com::android::server::art::CopyAndRewriteProfileResult* _aidl_return) override;

  ndk::ScopedAStatus copyAndRewriteEmbeddedProfile(
      aidl::com::android::server::art::OutputProfile* in_dst,
      const std::string& in_dexFile,
      aidl::com::android::server::art::CopyAndRewriteProfileResult* _aidl_return) override;

  ndk::ScopedAStatus commitTmpProfile(
      const aidl::com::android::server::art::ProfilePath::TmpProfilePath& in_profile) override;

  ndk::ScopedAStatus deleteProfile(
      const aidl::com::android::server::art::ProfilePath& in_profile) override;

  ndk::ScopedAStatus getProfileVisibility(
      const aidl::com::android::server::art::ProfilePath& in_profile,
      aidl::com::android::server::art::FileVisibility* _aidl_return) override;

  ndk::ScopedAStatus mergeProfiles(
      const std::vector<aidl::com::android::server::art::ProfilePath>& in_profiles,
      const std::optional<aidl::com::android::server::art::ProfilePath>& in_referenceProfile,
      aidl::com::android::server::art::OutputProfile* in_outputProfile,
      const std::vector<std::string>& in_dexFiles,
      const aidl::com::android::server::art::MergeProfileOptions& in_options,
      bool* _aidl_return) override;

  ndk::ScopedAStatus getArtifactsVisibility(
      const aidl::com::android::server::art::ArtifactsPath& in_artifactsPath,
      aidl::com::android::server::art::FileVisibility* _aidl_return) override;

  ndk::ScopedAStatus getDexFileVisibility(
      const std::string& in_dexFile,
      aidl::com::android::server::art::FileVisibility* _aidl_return) override;

  ndk::ScopedAStatus getDmFileVisibility(
      const aidl::com::android::server::art::DexMetadataPath& in_dmFile,
      aidl::com::android::server::art::FileVisibility* _aidl_return) override;

  ndk::ScopedAStatus getDexoptNeeded(
      const std::string& in_dexFile,
      const std::string& in_instructionSet,
      const std::optional<std::string>& in_classLoaderContext,
      const std::string& in_compilerFilter,
      int32_t in_dexoptTrigger,
      aidl::com::android::server::art::GetDexoptNeededResult* _aidl_return) override;

  ndk::ScopedAStatus maybeCreateSdc(
      const aidl::com::android::server::art::OutputSecureDexMetadataCompanion& in_outputSdc)
      override;

  ndk::ScopedAStatus dexopt(
      const aidl::com::android::server::art::OutputArtifacts& in_outputArtifacts,
      const std::string& in_dexFile,
      const std::string& in_instructionSet,
      const std::optional<std::string>& in_classLoaderContext,
      const std::string& in_compilerFilter,
      const std::optional<aidl::com::android::server::art::ProfilePath>& in_profile,
      const std::optional<aidl::com::android::server::art::VdexPath>& in_inputVdex,
      const std::optional<aidl::com::android::server::art::DexMetadataPath>& in_dmFile,
      aidl::com::android::server::art::PriorityClass in_priorityClass,
      const aidl::com::android::server::art::DexoptOptions& in_dexoptOptions,
      const std::shared_ptr<aidl::com::android::server::art::IArtdCancellationSignal>&
          in_cancellationSignal,
      aidl::com::android::server::art::ArtdDexoptResult* _aidl_return) override;

  ndk::ScopedAStatus createCancellationSignal(
      std::shared_ptr<aidl::com::android::server::art::IArtdCancellationSignal>* _aidl_return)
      override;

  ndk::ScopedAStatus cleanup(
      const std::vector<aidl::com::android::server::art::ProfilePath>& in_profilesToKeep,
      const std::vector<aidl::com::android::server::art::ArtifactsPath>& in_artifactsToKeep,
      const std::vector<aidl::com::android::server::art::VdexPath>& in_vdexFilesToKeep,
      const std::vector<aidl::com::android::server::art::SecureDexMetadataWithCompanionPaths>&
          in_SdmSdcFilesToKeep,
      const std::vector<aidl::com::android::server::art::RuntimeArtifactsPath>&
          in_runtimeArtifactsToKeep,
      bool in_keepPreRebootStagedFiles,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus cleanUpPreRebootStagedFiles() override;

  ndk::ScopedAStatus isInDalvikCache(const std::string& in_dexFile, bool* _aidl_return) override;

  ndk::ScopedAStatus deleteSdmSdcFiles(
      const aidl::com::android::server::art::SecureDexMetadataWithCompanionPaths& in_sdmSdcPaths,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus deleteRuntimeArtifacts(
      const aidl::com::android::server::art::RuntimeArtifactsPath& in_runtimeArtifactsPath,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus getArtifactsSize(
      const aidl::com::android::server::art::ArtifactsPath& in_artifactsPath,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus getVdexFileSize(const aidl::com::android::server::art::VdexPath& in_vdexPath,
                                     int64_t* _aidl_return) override;

  ndk::ScopedAStatus getSdmFileSize(
      const aidl::com::android::server::art::SecureDexMetadataWithCompanionPaths& in_sdmPath,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus getRuntimeArtifactsSize(
      const aidl::com::android::server::art::RuntimeArtifactsPath& in_runtimeArtifactsPath,
      int64_t* _aidl_return) override;

  ndk::ScopedAStatus getProfileSize(const aidl::com::android::server::art::ProfilePath& in_profile,
                                    int64_t* _aidl_return) override;

  ndk::ScopedAStatus initProfileSaveNotification(
      const aidl::com::android::server::art::ProfilePath::PrimaryCurProfilePath& in_profilePath,
      int in_pid,
      std::shared_ptr<aidl::com::android::server::art::IArtdNotification>* _aidl_return) override;

  ndk::ScopedAStatus commitPreRebootStagedFiles(
      const std::vector<aidl::com::android::server::art::ArtifactsPath>& in_artifacts,
      const std::vector<aidl::com::android::server::art::ProfilePath::WritableProfilePath>&
          in_profiles,
      bool* _aidl_return) override;

  ndk::ScopedAStatus checkPreRebootSystemRequirements(const std::string& in_chrootDir,
                                                      bool* _aidl_return) override;

  ndk::ScopedAStatus preRebootInit(
      const std::shared_ptr<aidl::com::android::server::art::IArtdCancellationSignal>&
          in_cancellationSignal,
      bool* _aidl_return) override;

  ndk::ScopedAStatus validateDexPath(const std::string& in_dexFile,
                                     std::optional<std::string>* _aidl_return) override;

  ndk::ScopedAStatus validateClassLoaderContext(const std::string& in_dexFile,
                                                const std::string& in_classLoaderContext,
                                                std::optional<std::string>* _aidl_return) override;

  android::base::Result<void> Start();

 private:
  android::base::Result<OatFileAssistantContext*> GetOatFileAssistantContext()
      EXCLUDES(ofa_context_mu_);

  android::base::Result<const std::vector<std::string>*> GetBootImageLocations()
      EXCLUDES(cache_mu_);

  android::base::Result<const std::vector<std::string>*> GetBootClassPath() EXCLUDES(cache_mu_);

  bool UseJitZygote() EXCLUDES(cache_mu_);
  bool UseJitZygoteLocked() REQUIRES(cache_mu_);

  const std::string& GetUserDefinedBootImageLocations() EXCLUDES(cache_mu_);
  const std::string& GetUserDefinedBootImageLocationsLocked() REQUIRES(cache_mu_);

  bool DenyArtApexDataFiles() EXCLUDES(cache_mu_);
  bool DenyArtApexDataFilesLocked() REQUIRES(cache_mu_);

  android::base::Result<int> ExecAndReturnCode(const std::vector<std::string>& arg_vector,
                                               int timeout_sec,
                                               const ExecCallbacks& callbacks = ExecCallbacks(),
                                               ProcessStat* stat = nullptr) const;

  android::base::Result<std::string> GetProfman();

  android::base::Result<tools::CmdlineBuilder> GetArtExecCmdlineBuilder();

  bool ShouldUseDex2Oat64();

  bool ShouldUseDebugBinaries();

  android::base::Result<std::string> GetDex2Oat();

  bool ShouldCreateSwapFileForDexopt();

  void AddBootImageFlags(/*out*/ art::tools::CmdlineBuilder& args);

  void AddCompilerConfigFlags(const std::string& instruction_set,
                              const std::string& compiler_filter,
                              const aidl::com::android::server::art::DexoptOptions& dexopt_options,
                              /*out*/ art::tools::CmdlineBuilder& args);

  void AddPerfConfigFlags(aidl::com::android::server::art::PriorityClass priority_class,
                          /*out*/ art::tools::CmdlineBuilder& art_exec_args,
                          /*out*/ art::tools::CmdlineBuilder& args);

  android::base::Result<struct stat> Fstat(const art::File& file) const;

  // Creates a new dir at `source` and bind-mounts it at `target`.
  android::base::Result<void> BindMountNewDir(const std::string& source,
                                              const std::string& target) const;

  android::base::Result<void> BindMount(const std::string& source, const std::string& target) const;

  ndk::ScopedAStatus CopyAndRewriteProfileImpl(
      File src,
      aidl::com::android::server::art::OutputProfile* dst_aidl,
      const std::string& dex_path,
      aidl::com::android::server::art::CopyAndRewriteProfileResult* aidl_return);

  android::base::Result<void> PreRebootInitClearEnvs();
  android::base::Result<void> PreRebootInitSetEnvFromFile(const std::string& path);
  android::base::Result<void> PreRebootInitDeriveClasspath(const std::string& path);
  android::base::Result<bool> PreRebootInitBootImages(ArtdCancellationSignal* cancellation_signal);

  std::mutex cache_mu_;
  std::optional<std::vector<std::string>> cached_boot_image_locations_ GUARDED_BY(cache_mu_);
  std::optional<std::vector<std::string>> cached_boot_class_path_ GUARDED_BY(cache_mu_);
  std::optional<bool> cached_use_jit_zygote_ GUARDED_BY(cache_mu_);
  std::optional<std::string> cached_user_defined_boot_image_locations_ GUARDED_BY(cache_mu_);
  std::optional<bool> cached_deny_art_apex_data_files_ GUARDED_BY(cache_mu_);

  std::mutex ofa_context_mu_;
  std::unique_ptr<OatFileAssistantContext> ofa_context_ GUARDED_BY(ofa_context_mu_);

  const Options options_;
  const std::unique_ptr<art::tools::SystemProperties> props_;
  const std::unique_ptr<ExecUtils> exec_utils_;
  const std::function<KillFn> kill_;
  const std::function<FstatFn> fstat_;
  const std::function<PollFn> poll_;
  const std::function<MountFn> mount_;
  const std::function<decltype(Restorecon)> restorecon_;
  const std::optional<std::string> pre_reboot_tmp_dir_;
  const std::optional<std::string> init_environ_rc_path_;
  std::unique_ptr<art::tools::SystemProperties> pre_reboot_build_props_;
};

// A class for getting system properties from a `build.prop` file.
// Note that this class ignores import statements and only reads properties from the given file
// itself. To read properties from an imported file, insatiate this class with the imported file
// directly.
class BuildSystemProperties : public tools::SystemProperties {
 public:
  // Creates an instance and loads system properties from the `build.prop` file specified at the
  // given path.
  static android::base::Result<BuildSystemProperties> Create(const std::string& filename);

 protected:
  std::string GetProperty(const std::string& key) const override;

 private:
  explicit BuildSystemProperties(std::unordered_map<std::string, std::string>&& system_properties)
      : system_properties_(std::move(system_properties)) {}

  const std::unordered_map<std::string, std::string> system_properties_;
};

}  // namespace artd
}  // namespace art

#endif  // ART_ARTD_ARTD_H_
