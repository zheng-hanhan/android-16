/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <stdint.h>
#include <array>
#include <string>

namespace android {
namespace vintf {

enum class ExclusiveTo : size_t {
    // Not exclusive to any particular execution environment and
    // is available to host processes on the device (given they have
    // the correct access permissions like sepolicy).
    EMPTY = 0,
    // Exclusive to processes inside virtual machines on devices (given
    // they have the correct access permissions).
    // Host processes do not have access to these services.
    VM,
};

static constexpr std::array<const char*, 2> gExclusiveToStrings = {{
    "",
    "virtual-machine",
}};

}  // namespace vintf
}  // namespace android
