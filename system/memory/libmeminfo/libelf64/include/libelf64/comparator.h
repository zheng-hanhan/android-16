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

#include <libelf64/elf64.h>

#include <elf.h>
#include <vector>

namespace android {
namespace elf64 {

// Class to compare ELF64 binaries (shared libraries, executables).
//
// This class provides methods to compare:
//
// - Executable header (Elf64_Ehdr)
// - Program headers (Elf64_Phdr)
// - Section contents
// - Section headers (Elf64_Shdr)
class Elf64Comparator {
  public:
    // Compares the ELF64 Executable Header.
    // Returns true if they are equal, otherwise false.
    static bool compare(const Elf64_Ehdr& ehdr1, const Elf64_Ehdr& ehdr2);

    // Compares the ELF64 Program (Segment) Headers.
    // Returns true if they are equal, otherwise false.
    static bool compare(const std::vector<Elf64_Phdr>& phdrs1,
                        const std::vector<Elf64_Phdr>& phdrs2);

    // Compares the ELF64 Section Headers.
    // Returns true if they are equal, otherwise false.
    static bool compare(const std::vector<Elf64_Shdr>& shdrs1,
                        const std::vector<Elf64_Shdr>& shdrs2);

    // Compares the ELF64 Section content.
    // Returns true if they are equal, otherwise false.
    static bool compare(const std::vector<Elf64_Sc>& sections1,
                        const std::vector<Elf64_Sc>& sections2);
};

}  // namespace elf64
}  // namespace android
