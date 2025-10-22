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
 */

#include <android-base/logging.h>
#include <fuzzbinder/libbinder_driver.h>

#include "VendorVoldNativeService.h"
#include "VoldNativeService.h"
#include "sehandle.h"

using ::android::fuzzService;
using ::android::sp;

struct selabel_handle* sehandle;

extern "C" int LLVMFuzzerInitialize(int argc, char argv) {
    sehandle = selinux_android_file_context_handle();
    if (!sehandle) {
        LOG(ERROR) << "Failed to get SELinux file contexts handle in voldFuzzer!";
        exit(1);
    }
    selinux_android_set_sehandle(sehandle);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // TODO(b/183141167): need to rewrite 'dump' to avoid SIGPIPE.
    signal(SIGPIPE, SIG_IGN);
    auto voldService = sp<android::vold::VoldNativeService>::make();
    auto voldVendorService = sp<android::vold::VendorVoldNativeService>::make();
    fuzzService({voldService, voldVendorService}, FuzzedDataProvider(data, size));
    return 0;
}