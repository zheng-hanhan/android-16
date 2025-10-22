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

#pragma once

#include <libelf64/elf64.h>

#include <fstream>

namespace android {
namespace elf64 {

// Class to parse ELF64 binaries.
//
// The class will parse the 4 parts if present:
//
// - Executable header (Elf64_Ehdr).
// - Program headers (Elf64_Phdr - present in executables or shared libraries).
// - Section headers (Elf64_Shdr)
// - Sections (.interp, .init, .plt, .text, .rodata, .data, .bss, .shstrtab, etc).
//
// The basic usage of the library is:
//
//       android::elf64::Elf64Binary elf64Binary;
//       std::string fileName("new_binary.so");
//       // The content of the elf file will be populated in elf64Binary.
//       android::elf64::Elf64Parser::ParseElfFile(fileName, elf64Binary);
//
class Elf64Parser {
  public:
    // Parse the elf file and populate the elfBinary object.
    // Returns true if the parsing was successful, otherwise false.
    [[nodiscard]] static bool ParseElfFile(const std::string& fileName, Elf64Binary& elfBinary);
    static bool IsElf64(const std::string& fileName);

  private:
    std::ifstream elf64stream;
    Elf64Binary* elfBinaryPtr;

    Elf64Parser(const std::string& fileName, Elf64Binary& elfBinary);
    bool ParseExecutableHeader();
    bool ParseProgramHeaders();
    bool ParseSections();
    bool ParseSectionHeaders();
};

}  // namespace elf64
}  // namespace android
