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

#include <android-base/stringprintf.h>

#include "meminfo_private.h"

namespace android {
namespace meminfo {

bool ExtractAndroidHeapStats(int pid, AndroidHeapStats* stats, bool* foundSwapPss) {
    std::string smaps_path = base::StringPrintf("/proc/%d/smaps", pid);
    return ExtractAndroidHeapStatsFromFile(smaps_path, stats, foundSwapPss);
}

bool ExtractAndroidHeapStatsFromFile(const std::string& smaps_path, AndroidHeapStats* stats,
                                     bool* foundSwapPss) {
    *foundSwapPss = false;
    uint64_t prev_end = 0;
    int prev_heap = HEAP_UNKNOWN;

    auto vma_scan = [&](const Vma& vma) {
        int which_heap = HEAP_UNKNOWN;
        int sub_heap = HEAP_UNKNOWN;
        bool is_swappable = false;
        std::string name;
        if (vma.name.ends_with(" (deleted)")) {
            name = vma.name.substr(0, vma.name.size() - strlen(" (deleted)"));
        } else {
            name = vma.name;
        }

        uint32_t namesz = name.size();
        if (name.starts_with("[heap]")) {
            which_heap = HEAP_NATIVE;
        } else if (name.starts_with("[anon:libc_malloc]")) {
            which_heap = HEAP_NATIVE;
        } else if (name.starts_with("[anon:scudo:")) {
            which_heap = HEAP_NATIVE;
        } else if (name.starts_with("[anon:GWP-ASan")) {
            which_heap = HEAP_NATIVE;
        } else if (name.starts_with("[stack")) {
            which_heap = HEAP_STACK;
        } else if (name.starts_with("[anon:stack_and_tls:")) {
            which_heap = HEAP_STACK;
        } else if (name.ends_with(".so")) {
            which_heap = HEAP_SO;
            is_swappable = true;
        } else if (name.ends_with(".jar")) {
            which_heap = HEAP_JAR;
            is_swappable = true;
        } else if (name.ends_with(".apk")) {
            which_heap = HEAP_APK;
            is_swappable = true;
        } else if (name.ends_with(".ttf")) {
            which_heap = HEAP_TTF;
            is_swappable = true;
        } else if ((name.ends_with(".odex")) ||
                   (namesz > 4 && strstr(name.c_str(), ".dex") != nullptr)) {
            which_heap = HEAP_DEX;
            sub_heap = HEAP_DEX_APP_DEX;
            is_swappable = true;
        } else if (name.ends_with(".vdex")) {
            which_heap = HEAP_DEX;
            // Handle system@framework@boot and system/framework/boot|apex
            if ((strstr(name.c_str(), "@boot") != nullptr) ||
                (strstr(name.c_str(), "/boot") != nullptr) ||
                (strstr(name.c_str(), "/apex") != nullptr)) {
                sub_heap = HEAP_DEX_BOOT_VDEX;
            } else {
                sub_heap = HEAP_DEX_APP_VDEX;
            }
            is_swappable = true;
        } else if (name.ends_with(".oat")) {
            which_heap = HEAP_OAT;
            is_swappable = true;
        } else if (name.ends_with(".art") || name.ends_with(".art]")) {
            which_heap = HEAP_ART;
            // Handle system@framework@boot* and system/framework/boot|apex*
            if ((strstr(name.c_str(), "@boot") != nullptr) ||
                (strstr(name.c_str(), "/boot") != nullptr) ||
                (strstr(name.c_str(), "/apex") != nullptr)) {
                sub_heap = HEAP_ART_BOOT;
            } else {
                sub_heap = HEAP_ART_APP;
            }
            is_swappable = true;
        } else if (name.find("kgsl-3d0") != std::string::npos) {
            which_heap = HEAP_GL_DEV;
        } else if (name.starts_with("/dev/")) {
            which_heap = HEAP_UNKNOWN_DEV;
            if (name.starts_with("/dev/ashmem/CursorWindow")) {
                which_heap = HEAP_CURSOR;
            } else if (name.starts_with("/dev/ashmem/jit-zygote-cache")) {
                which_heap = HEAP_DALVIK_OTHER;
                sub_heap = HEAP_DALVIK_OTHER_ZYGOTE_CODE_CACHE;
            } else if (name.starts_with("/dev/ashmem")) {
                which_heap = HEAP_ASHMEM;
            }
        } else if (name.starts_with("/memfd:jit-cache")) {
            which_heap = HEAP_DALVIK_OTHER;
            sub_heap = HEAP_DALVIK_OTHER_APP_CODE_CACHE;
        } else if (name.starts_with("/memfd:jit-zygote-cache")) {
            which_heap = HEAP_DALVIK_OTHER;
            sub_heap = HEAP_DALVIK_OTHER_ZYGOTE_CODE_CACHE;
        } else if (name.starts_with("[anon:")) {
            which_heap = HEAP_UNKNOWN;
            if (name.starts_with("[anon:dalvik-")) {
                which_heap = HEAP_DALVIK_OTHER;
                if (name.starts_with("[anon:dalvik-LinearAlloc")) {
                    sub_heap = HEAP_DALVIK_OTHER_LINEARALLOC;
                } else if (name.starts_with("[anon:dalvik-alloc space") ||
                           name.starts_with("[anon:dalvik-main space")) {
                    // This is the regular Dalvik heap.
                    which_heap = HEAP_DALVIK;
                    sub_heap = HEAP_DALVIK_NORMAL;
                } else if (name.starts_with("[anon:dalvik-large object space") ||
                           name.starts_with("[anon:dalvik-free list large object space")) {
                    which_heap = HEAP_DALVIK;
                    sub_heap = HEAP_DALVIK_LARGE;
                } else if (name.starts_with("[anon:dalvik-non moving space")) {
                    which_heap = HEAP_DALVIK;
                    sub_heap = HEAP_DALVIK_NON_MOVING;
                } else if (name.starts_with("[anon:dalvik-zygote space")) {
                    which_heap = HEAP_DALVIK;
                    sub_heap = HEAP_DALVIK_ZYGOTE;
                } else if (name.starts_with("[anon:dalvik-indirect ref")) {
                    sub_heap = HEAP_DALVIK_OTHER_INDIRECT_REFERENCE_TABLE;
                } else if (name.starts_with("[anon:dalvik-jit-code-cache") ||
                           name.starts_with("[anon:dalvik-data-code-cache")) {
                    sub_heap = HEAP_DALVIK_OTHER_APP_CODE_CACHE;
                } else if (name.starts_with("[anon:dalvik-CompilerMetadata")) {
                    sub_heap = HEAP_DALVIK_OTHER_COMPILER_METADATA;
                } else {
                    sub_heap = HEAP_DALVIK_OTHER_ACCOUNTING;  // Default to accounting.
                }
            }
        } else if (namesz > 0) {
            which_heap = HEAP_UNKNOWN_MAP;
        } else if (vma.start == prev_end && prev_heap == HEAP_SO) {
            // bss section of a shared library
            which_heap = HEAP_SO;
        }

        prev_end = vma.end;
        prev_heap = which_heap;

        const MemUsage& usage = vma.usage;
        if (usage.swap_pss > 0 && !*foundSwapPss) {
            *foundSwapPss = true;
        }

        uint64_t swapable_pss = 0;
        if (is_swappable && (usage.pss > 0)) {
            float sharing_proportion = 0.0;
            if ((usage.shared_clean > 0) || (usage.shared_dirty > 0)) {
                sharing_proportion =
                        (usage.pss - usage.uss) / (usage.shared_clean + usage.shared_dirty);
            }
            swapable_pss = (sharing_proportion * usage.shared_clean) + usage.private_clean;
        }

        stats[which_heap].pss += usage.pss;
        stats[which_heap].swappablePss += swapable_pss;
        stats[which_heap].rss += usage.rss;
        stats[which_heap].privateDirty += usage.private_dirty;
        stats[which_heap].sharedDirty += usage.shared_dirty;
        stats[which_heap].privateClean += usage.private_clean;
        stats[which_heap].sharedClean += usage.shared_clean;
        stats[which_heap].swappedOut += usage.swap;
        stats[which_heap].swappedOutPss += usage.swap_pss;
        if (which_heap == HEAP_DALVIK || which_heap == HEAP_DALVIK_OTHER ||
            which_heap == HEAP_DEX || which_heap == HEAP_ART) {
            stats[sub_heap].pss += usage.pss;
            stats[sub_heap].swappablePss += swapable_pss;
            stats[sub_heap].rss += usage.rss;
            stats[sub_heap].privateDirty += usage.private_dirty;
            stats[sub_heap].sharedDirty += usage.shared_dirty;
            stats[sub_heap].privateClean += usage.private_clean;
            stats[sub_heap].sharedClean += usage.shared_clean;
            stats[sub_heap].swappedOut += usage.swap;
            stats[sub_heap].swappedOutPss += usage.swap_pss;
        }
        return true;
    };

    return ForEachVmaFromFile(smaps_path, vma_scan);
}
}  // namespace meminfo
}  // namespace android
