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

#include "ETMDecoder.h"
#include "RegEx.h"
#include "thread_tree.h"
#include "utils.h"

namespace simpleperf {

// When processing binary info in an input file, the binaries are identified by their path.
// But this isn't sufficient when merging binary info from multiple input files. Because
// binaries for the same path may be changed between generating input files. So after processing
// each input file, we create BinaryKeys to identify binaries, which consider path, build_id and
// kernel_start_addr (for vmlinux). kernel_start_addr affects how addresses in ETMBinary
// are interpreted for vmlinux.
struct BinaryKey {
  std::string path;
  BuildId build_id;
  uint64_t kernel_start_addr = 0;

  BinaryKey() {}

  BinaryKey(const std::string& path, BuildId build_id) : path(path), build_id(build_id) {}

  BinaryKey(const Dso* dso, uint64_t kernel_start_addr) : path(dso->Path()) {
    build_id = Dso::FindExpectedBuildIdForPath(dso->Path());
    if (build_id.IsEmpty()) {
      GetBuildId(*dso, build_id);
    }
    if (dso->type() == DSO_KERNEL) {
      this->kernel_start_addr = kernel_start_addr;
    }
  }

  bool operator==(const BinaryKey& other) const {
    return path == other.path && build_id == other.build_id &&
           kernel_start_addr == other.kernel_start_addr;
  }
};

struct BinaryKeyHash {
  size_t operator()(const BinaryKey& key) const noexcept {
    size_t seed = 0;
    HashCombine(seed, key.path);
    HashCombine(seed, key.build_id);
    if (key.kernel_start_addr != 0) {
      HashCombine(seed, key.kernel_start_addr);
    }
    return seed;
  }
};

class BinaryFilter {
 public:
  BinaryFilter(const RegEx* binary_name_regex) : binary_name_regex_(binary_name_regex) {}

  void SetRegex(const RegEx* binary_name_regex) {
    binary_name_regex_ = binary_name_regex;
    dso_filter_cache_.clear();
  }

  bool Filter(const Dso* dso) {
    auto lookup = dso_filter_cache_.find(dso);
    if (lookup != dso_filter_cache_.end()) {
      return lookup->second;
    }
    bool match = Filter(dso->Path());
    dso_filter_cache_.insert({dso, match});
    return match;
  }

  bool Filter(const std::string& path) {
    return binary_name_regex_ == nullptr || binary_name_regex_->Search(path);
  }

 private:
  const RegEx* binary_name_regex_;
  std::unordered_map<const Dso*, bool> dso_filter_cache_;
};

using UnorderedETMBranchMap =
    std::unordered_map<uint64_t, std::unordered_map<std::vector<bool>, uint64_t>>;

struct ETMBinary {
  DsoType dso_type;
  UnorderedETMBranchMap branch_map;

  void Merge(const ETMBinary& other) {
    for (auto& other_p : other.branch_map) {
      auto it = branch_map.find(other_p.first);
      if (it == branch_map.end()) {
        branch_map[other_p.first] = std::move(other_p.second);
      } else {
        auto& map2 = it->second;
        for (auto& other_p2 : other_p.second) {
          auto it2 = map2.find(other_p2.first);
          if (it2 == map2.end()) {
            map2[other_p2.first] = other_p2.second;
          } else {
            OverflowSafeAdd(it2->second, other_p2.second);
          }
        }
      }
    }
  }

  ETMBranchMap GetOrderedBranchMap() const {
    ETMBranchMap result;
    for (const auto& p : branch_map) {
      uint64_t addr = p.first;
      const auto& b_map = p.second;
      result[addr] = std::map<std::vector<bool>, uint64_t>(b_map.begin(), b_map.end());
    }
    return result;
  }
};

using ETMBinaryMap = std::unordered_map<BinaryKey, ETMBinary, BinaryKeyHash>;
bool ETMBinaryMapToString(const ETMBinaryMap& binary_map, std::string& s);
bool StringToETMBinaryMap(const std::string& s, ETMBinaryMap& binary_map);

// Convert ETM data into branch lists while recording.
class ETMBranchListGenerator {
 public:
  static std::unique_ptr<ETMBranchListGenerator> Create(bool dump_maps_from_proc);

  virtual ~ETMBranchListGenerator();
  virtual void SetExcludePid(pid_t pid) = 0;
  virtual void SetBinaryFilter(const RegEx* binary_name_regex) = 0;
  virtual bool ProcessRecord(const Record& r, bool& consumed) = 0;
  virtual ETMBinaryMap GetETMBinaryMap() = 0;
};

struct LBRBranch {
  // If from_binary_id >= 1, it refers to LBRData.binaries[from_binary_id - 1]. Otherwise, it's
  // invalid.
  uint32_t from_binary_id = 0;
  // If to_binary_id >= 1, it refers to LBRData.binaries[to_binary_id - 1]. Otherwise, it's invalid.
  uint32_t to_binary_id = 0;
  uint64_t from_vaddr_in_file = 0;
  uint64_t to_vaddr_in_file = 0;
};

struct LBRSample {
  // If binary_id >= 1, it refers to LBRData.binaries[binary_id - 1]. Otherwise, it's invalid.
  uint32_t binary_id = 0;
  uint64_t vaddr_in_file = 0;
  std::vector<LBRBranch> branches;
};

struct LBRData {
  std::vector<LBRSample> samples;
  std::vector<BinaryKey> binaries;
};

bool LBRDataToString(const LBRData& data, std::string& s);

namespace proto {
class BranchList;
class ETMBinary;
class LBRData;
}  // namespace proto

class BranchListProtoWriter {
 private:
  // This value is choosen to prevent exceeding the 2GB size limit for a protobuf message.
  static constexpr size_t kMaxBranchesPerMessage = 100000000;

 public:
  static std::unique_ptr<BranchListProtoWriter> CreateForFile(
      const std::string& output_filename, bool compress,
      size_t max_branches_per_message = kMaxBranchesPerMessage);
  static std::unique_ptr<BranchListProtoWriter> CreateForString(
      std::string* output_str, bool compress,
      size_t max_branches_per_message = kMaxBranchesPerMessage);

  bool Write(const ETMBinaryMap& etm_data);
  bool Write(const LBRData& lbr_data);

 private:
  BranchListProtoWriter(const std::string& output_filename, std::string* output_str, bool compress,
                        size_t max_branches_per_message)
      : output_filename_(output_filename),
        compress_(compress),
        max_branches_per_message_(max_branches_per_message),
        output_fp_(nullptr, fclose),
        output_str_(output_str) {}

  bool WriteHeader();
  bool WriteProtoBranchList(proto::BranchList& branch_list);
  bool WriteData(const void* data, size_t size);

  const std::string output_filename_;
  const bool compress_;
  const size_t max_branches_per_message_;
  std::unique_ptr<FILE, decltype(&fclose)> output_fp_;
  std::string* output_str_;
};

class BranchListProtoReader {
 public:
  static std::unique_ptr<BranchListProtoReader> CreateForFile(const std::string& input_filename);
  static std::unique_ptr<BranchListProtoReader> CreateForString(const std::string& input_str);
  bool Read(ETMBinaryMap& etm_data, LBRData& lbr_data);

 private:
  BranchListProtoReader(const std::string& input_filename, const std::string& input_str)
      : input_filename_(input_filename), input_fp_(nullptr, fclose), input_str_(input_str) {}
  bool ReadProtoBranchList(uint32_t size, proto::BranchList& proto_branch_list);
  bool AddETMBinary(const proto::ETMBinary& proto_binary, ETMBinaryMap& etm_data);
  void AddLBRData(const proto::LBRData& proto_lbr_data, LBRData& lbr_data);
  void Rewind();
  bool ReadData(void* data, size_t size);
  bool ReadOldFileFormat(ETMBinaryMap& etm_data, LBRData& lbr_data);

  const std::string input_filename_;
  std::unique_ptr<FILE, decltype(&fclose)> input_fp_;
  const std::string& input_str_;
  size_t input_str_pos_ = 0;
  bool compress_ = false;
};

bool DumpBranchListFile(std::string filename);

// for testing
std::string ETMBranchToProtoString(const std::vector<bool>& branch);
std::vector<bool> ProtoStringToETMBranch(const std::string& s, size_t bit_size);

}  // namespace simpleperf
