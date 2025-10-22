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

#include <libelf64/comparator.h>
#include <libelf64/elf64.h>
#include <libelf64/parse.h>

#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <elf.h>
#include <stdlib.h>

void usage() {
    const std::string progname = getprogname();

    std::cout << "Usage: " << progname << " [shared_lib_1] [shared_lib_2]\n"
              << R"(
Options:
shared_lib_1    elf64 shared library to compare with shared_lib_2
shared_lib_2    elf64 shared library to compare with shared_lib_1
)" << std::endl;
}

// Compare ELF64 binaries (shared libraries, executables).
int main(int argc, char* argv[]) {
    if (argc < 3) {
        usage();
        return EXIT_FAILURE;
    }

    std::string baseSharedLibName1(argv[1]);
    std::string baseSharedLibName2(argv[2]);

    android::elf64::Elf64Binary elf64Binary1;
    android::elf64::Elf64Binary elf64Binary2;

    bool parse = android::elf64::Elf64Parser::ParseElfFile(baseSharedLibName1, elf64Binary1);
    if (!parse) {
        std::cerr << "Failed to parse file " << baseSharedLibName1 << std::endl;
        return EXIT_FAILURE;
    }

    parse = android::elf64::Elf64Parser::ParseElfFile(baseSharedLibName2, elf64Binary2);
    if (!parse) {
        std::cerr << "Failed to parse file " << baseSharedLibName2 << std::endl;
        return EXIT_FAILURE;
    }

    if (android::elf64::Elf64Comparator::compare(elf64Binary1.ehdr, elf64Binary2.ehdr)) {
        std::cout << "Executable Headers are equal" << std::endl;
    } else {
        std::cout << "Executable Headers are NOT equal" << std::endl;
    }

    if (android::elf64::Elf64Comparator::compare(elf64Binary1.phdrs, elf64Binary2.phdrs)) {
        std::cout << "Program Headers are equal" << std::endl;
    } else {
        std::cout << "Program Headers are NOT equal" << std::endl;
    }

    if (android::elf64::Elf64Comparator::compare(elf64Binary1.shdrs, elf64Binary2.shdrs)) {
        std::cout << "Section Headers are equal" << std::endl;
    } else {
        std::cout << "Section Headers are NOT equal" << std::endl;
    }

    if (android::elf64::Elf64Comparator::compare(elf64Binary1.sections, elf64Binary2.sections)) {
        std::cout << "Sections are equal" << std::endl;
    } else {
        std::cout << "Sections are NOT equal" << std::endl;
    }

    return 0;
}
