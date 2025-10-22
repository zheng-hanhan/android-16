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
 * limitations under the License. */

#include <libelf64/parse.h>

#include <elf.h>

#include <fstream>
#include <iostream>

namespace android {
namespace elf64 {

Elf64Parser::Elf64Parser(const std::string& fileName, Elf64Binary& elfBinary)
    : elf64stream(fileName) {
    if (!elf64stream) {
        std::cerr << "Failed to open the file: " << fileName << std::endl;
    }

    elfBinaryPtr = &elfBinary;
}

// Parse the executable header.
//
// Note: The command below can be used to print the executable header:
//
//  $ readelf -h ../shared_lib.so
bool Elf64Parser::ParseExecutableHeader() {
    // Move the cursor position to the very beginning.
    elf64stream.seekg(0);
    elf64stream.read((char*)&elfBinaryPtr->ehdr, sizeof(elfBinaryPtr->ehdr));

    return elf64stream.good();
}

// Parse the Program or Segment Headers.
//
// Note: The command below can be used to print the program headers:
//
//  $ readelf --program-headers ./shared_lib.so
//  $ readelf -l ./shared_lib.so
bool Elf64Parser::ParseProgramHeaders() {
    uint64_t phOffset = elfBinaryPtr->ehdr.e_phoff;
    uint16_t phNum = elfBinaryPtr->ehdr.e_phnum;

    // Move the cursor position to the program header offset.
    elf64stream.seekg(phOffset);

    for (int i = 0; i < phNum; i++) {
        Elf64_Phdr phdr;

        elf64stream.read((char*)&phdr, sizeof(phdr));
        if (!elf64stream) return false;

        elfBinaryPtr->phdrs.push_back(phdr);
    }

    return true;
}

bool Elf64Parser::ParseSections() {
    Elf64_Sc sStrTblPtr;

    // Parse sections after reading all the section headers.
    for (int i = 0; i < elfBinaryPtr->shdrs.size(); i++) {
        uint64_t sOffset = elfBinaryPtr->shdrs[i].sh_offset;
        uint64_t sSize = elfBinaryPtr->shdrs[i].sh_size;

        Elf64_Sc section;

        // Skip .bss section.
        if (elfBinaryPtr->shdrs[i].sh_type != SHT_NOBITS) {
            section.data.resize(sSize);

            // Move the cursor position to the section offset.
            elf64stream.seekg(sOffset);
            elf64stream.read(section.data.data(), sSize);
            if (!elf64stream) return false;
        }

        section.size = sSize;
        section.index = i;

        // The index of the string table is in the executable header.
        if (elfBinaryPtr->ehdr.e_shstrndx == i) {
            sStrTblPtr = section;
        }

        elfBinaryPtr->sections.push_back(section);
    }

    // Set the data section name.
    // This is done after reading the data section with index e_shstrndx.
    for (int i = 0; i < elfBinaryPtr->sections.size(); i++) {
        uint32_t nameIdx = elfBinaryPtr->shdrs[i].sh_name;
        char* st = sStrTblPtr.data.data();

        if (nameIdx < sStrTblPtr.size) {
            CHECK_NE(nullptr, memchr(&st[nameIdx], 0, sStrTblPtr.size - nameIdx));
            elfBinaryPtr->sections[i].name = &st[nameIdx];
        }
    }

    return true;
}

// Parse the Section Headers.
//
// Note: The command below can be used to print the section headers:
//
//   $ readelf --sections ./shared_lib.so
//   $ readelf -S ./shared_lib.so
bool Elf64Parser::ParseSectionHeaders() {
    uint64_t shOffset = elfBinaryPtr->ehdr.e_shoff;
    uint16_t shNum = elfBinaryPtr->ehdr.e_shnum;

    // Move the cursor position to the section headers offset.
    elf64stream.seekg(shOffset);

    for (int i = 0; i < shNum; i++) {
        Elf64_Shdr shdr;

        elf64stream.read((char*)&shdr, sizeof(shdr));
        if (!elf64stream) return false;

        elfBinaryPtr->shdrs.push_back(shdr);
    }

    return true;
}

// Parse the elf file and populate the elfBinary object.
bool Elf64Parser::ParseElfFile(const std::string& fileName, Elf64Binary& elf64Binary) {
    Elf64Parser elf64Parser(fileName, elf64Binary);
    if (elf64Parser.elf64stream && elf64Parser.ParseExecutableHeader() && elf64Binary.IsElf64() &&
        elf64Parser.ParseProgramHeaders() && elf64Parser.ParseSectionHeaders() &&
        elf64Parser.ParseSections()) {
        elf64Binary.path = fileName;
        return true;
    }

    return false;
}

bool Elf64Parser::IsElf64(const std::string& fileName) {
    Elf64Binary elf64Binary;

    Elf64Parser elf64Parser(fileName, elf64Binary);
    if (elf64Parser.elf64stream && elf64Parser.ParseExecutableHeader() && elf64Binary.IsElf64()) {
        return true;
    }

    return false;
}

}  // namespace elf64
}  // namespace android

