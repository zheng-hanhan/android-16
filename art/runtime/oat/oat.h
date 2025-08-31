/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_OAT_OAT_H_
#define ART_RUNTIME_OAT_OAT_H_

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "base/compiler_filter.h"
#include "base/macros.h"
#include "base/safe_map.h"

namespace art HIDDEN {

enum class InstructionSet;
class InstructionSetFeatures;

enum class StubType {
  kJNIDlsymLookupTrampoline,
  kJNIDlsymLookupCriticalTrampoline,
  kQuickGenericJNITrampoline,
  kQuickIMTConflictTrampoline,
  kQuickResolutionTrampoline,
  kQuickToInterpreterBridge,
  kNterpTrampoline,
  kLast = kNterpTrampoline,
};
std::ostream& operator<<(std::ostream& stream, StubType stub_type);

class EXPORT PACKED(4) OatHeader {
 public:
  static constexpr std::array<uint8_t, 4> kOatMagic { { 'o', 'a', 't', '\n' } };
  // Last oat version changed reason: Ensure oat checksum determinism across hosts and devices.
  static constexpr std::array<uint8_t, 4> kOatVersion{{'2', '5', '9', '\0'}};

  static constexpr const char* kDex2OatCmdLineKey = "dex2oat-cmdline";
  static constexpr const char* kDebuggableKey = "debuggable";
  static constexpr const char* kNativeDebuggableKey = "native-debuggable";
  static constexpr const char* kCompilerFilter = "compiler-filter";
  static constexpr const char* kClassPathKey = "classpath";
  static constexpr const char* kBootClassPathKey = "bootclasspath";
  static constexpr const char* kBootClassPathChecksumsKey = "bootclasspath-checksums";
  static constexpr const char* kApexVersionsKey = "apex-versions";
  static constexpr const char* kConcurrentCopying = "concurrent-copying";
  static constexpr const char* kCompilationReasonKey = "compilation-reason";
  static constexpr const char* kRequiresImage = "requires-image";

  // Fields listed here are key value store fields that are deterministic across hosts and devices,
  // meaning they should have exactly the same value when the oat file is generated on different
  // hosts and devices for the same app / boot image and for the same device model with the same
  // compiler options. If you are adding a new field that doesn't hold this property, put it in
  // `kNonDeterministicFieldsAndLengths` and assign a length limit.
  //
  // When writing the oat header, the non-deterministic fields are padded to their length limits and
  // excluded from the oat checksum computation. This makes the oat checksum deterministic across
  // hosts and devices, which is important for Cloud Compilation, where we generate an oat file on a
  // host and use it on a device.
  static constexpr std::array<std::string_view, 9> kDeterministicFields{
      kDebuggableKey,
      kNativeDebuggableKey,
      kCompilerFilter,
      kClassPathKey,
      kBootClassPathKey,
      kBootClassPathChecksumsKey,
      kConcurrentCopying,
      kCompilationReasonKey,
      kRequiresImage,
  };

  static constexpr std::array<std::pair<std::string_view, size_t>, 2>
      kNonDeterministicFieldsAndLengths{
          std::make_pair(kDex2OatCmdLineKey, 2048),
          std::make_pair(kApexVersionsKey, 1024),
      };

  static constexpr const char kTrueValue[] = "true";
  static constexpr const char kFalseValue[] = "false";


  static OatHeader* Create(InstructionSet instruction_set,
                           const InstructionSetFeatures* instruction_set_features,
                           uint32_t dex_file_count,
                           const SafeMap<std::string, std::string>* variable_data,
                           uint32_t base_oat_offset = 0u);
  static void Delete(OatHeader* header);

  static constexpr bool IsDeterministicField(std::string_view key) {
    for (std::string_view field : kDeterministicFields) {
      if (field == key) {
        return true;
      }
    }
    return false;
  }

  static constexpr size_t GetNonDeterministicFieldLength(std::string_view key) {
    for (auto [field, length] : kNonDeterministicFieldsAndLengths) {
      if (field == key) {
        return length;
      }
    }
    return 0;
  }

  bool IsValid() const;
  std::string GetValidationErrorMessage() const;
  static void CheckOatVersion(std::array<uint8_t, 4> version);
  const char* GetMagic() const;
  uint32_t GetChecksum() const;
  void SetChecksum(uint32_t checksum);
  uint32_t GetDexFileCount() const {
    DCHECK(IsValid());
    return dex_file_count_;
  }
  uint32_t GetOatDexFilesOffset() const;
  void SetOatDexFilesOffset(uint32_t oat_dex_files_offset);
  uint32_t GetBcpBssInfoOffset() const;
  void SetBcpBssInfoOffset(uint32_t bcp_info_offset);
  uint32_t GetExecutableOffset() const;
  void SetExecutableOffset(uint32_t executable_offset);

  const void* GetJniDlsymLookupTrampoline() const;
  uint32_t GetJniDlsymLookupTrampolineOffset() const;
  void SetJniDlsymLookupTrampolineOffset(uint32_t offset);
  const void* GetJniDlsymLookupCriticalTrampoline() const;
  uint32_t GetJniDlsymLookupCriticalTrampolineOffset() const;
  void SetJniDlsymLookupCriticalTrampolineOffset(uint32_t offset);

  const void* GetQuickGenericJniTrampoline() const;
  uint32_t GetQuickGenericJniTrampolineOffset() const;
  void SetQuickGenericJniTrampolineOffset(uint32_t offset);
  const void* GetQuickResolutionTrampoline() const;
  uint32_t GetQuickResolutionTrampolineOffset() const;
  void SetQuickResolutionTrampolineOffset(uint32_t offset);
  const void* GetQuickImtConflictTrampoline() const;
  uint32_t GetQuickImtConflictTrampolineOffset() const;
  void SetQuickImtConflictTrampolineOffset(uint32_t offset);
  const void* GetQuickToInterpreterBridge() const;
  uint32_t GetQuickToInterpreterBridgeOffset() const;
  void SetQuickToInterpreterBridgeOffset(uint32_t offset);
  const void* GetNterpTrampoline() const;
  uint32_t GetNterpTrampolineOffset() const;
  void SetNterpTrampolineOffset(uint32_t offset);

  InstructionSet GetInstructionSet() const;
  uint32_t GetInstructionSetFeaturesBitmap() const;

  uint32_t GetKeyValueStoreSize() const;
  const uint8_t* GetKeyValueStore() const;
  const char* GetStoreValueByKeyUnsafe(const char* key) const;

  const char* GetStoreValueByKey(const char* key) const {
    // Do not get apex versions from the oat header directly. Use `OatFile::GetApexVersions`
    // instead.
    DCHECK_NE(std::string_view(key), kApexVersionsKey);
    return GetStoreValueByKeyUnsafe(key);
  }

  // Returns the next key-value pair, at the given offset. On success, updates `offset`.
  // The expected use case is to start the iteration with an offset initialized to zero and
  // repeatedly call this function with the same offset pointer, until the function returns false.
  bool GetNextStoreKeyValuePair(/*inout*/ uint32_t* offset,
                                /*out*/ const char** key,
                                /*out*/ const char** value) const;

  size_t GetHeaderSize() const;
  bool IsDebuggable() const;
  bool IsNativeDebuggable() const;
  CompilerFilter::Filter GetCompilerFilter() const;
  bool IsConcurrentCopying() const;
  bool RequiresImage() const;

  const uint8_t* GetOatAddress(StubType type) const;

  void ComputeChecksum(/*inout*/ uint32_t* checksum) const;

 private:
  bool KeyHasValue(const char* key, const char* value, size_t value_size) const;

  OatHeader(InstructionSet instruction_set,
            const InstructionSetFeatures* instruction_set_features,
            uint32_t dex_file_count,
            const SafeMap<std::string, std::string>* variable_data,
            uint32_t base_oat_offset);

  // Returns true if the value of the given key is "true", false otherwise.
  bool IsKeyEnabled(const char* key) const;

  void Flatten(const SafeMap<std::string, std::string>* variable_data);

  std::array<uint8_t, 4> magic_;
  std::array<uint8_t, 4> version_;
  uint32_t oat_checksum_;

  InstructionSet instruction_set_;
  uint32_t instruction_set_features_bitmap_;
  uint32_t dex_file_count_;
  uint32_t oat_dex_files_offset_;
  uint32_t bcp_bss_info_offset_;
  // Offset of the oat header (i.e. start of the oat data) in the ELF file.
  // It is used to additional validation of the oat header as it is not
  // page-aligned in the memory.
  uint32_t base_oat_offset_;
  uint32_t executable_offset_;
  uint32_t jni_dlsym_lookup_trampoline_offset_;
  uint32_t jni_dlsym_lookup_critical_trampoline_offset_;
  uint32_t quick_generic_jni_trampoline_offset_;
  uint32_t quick_imt_conflict_trampoline_offset_;
  uint32_t quick_resolution_trampoline_offset_;
  uint32_t quick_to_interpreter_bridge_offset_;
  uint32_t nterp_trampoline_offset_;

  uint32_t key_value_store_size_;
  uint8_t key_value_store_[0];  // note variable width data at end

  DISALLOW_COPY_AND_ASSIGN(OatHeader);
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_OAT_H_
