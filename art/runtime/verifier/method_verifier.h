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

#ifndef ART_RUNTIME_VERIFIER_METHOD_VERIFIER_H_
#define ART_RUNTIME_VERIFIER_METHOD_VERIFIER_H_

#include <memory>
#include <sstream>
#include <vector>

#include <android-base/logging.h>

#include "base/arena_allocator.h"
#include "base/arena_containers.h"
#include "base/macros.h"
#include "base/value_object.h"
#include "dex/code_item_accessors.h"
#include "dex/dex_file_types.h"
#include "dex/method_reference.h"
#include "handle.h"
#include "instruction_flags.h"
#include "register_line.h"
#include "verifier_enums.h"

namespace art HIDDEN {

class ClassLinker;
class DexFile;
class Instruction;
struct ReferenceMap2Visitor;
class Thread;
class VariableIndentationOutputStream;

namespace dex {
struct ClassDef;
struct CodeItem;
}  // namespace dex

namespace mirror {
class ClassLoader;
class DexCache;
}  // namespace mirror

namespace verifier {

class MethodVerifier;
class RegType;
class RegTypeCache;
struct ScopedNewLine;
class VerifierDeps;

// A mapping from a dex pc to the register line statuses as they are immediately prior to the
// execution of that instruction.
class PcToRegisterLineTable {
 public:
  explicit PcToRegisterLineTable(ArenaAllocator& allocator);
  ~PcToRegisterLineTable();

  // Initialize the RegisterTable. Every instruction address can have a different set of information
  // about what's in which register, but for verification purposes we only need to store it at
  // branch target addresses (because we merge into that).
  void Init(InstructionFlags* flags,
            uint32_t insns_size,
            uint16_t registers_size,
            ArenaAllocator& allocator,
            uint32_t interesting_dex_pc);

  bool IsInitialized() const {
    return !register_lines_.empty();
  }

  RegisterLine* GetLine(size_t idx) const {
    return register_lines_[idx].get();
  }

 private:
  ArenaVector<RegisterLineArenaUniquePtr> register_lines_;

  DISALLOW_COPY_AND_ASSIGN(PcToRegisterLineTable);
};

// The verifier
class MethodVerifier {
 public:
  EXPORT static void VerifyMethodAndDump(Thread* self,
                                         VariableIndentationOutputStream* vios,
                                         uint32_t method_idx,
                                         const DexFile* dex_file,
                                         Handle<mirror::DexCache> dex_cache,
                                         Handle<mirror::ClassLoader> class_loader,
                                         const dex::ClassDef& class_def,
                                         const dex::CodeItem* code_item,
                                         uint32_t method_access_flags,
                                         uint32_t api_level)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Calculates the type information at the given `dex_pc`.
  // No classes will be loaded.
  EXPORT static MethodVerifier* CalculateVerificationInfo(Thread* self,
                                                          RegTypeCache* reg_types,
                                                          ArtMethod* method,
                                                          Handle<mirror::DexCache> dex_cache,
                                                          uint32_t dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_);

  const DexFile& GetDexFile() const {
    DCHECK(dex_file_ != nullptr);
    return *dex_file_;
  }

  const dex::ClassDef& GetClassDef() const {
    return class_def_;
  }

  RegTypeCache* GetRegTypeCache() {
    return &reg_types_;
  }

  // Log a verification failure.
  std::ostream& Fail(VerifyError error, bool pending_exc = true);

  // Log for verification information.
  ScopedNewLine LogVerifyInfo();

  // Information structure for a lock held at a certain point in time.
  struct DexLockInfo {
    // The registers aliasing the lock.
    std::set<uint32_t> dex_registers;
    // The dex PC of the monitor-enter instruction.
    uint32_t dex_pc;

    explicit DexLockInfo(uint32_t dex_pc_in) {
      dex_pc = dex_pc_in;
    }
  };
  // Fills 'monitor_enter_dex_pcs' with the dex pcs of the monitor-enter instructions corresponding
  // to the locks held at 'dex_pc' in method 'm'.
  // Note: this is the only situation where the verifier will visit quickened instructions.
  static void FindLocksAtDexPc(ArtMethod* m,
                               uint32_t dex_pc,
                               std::vector<DexLockInfo>* monitor_enter_dex_pcs,
                               uint32_t api_level)
      REQUIRES_SHARED(Locks::mutator_lock_);

  virtual ~MethodVerifier() {}

  const CodeItemDataAccessor& CodeItem() const {
    return code_item_accessor_;
  }
  RegisterLine* GetRegLine(uint32_t dex_pc);
  ALWAYS_INLINE const InstructionFlags& GetInstructionFlags(size_t index) const;

  MethodReference GetMethodReference() const;
  bool HasFailures() const;
  bool HasInstructionThatWillThrow() const {
    return (encountered_failure_types_ & VERIFY_ERROR_RUNTIME_THROW) != 0;
  }

  uint32_t GetEncounteredFailureTypes() const {
    return encountered_failure_types_;
  }

  ClassLinker* GetClassLinker() const;

  bool IsAotMode() const {
    return const_flags_.aot_mode_;
  }

  bool CanLoadClasses() const {
    return const_flags_.can_load_classes_;
  }

  VerifierDeps* GetVerifierDeps() const {
    return verifier_deps_;
  }

 protected:
  MethodVerifier(Thread* self,
                 ArenaPool* arena_pool,
                 RegTypeCache* reg_types,
                 VerifierDeps* verifier_deps,
                 const dex::ClassDef& class_def,
                 const dex::CodeItem* code_item,
                 uint32_t dex_method_idx,
                 bool aot_mode)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Verification result for method(s). Includes a (maximum) failure kind, and (the union of)
  // all failure types.
  struct FailureData : ValueObject {
    FailureKind kind = FailureKind::kNoFailure;
    uint32_t types = 0U;

    // Merge src into this. Uses the most severe failure kind, and the union of types.
    void Merge(const FailureData& src);
  };

  /*
   * Perform verification on a single method.
   *
   * We do this in three passes:
   *  (1) Walk through all code units, determining instruction locations,
   *      widths, and other characteristics.
   *  (2) Walk through all code units, performing static checks on
   *      operands.
   *  (3) Iterate through the method, checking type safety and looking
   *      for code flow problems.
   */
  static FailureData VerifyMethod(Thread* self,
                                  ArenaPool* arena_pool,
                                  RegTypeCache* reg_types,
                                  VerifierDeps* verifier_deps,
                                  uint32_t method_idx,
                                  Handle<mirror::DexCache> dex_cache,
                                  const dex::ClassDef& class_def_idx,
                                  const dex::CodeItem* code_item,
                                  uint32_t method_access_flags,
                                  HardFailLogMode log_level,
                                  uint32_t api_level,
                                  bool aot_mode,
                                  std::string* hard_failure_msg)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <bool kVerifierDebug>
  static FailureData VerifyMethod(Thread* self,
                                  ArenaPool* arena_pool,
                                  RegTypeCache* reg_types,
                                  VerifierDeps* verifier_deps,
                                  uint32_t method_idx,
                                  Handle<mirror::DexCache> dex_cache,
                                  const dex::ClassDef& class_def_idx,
                                  const dex::CodeItem* code_item,
                                  uint32_t method_access_flags,
                                  HardFailLogMode log_level,
                                  uint32_t api_level,
                                  bool aot_mode,
                                  std::string* hard_failure_msg)
      REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Get the "this" pointer from a non-static method invocation. This returns the RegType so the
   * caller can decide whether it needs the reference to be initialized or not.
   *
   * The argument count is in vA, and the first argument is in vC, for both "simple" and "range"
   * versions. We just need to make sure vA is >= 1 and then return vC.
   */
  const RegType& GetInvocationThis(const Instruction* inst)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Can a variable with type `lhs` be assigned a value with type `rhs`?
  // Note: Object and interface types may always be assigned to one another, see
  // comment on `ClassJoin()`.
  bool IsAssignableFrom(const RegType& lhs, const RegType& rhs) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Can a variable with type `lhs` be assigned a value with type `rhs`?
  // Variant of IsAssignableFrom that doesn't allow assignment to an interface from an Object.
  bool IsStrictlyAssignableFrom(const RegType& lhs, const RegType& rhs) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Implementation helper for `IsAssignableFrom()` and `IsStrictlyAssignableFrom()`.
  bool AssignableFrom(const RegType& lhs, const RegType& rhs, bool strict) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // For VerifierDepsTest. TODO: Refactor.

  // Run verification on the method. Returns true if verification completes and false if the input
  // has an irrecoverable corruption.
  virtual bool Verify() REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  static MethodVerifier* CreateVerifier(Thread* self,
                                        RegTypeCache* reg_types,
                                        VerifierDeps* verifier_deps,
                                        Handle<mirror::DexCache> dex_cache,
                                        const dex::ClassDef& class_def,
                                        const dex::CodeItem* code_item,
                                        uint32_t method_idx,
                                        uint32_t access_flags,
                                        bool verify_to_dump,
                                        uint32_t api_level)
      REQUIRES_SHARED(Locks::mutator_lock_);

  virtual void PotentiallyMarkRuntimeThrow() = 0;

  std::ostringstream& InfoMessages() {
    if (!info_messages_.has_value()) {
      info_messages_.emplace();
    }
    return info_messages_.value();
  }

  // The thread we're verifying on.
  Thread* const self_;

  // Arena allocator.
  ArenaAllocator allocator_;

  RegTypeCache& reg_types_;  // TODO: Change to a pointer in a separate CL.

  PcToRegisterLineTable reg_table_;

  // Storage for the register status we're currently working on.
  RegisterLineArenaUniquePtr work_line_;

  // The address of the instruction we're currently working on, note that this is in 2 byte
  // quantities
  uint32_t work_insn_idx_;

  // Storage for the register status we're saving for later.
  RegisterLineArenaUniquePtr saved_line_;

  const uint32_t dex_method_idx_;   // The method we're working on.
  const DexFile* const dex_file_;   // The dex file containing the method.
  const dex::ClassDef& class_def_;  // The class being verified.
  const CodeItemDataAccessor code_item_accessor_;

  // Instruction widths and flags, one entry per code unit.
  // Owned, but not unique_ptr since insn_flags_ are allocated in arenas.
  ArenaUniquePtr<InstructionFlags[]> insn_flags_;

  // The types of any error that occurs and associated error messages.
  using MessageOStream =
      std::basic_ostringstream<char, std::char_traits<char>, ArenaAllocatorAdapter<char>>;
  struct VerifyErrorAndMessage {
    VerifyErrorAndMessage(VerifyError e, const std::string& location, ArenaAllocatorAdapter<char> a)
        : error(e), message(location, std::ios_base::ate, a) {}
    VerifyError error;
    MessageOStream message;
  };
  ArenaList<VerifyErrorAndMessage> failures_;

  struct {
    // Is there a pending hard failure?
    bool have_pending_hard_failure_ : 1;

    // Is there a pending runtime throw failure? A runtime throw failure is when an instruction
    // would fail at runtime throwing an exception. Such an instruction causes the following code
    // to be unreachable. This is set by Fail and used to ensure we don't process unreachable
    // instructions that would hard fail the verification.
    // Note: this flag is reset after processing each instruction.
    bool have_pending_runtime_throw_failure_ : 1;
  } flags_;

  struct {
    // Verify in AoT mode?
    bool aot_mode_ : 1;

    // Whether the `MethodVerifer` can load classes.
    bool can_load_classes_ : 1;
  } const const_flags_;

  // Bitset of the encountered failure types. Bits are according to the values in VerifyError.
  uint32_t encountered_failure_types_;

  // Info message log use primarily for verifier diagnostics.
  std::optional<std::ostringstream> info_messages_;

  // The verifier deps object we are going to report type assigability
  // constraints to. Can be null for runtime verification.
  VerifierDeps* verifier_deps_;

  // Link, for the method verifier root linked list.
  MethodVerifier* link_;

  friend class art::Thread;
  friend class ClassVerifier;
  friend class RegisterLineTest;
  friend class VerifierDepsTest;

  DISALLOW_COPY_AND_ASSIGN(MethodVerifier);
};

}  // namespace verifier
}  // namespace art

#endif  // ART_RUNTIME_VERIFIER_METHOD_VERIFIER_H_
