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

#include <sys/types.h>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <elf.h>

namespace android {
namespace elf64 {

// Section content representation
typedef struct {
    std::vector<char> data;   // Raw content of the data section.
    uint64_t size;   // Size of the data section.
    std::string name;      // The name of the section.
    uint16_t index;  // Index of the section.
} Elf64_Sc;

// Class to represent an ELF64 binary.
//
// An ELF binary is formed by 4 parts:
//
// - Executable header.
// - Program headers (present in executables or shared libraries).
// - Sections (.interp, .init, .plt, .text, .rodata, .data, .bss, .shstrtab, etc).
// - Section headers.
//
//                ______________________
//                |                    |
//                | Executable header  |
//                |____________________|
//                |                    |
//                |                    |
//                |  Program headers   |
//                |                    |
//                |____________________|
//                |                    |
//                |                    |
//                |      Sections      |
//                |                    |
//                |____________________|
//                |                    |
//                |                    |
//                |  Section headers   |
//                |                    |
//                |____________________|
//
//
// The structs defined in linux for ELF parts can be found in:
//
//   - /usr/include/elf.h.
//   - https://elixir.bootlin.com/linux/v5.14.21/source/include/uapi/linux/elf.h#L222
class Elf64Binary {
  public:
    Elf64_Ehdr ehdr;
    std::vector<Elf64_Phdr> phdrs;
    std::vector<Elf64_Shdr> shdrs;
    std::vector<Elf64_Sc> sections;
    std::string path;

    bool IsElf64() { return ehdr.e_ident[EI_CLASS] == ELFCLASS64; }

    // Returns the index of the dynamic section header if found,
    // otherwise it returns -1.
    //
    // Note: The dynamic section can be identified by:
    //
    //   - the section header with name .dynamic
    //   - the section header type SHT_DYNAMIC
    int GetDynamicSectionIndex() {
        for (int i = 0; i < shdrs.size(); i++) {
            if (shdrs.at(i).sh_type == SHT_DYNAMIC) {
                return i;
            }
        }

        return -1;
    }

    // Populate dynEntries with the entries in the .dynamic section.
    void AppendDynamicEntries(std::vector<Elf64_Dyn>* dynEntries) {
        int idx = GetDynamicSectionIndex();

        if (idx == -1) {
            return;
        }

        Elf64_Dyn* dynPtr = (Elf64_Dyn*)sections.at(idx).data.data();
        int numEntries = sections.at(idx).data.size() / sizeof(*dynPtr);

        for (int j = 0; j < numEntries; j++) {
            Elf64_Dyn dynEntry;
            memcpy(&dynEntry, dynPtr, sizeof(*dynPtr));
            dynPtr++;

            dynEntries->push_back(dynEntry);
        }
    }

    // Set the dynEntries in the .dynamic section.
    void SetDynamicEntries(const std::vector<Elf64_Dyn>* dynEntries) {
        int idx = GetDynamicSectionIndex();

        if (idx == -1) {
            return;
        }

        Elf64_Dyn* dynPtr = (Elf64_Dyn*)sections.at(idx).data.data();
        int numEntries = sections.at(idx).data.size() / sizeof(*dynPtr);

        for (int j = 0; j < dynEntries->size() && j < numEntries; j++) {
            memcpy(dynPtr, &dynEntries->at(j), sizeof(*dynPtr));
            dynPtr++;
        }
    }

    // Returns the string at the given offset in the dynamic string table.
    // If .dynamic or .dynstr sections are not found, it returns an empty string.
    // If the offset is invalid, it returns an empty  string.
    std::string GetStrFromDynStrTable(Elf64_Xword offset) {
        int idx = GetDynamicSectionIndex();

        if (idx == -1) {
            return "";
        }

        // Get the index of the string table .dynstr.
        Elf64_Word dynStrIdx = shdrs.at(idx).sh_link;
        if (offset >= sections.at(dynStrIdx).data.size()) {
            return "";
        }

        char* st = sections.at(dynStrIdx).data.data();

        CHECK_NE(nullptr, memchr(&st[offset], 0, sections.at(dynStrIdx).data.size() - offset));
        return &st[offset];
    }
};

}  // namespace elf64
}  // namespace android
