/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef ANDROID_VINTF_UTILS_H
#define ANDROID_VINTF_UTILS_H

#include <memory>
#include <mutex>

#include <utils/Errors.h>
#include <vintf/FileSystem.h>
#include <vintf/PropertyFetcher.h>
#include <vintf/RuntimeInfo.h>
#include <vintf/parse_xml.h>

// Equality for timespec. This should be in global namespace where timespec
// is defined.

inline bool operator==(const timespec& a, const timespec& b) noexcept {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

inline bool operator!=(const timespec& a, const timespec& b) noexcept {
    return !(a == b);
}

namespace android {
namespace vintf {
namespace details {

template <typename T>
status_t fetchAllInformation(const FileSystem* fileSystem, const std::string& path, T* outObject,
                             std::string* error) {
    if (outObject->fileName().empty()) {
        outObject->setFileName(path);
    } else {
        outObject->setFileName(outObject->fileName() + ":" + path);
    }

    std::string info;
    status_t result = fileSystem->fetch(path, &info, error);

    if (result != OK) {
        return result;
    }

    bool success = fromXml(outObject, info, error);
    if (!success) {
        if (error) {
            *error = "Illformed file: " + path + ": " + *error;
        }
        return BAD_VALUE;
    }
    return OK;
}

class PropertyFetcherImpl : public PropertyFetcher {
   public:
    virtual std::string getProperty(const std::string& key,
                                    const std::string& defaultValue = "") const;
    virtual uint64_t getUintProperty(const std::string& key, uint64_t defaultValue,
                                     uint64_t max = UINT64_MAX) const;
    virtual bool getBoolProperty(const std::string& key, bool defaultValue) const;
};

class PropertyFetcherNoOp : public PropertyFetcher {
   public:
    virtual std::string getProperty(const std::string& key,
                                    const std::string& defaultValue = "") const override;
    virtual uint64_t getUintProperty(const std::string& key, uint64_t defaultValue,
                                     uint64_t max = UINT64_MAX) const override;
    virtual bool getBoolProperty(const std::string& key, bool defaultValue) const override;
};

// Merge src into dst.
// postcondition (if successful): *src == empty.
template <typename T>
static bool mergeField(T* dst, T* src, const T& empty = T{}) {
    if (*dst == *src) {
        *src = empty;
        return true;  // no conflict
    }
    if (*src == empty) {
        return true;
    }
    if (*dst == empty) {
        *dst = std::move(*src);
        *src = empty;
        return true;
    }
    return false;
}

// Check legacy instances (i.e. <version> + <interface> + <instance>) can be
// converted into FqInstance because forEachInstance relies on FqInstance.
// If error and appendedError is not null, error message is appended to appendedError.
// Return the corresponding value in <fqname> (i.e. @ver::Interface/instance for
// HIDL, Interface/instance for AIDL, @ver[::Interface]/instance for native)
[[nodiscard]] std::optional<FqInstance> convertLegacyInstanceIntoFqInstance(
    const std::string& package, const Version& version, const std::string& interface,
    const std::string& instance, HalFormat format, std::string* appendedError);

bool isCoreHal(const std::string& halName);

}  // namespace details
}  // namespace vintf
}  // namespace android



#endif
