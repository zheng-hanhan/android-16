/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define LOG_TAG "nativeloader"

#include "nativeloader/native_loader.h"

#include <dlfcn.h>
#include <sys/types.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "android-base/file.h"
#include "android-base/macros.h"
#include <android-base/properties.h>
#include "android-base/strings.h"
#include "android-base/thread_annotations.h"
#include "base/macros.h"
#include "nativebridge/native_bridge.h"
#include "nativehelper/scoped_utf_chars.h"
#include "public_libraries.h"

#ifdef ART_TARGET_ANDROID
#include "android-modules-utils/sdk_level.h"
#include "android/api-level.h"
#include "library_namespaces.h"
#include "log/log.h"
#include "nativeloader/dlext_namespaces.h"
#endif

namespace android {

namespace {

#if defined(ART_TARGET_ANDROID)

using ::android::base::Result;
using ::android::nativeloader::LibraryNamespaces;

// NATIVELOADER_DEFAULT_NAMESPACE_LIBS is an environment variable that can be
// used to list extra libraries (separated by ":") that libnativeloader will
// load from the default namespace. The libraries must be listed without paths,
// and then LD_LIBRARY_PATH is typically set to the directories to load them
// from. The libraries will be available in all classloader namespaces, and also
// in the fallback namespace used when no classloader is given.
//
// kNativeloaderExtraLibs is the name of that fallback namespace.
//
// NATIVELOADER_DEFAULT_NAMESPACE_LIBS is intended to be used for testing only,
// and in particular in the ART run tests that are executed through dalvikvm in
// the APEX. In that case the default namespace links to the ART namespace
// (com_android_art) for all libraries, which means this can be used to load
// test libraries that depend on ART internal libraries.
//
// There's also code in art/dalvikvm.cc to add links from com_android_art back
// to the default namespace for NATIVELOADER_DEFAULT_NAMESPACE_LIBS, enabling
// access in the opposite direction as well. Useful e.g. to load ART plugins in
// NATIVELOADER_DEFAULT_NAMESPACE_LIBS.
constexpr const char* kNativeloaderExtraLibs = "nativeloader-extra-libs";

std::mutex g_namespaces_mutex;
LibraryNamespaces* g_namespaces GUARDED_BY(g_namespaces_mutex) = new LibraryNamespaces;
NativeLoaderNamespace* g_nativeloader_extra_libs_namespace GUARDED_BY(g_namespaces_mutex) = nullptr;

std::optional<NativeLoaderNamespace> FindApexNamespace(const char* caller_location) {
  std::optional<std::string> name = nativeloader::FindApexNamespaceName(caller_location);
  if (name.has_value()) {
    // Native Bridge is never used for APEXes.
    Result<NativeLoaderNamespace> ns =
        NativeLoaderNamespace::GetExportedNamespace(name.value(), /*is_bridged=*/false);
    LOG_ALWAYS_FATAL_IF(!ns.ok(),
                        "Error finding ns %s for APEX location %s: %s",
                        name.value().c_str(),
                        caller_location,
                        ns.error().message().c_str());
    return ns.value();
  }
  return std::nullopt;
}

Result<NativeLoaderNamespace> GetNamespaceForApiDomain(nativeloader::ApiDomain api_domain,
                                                       bool is_bridged) {
  switch (api_domain) {
    case nativeloader::API_DOMAIN_VENDOR:
      return NativeLoaderNamespace::GetExportedNamespace(nativeloader::kVendorNamespaceName,
                                                         is_bridged);
    case nativeloader::API_DOMAIN_PRODUCT:
      return NativeLoaderNamespace::GetExportedNamespace(nativeloader::kProductNamespaceName,
                                                         is_bridged);
    case nativeloader::API_DOMAIN_SYSTEM:
      return NativeLoaderNamespace::GetSystemNamespace(is_bridged);
    default:
      LOG_FATAL("Invalid API domain %d", api_domain);
      UNREACHABLE();
  }
}

Result<void> CreateNativeloaderDefaultNamespaceLibsLink(NativeLoaderNamespace& ns)
    REQUIRES(g_namespaces_mutex) {
  const char* links = getenv("NATIVELOADER_DEFAULT_NAMESPACE_LIBS");
  if (links == nullptr || *links == 0) {
    return {};
  }
  // Pass nullptr to Link() to create a link to the default namespace without
  // requiring it to be visible.
  return ns.Link(nullptr, links);
}

Result<NativeLoaderNamespace*> GetNativeloaderExtraLibsNamespace() REQUIRES(g_namespaces_mutex) {
  if (g_nativeloader_extra_libs_namespace != nullptr) {
    return g_nativeloader_extra_libs_namespace;
  }

  Result<NativeLoaderNamespace> ns =
      NativeLoaderNamespace::Create(kNativeloaderExtraLibs,
                                    /*search_paths=*/"",
                                    /*permitted_paths=*/"",
                                    /*parent=*/nullptr,
                                    /*is_shared=*/false,
                                    /*is_exempt_list_enabled=*/false,
                                    /*also_used_as_anonymous=*/false);
  if (!ns.ok()) {
    return ns.error();
  }
  g_nativeloader_extra_libs_namespace = new NativeLoaderNamespace(std::move(ns.value()));
  Result<void> linked =
      CreateNativeloaderDefaultNamespaceLibsLink(*g_nativeloader_extra_libs_namespace);
  if (!linked.ok()) {
    return linked.error();
  }
  return g_nativeloader_extra_libs_namespace;
}

// If the given path matches a library in NATIVELOADER_DEFAULT_NAMESPACE_LIBS
// then load it in the nativeloader-extra-libs namespace, otherwise return
// nullptr without error.
Result<void*> TryLoadNativeloaderExtraLib(const char* path) {
  const char* links = getenv("NATIVELOADER_DEFAULT_NAMESPACE_LIBS");
  if (links == nullptr || *links == 0) {
    return nullptr;
  }
  std::vector<std::string> lib_list = base::Split(links, ":");
  if (std::find(lib_list.begin(), lib_list.end(), path) == lib_list.end()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> guard(g_namespaces_mutex);
  Result<NativeLoaderNamespace*> ns = GetNativeloaderExtraLibsNamespace();
  if (!ns.ok()) {
    return ns.error();
  }

  Result<void*> res = ns.value()->Load(path);
  ALOGD("Load %s using ns %s from NATIVELOADER_DEFAULT_NAMESPACE_LIBS match: %s",
        path,
        ns.value()->name().c_str(),
        res.ok() ? "ok" : res.error().message().c_str());
  return res;
}

Result<NativeLoaderNamespace*> CreateClassLoaderNamespaceLocked(JNIEnv* env,
                                                                int32_t target_sdk_version,
                                                                jobject class_loader,
                                                                nativeloader::ApiDomain api_domain,
                                                                bool is_shared,
                                                                const std::string& dex_path,
                                                                jstring library_path_j,
                                                                jstring permitted_path_j,
                                                                jstring uses_library_list_j)
    REQUIRES(g_namespaces_mutex) {
  Result<NativeLoaderNamespace*> ns = g_namespaces->Create(env,
                                                           target_sdk_version,
                                                           class_loader,
                                                           api_domain,
                                                           is_shared,
                                                           dex_path,
                                                           library_path_j,
                                                           permitted_path_j,
                                                           uses_library_list_j);
  if (!ns.ok()) {
    return ns;
  }
  Result<void> linked = CreateNativeloaderDefaultNamespaceLibsLink(*ns.value());
  if (!linked.ok()) {
    return linked.error();
  }
  return ns;
}

#endif  // ART_TARGET_ANDROID

}  // namespace

void InitializeNativeLoader() {
#if defined(ART_TARGET_ANDROID)
  std::lock_guard<std::mutex> guard(g_namespaces_mutex);
  g_namespaces->Initialize();
#endif
}

void ResetNativeLoader() {
#if defined(ART_TARGET_ANDROID)
  std::lock_guard<std::mutex> guard(g_namespaces_mutex);
  g_namespaces->Reset();
  delete g_nativeloader_extra_libs_namespace;
  g_nativeloader_extra_libs_namespace = nullptr;
#endif
}

// dex_path_j may be a ':'-separated list of paths, e.g. when creating a shared
// library loader - cf. mCodePaths in android.content.pm.SharedLibraryInfo.
jstring CreateClassLoaderNamespace(JNIEnv* env,
                                   int32_t target_sdk_version,
                                   jobject class_loader,
                                   bool is_shared,
                                   jstring dex_path_j,
                                   jstring library_path_j,
                                   jstring permitted_path_j,
                                   jstring uses_library_list_j) {
#if defined(ART_TARGET_ANDROID)
  std::string dex_path;
  if (dex_path_j != nullptr) {
    ScopedUtfChars dex_path_chars(env, dex_path_j);
    dex_path = dex_path_chars.c_str();
  }

  Result<nativeloader::ApiDomain> api_domain = nativeloader::GetApiDomainFromPathList(dex_path);
  if (!api_domain.ok()) {
    return env->NewStringUTF(api_domain.error().message().c_str());
  }

  std::lock_guard<std::mutex> guard(g_namespaces_mutex);
  Result<NativeLoaderNamespace*> ns = CreateClassLoaderNamespaceLocked(env,
                                                                       target_sdk_version,
                                                                       class_loader,
                                                                       api_domain.value(),
                                                                       is_shared,
                                                                       dex_path,
                                                                       library_path_j,
                                                                       permitted_path_j,
                                                                       uses_library_list_j);
  if (!ns.ok()) {
    return env->NewStringUTF(ns.error().message().c_str());
  }

#else
  UNUSED(env,
         target_sdk_version,
         class_loader,
         is_shared,
         dex_path_j,
         library_path_j,
         permitted_path_j,
         uses_library_list_j);
#endif

  return nullptr;
}

#if defined(ART_TARGET_ANDROID)
static bool ShouldBypassLoadingForB349878424() {
  struct stat st;
  if (stat("/system/lib64/libsobridge.so", &st) != 0 &&
      stat("/system/lib64/libwalkstack.so", &st) != 0) {
    return false;
  }
  std::string property = android::base::GetProperty("ro.product.build.fingerprint", "");
  return android_get_device_api_level() == 33 &&
      (property.starts_with("Xiaomi") ||
       property.starts_with("Redmi") ||
       property.starts_with("POCO"));
}
#endif

void* OpenNativeLibrary(JNIEnv* env,
                        int32_t target_sdk_version,
                        const char* path,
                        jobject class_loader,
                        const char* caller_location,
                        jstring library_path_j,
                        bool* needs_native_bridge,
                        char** error_msg) {
#if defined(ART_TARGET_ANDROID)
  if (class_loader == nullptr) {
    // class_loader is null only for the boot class loader (see
    // IsBootClassLoader call in JavaVMExt::LoadNativeLibrary), i.e. the caller
    // is in the boot classpath.
    *needs_native_bridge = false;
    if (caller_location != nullptr) {
      std::optional<NativeLoaderNamespace> ns = FindApexNamespace(caller_location);
      if (ns.has_value()) {
        const android_dlextinfo dlextinfo = {
            .flags = ANDROID_DLEXT_USE_NAMESPACE,
            .library_namespace = ns.value().ToRawAndroidNamespace(),
        };
        void* handle = android_dlopen_ext(path, RTLD_NOW, &dlextinfo);
        char* dlerror_msg = handle == nullptr ? strdup(dlerror()) : nullptr;
        ALOGD("Load %s using APEX ns %s for caller %s: %s",
              path,
              ns.value().name().c_str(),
              caller_location,
              dlerror_msg == nullptr ? "ok" : dlerror_msg);
        if (dlerror_msg != nullptr) {
          *error_msg = dlerror_msg;
        }
        return handle;
      }
    }

    // Check if the library is in NATIVELOADER_DEFAULT_NAMESPACE_LIBS and should
    // be loaded from the kNativeloaderExtraLibs namespace.
    {
      Result<void*> handle = TryLoadNativeloaderExtraLib(path);
      if (!handle.ok()) {
        *error_msg = strdup(handle.error().message().c_str());
        return nullptr;
      }
      if (handle.value() != nullptr) {
        return handle.value();
      }
    }

    // Handle issue b/349878424.
    static bool bypass_loading_for_b349878424 = ShouldBypassLoadingForB349878424();

    if (bypass_loading_for_b349878424 &&
        (strcmp("libsobridge.so", path) == 0 || strcmp("libwalkstack.so", path) == 0)) {
      // Load a different library to pretend the loading was successful. This
      // allows the device to boot.
      ALOGD("Loading libbase.so instead of %s due to b/349878424", path);
      path = "libbase.so";
    }

    // Fall back to the system namespace. This happens for preloaded JNI
    // libraries in the zygote.
    void* handle = OpenSystemLibrary(path, RTLD_NOW);
    char* dlerror_msg = handle == nullptr ? strdup(dlerror()) : nullptr;
    ALOGD("Load %s using system ns (caller=%s): %s",
          path,
          caller_location == nullptr ? "<unknown>" : caller_location,
          dlerror_msg == nullptr ? "ok" : dlerror_msg);
    if (dlerror_msg != nullptr) {
      *error_msg = dlerror_msg;
    }
    return handle;
  }

  // If the caller is in any of the system image partitions and the library is
  // in the same partition then load it without regards to public library
  // restrictions. This is only done if the library is specified by an absolute
  // path, so we don't affect the lookup process for libraries specified by name
  // only.
  if (caller_location != nullptr &&
      // Apps in the partition may have their own native libraries which should
      // be loaded with the app's classloader namespace, so only do this for
      // libraries in the partition-wide lib(64) directories.
      nativeloader::IsPartitionNativeLibPath(path) &&
      // Don't do this if the system image is older than V, to avoid any compat
      // issues with apps and shared libs in them.
      android::modules::sdklevel::IsAtLeastV()) {
    nativeloader::ApiDomain caller_api_domain = nativeloader::GetApiDomainFromPath(caller_location);
    if (caller_api_domain != nativeloader::API_DOMAIN_DEFAULT) {
      nativeloader::ApiDomain library_api_domain = nativeloader::GetApiDomainFromPath(path);

      if (library_api_domain == caller_api_domain) {
        bool is_bridged = false;
        if (library_path_j != nullptr) {
          ScopedUtfChars library_path_utf_chars(env, library_path_j);
          if (library_path_utf_chars[0] != '\0') {
            is_bridged = NativeBridgeIsPathSupported(library_path_utf_chars.c_str());
          }
        }

        Result<NativeLoaderNamespace> ns = GetNamespaceForApiDomain(caller_api_domain, is_bridged);
        if (!ns.ok()) {
          ALOGD("Failed to find ns for caller %s in API domain %d to load %s (is_bridged=%b): %s",
                caller_location,
                caller_api_domain,
                path,
                is_bridged,
                ns.error().message().c_str());
          *error_msg = strdup(ns.error().message().c_str());
          return nullptr;
        }

        *needs_native_bridge = ns.value().IsBridged();
        Result<void*> handle = ns.value().Load(path);
        ALOGD("Load %s using ns %s for caller %s in same partition (is_bridged=%b): %s",
              path,
              ns.value().name().c_str(),
              caller_location,
              is_bridged,
              handle.ok() ? "ok" : handle.error().message().c_str());
        if (!handle.ok()) {
          *error_msg = strdup(handle.error().message().c_str());
          return nullptr;
        }
        return handle.value();
      }
    }
  }

  NativeLoaderNamespace* ns;
  const char* ns_descr;
  {
    std::lock_guard<std::mutex> guard(g_namespaces_mutex);

    ns = g_namespaces->FindNamespaceByClassLoader(env, class_loader);
    ns_descr = "class loader";

    if (ns == nullptr) {
      // This is the case where the classloader was not created by ApplicationLoaders
      // In this case we create an isolated not-shared namespace for it.
      const std::string empty_dex_path;
      Result<NativeLoaderNamespace*> res =
          CreateClassLoaderNamespaceLocked(env,
                                           target_sdk_version,
                                           class_loader,
                                           nativeloader::API_DOMAIN_DEFAULT,
                                           /*is_shared=*/false,
                                           empty_dex_path,
                                           library_path_j,
                                           /*permitted_path_j=*/nullptr,
                                           /*uses_library_list_j=*/nullptr);
      if (!res.ok()) {
        ALOGD("Failed to create isolated ns for %s (caller=%s)",
              path,
              caller_location == nullptr ? "<unknown>" : caller_location);
        *error_msg = strdup(res.error().message().c_str());
        return nullptr;
      }
      ns = res.value();
      ns_descr = "isolated";
    }
  }

  *needs_native_bridge = ns->IsBridged();
  Result<void*> handle = ns->Load(path);
  ALOGD("Load %s using %s ns %s (caller=%s): %s",
        path,
        ns_descr,
        ns->name().c_str(),
        caller_location == nullptr ? "<unknown>" : caller_location,
        handle.ok() ? "ok" : handle.error().message().c_str());
  if (!handle.ok()) {
    *error_msg = strdup(handle.error().message().c_str());
    return nullptr;
  }
  return handle.value();

#else   // !ART_TARGET_ANDROID
  UNUSED(env, target_sdk_version, class_loader, caller_location);

  // Do some best effort to emulate library-path support. It will not
  // work for dependencies.
  //
  // Note: null has a special meaning and must be preserved.
  std::string library_path;  // Empty string by default.
  if (library_path_j != nullptr && path != nullptr && path[0] != '/') {
    ScopedUtfChars library_path_utf_chars(env, library_path_j);
    library_path = library_path_utf_chars.c_str();
  }

  std::vector<std::string> library_paths = base::Split(library_path, ":");

  for (const std::string& lib_path : library_paths) {
    *needs_native_bridge = false;
    const char* path_arg;
    std::string complete_path;
    if (path == nullptr) {
      // Preserve null.
      path_arg = nullptr;
    } else {
      complete_path = lib_path;
      if (!complete_path.empty()) {
        complete_path.append("/");
      }
      complete_path.append(path);
      path_arg = complete_path.c_str();
    }
    void* handle = dlopen(path_arg, RTLD_NOW);
    if (handle != nullptr) {
      return handle;
    }
    if (NativeBridgeIsSupported(path_arg)) {
      *needs_native_bridge = true;
      handle = NativeBridgeLoadLibrary(path_arg, RTLD_NOW);
      if (handle != nullptr) {
        return handle;
      }
      *error_msg = strdup(NativeBridgeGetError());
    } else {
      *error_msg = strdup(dlerror());
    }
  }
  return nullptr;
#endif  // !ART_TARGET_ANDROID
}

bool CloseNativeLibrary(void* handle, const bool needs_native_bridge, char** error_msg) {
  bool success;
  if (needs_native_bridge) {
    success = (NativeBridgeUnloadLibrary(handle) == 0);
    if (!success) {
      *error_msg = strdup(NativeBridgeGetError());
    }
  } else {
    success = (dlclose(handle) == 0);
    if (!success) {
      *error_msg = strdup(dlerror());
    }
  }

  return success;
}

void NativeLoaderFreeErrorMessage(char* msg) {
  // The error messages get allocated through strdup, so we must call free on them.
  free(msg);
}

#if defined(ART_TARGET_ANDROID)
void* OpenNativeLibraryInNamespace(NativeLoaderNamespace* ns, const char* path,
                                   bool* needs_native_bridge, char** error_msg) {
  Result<void*> handle = ns->Load(path);
  if (!handle.ok() && error_msg != nullptr) {
    *error_msg = strdup(handle.error().message().c_str());
  }
  if (needs_native_bridge != nullptr) {
    *needs_native_bridge = ns->IsBridged();
  }
  return handle.ok() ? *handle : nullptr;
}

bool IsNamespaceNativeBridged(const struct NativeLoaderNamespace* ns) { return ns->IsBridged(); }

// native_bridge_namespaces are not supported for callers of this function.
// This function will return nullptr in the case when application is running
// on native bridge.
android_namespace_t* FindNamespaceByClassLoader(JNIEnv* env, jobject class_loader) {
  std::lock_guard<std::mutex> guard(g_namespaces_mutex);
  NativeLoaderNamespace* ns = g_namespaces->FindNamespaceByClassLoader(env, class_loader);
  if (ns != nullptr && !ns->IsBridged()) {
    return ns->ToRawAndroidNamespace();
  }
  return nullptr;
}

NativeLoaderNamespace* FindNativeLoaderNamespaceByClassLoader(JNIEnv* env, jobject class_loader) {
  std::lock_guard<std::mutex> guard(g_namespaces_mutex);
  return g_namespaces->FindNamespaceByClassLoader(env, class_loader);
}

void LinkNativeLoaderNamespaceToExportedNamespaceLibrary(struct NativeLoaderNamespace* ns,
                                                         const char* exported_ns_name,
                                                         const char* library_name,
                                                         char** error_msg) {
  Result<NativeLoaderNamespace> exported_ns =
      NativeLoaderNamespace::GetExportedNamespace(exported_ns_name, ns->IsBridged());
  if (!exported_ns.ok()) {
    *error_msg = strdup(exported_ns.error().message().c_str());
    return;
  }

  Result<void> linked = ns->Link(&exported_ns.value(), std::string(library_name));
  if (!linked.ok()) {
    *error_msg = strdup(linked.error().message().c_str());
  }
}

#endif  // ART_TARGET_ANDROID

}  // namespace android
