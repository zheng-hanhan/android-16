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

#ifndef ART_RUNTIME_ARCH_CONTEXT_H_
#define ART_RUNTIME_ARCH_CONTEXT_H_

#include <stddef.h>
#include <stdint.h>

#include "arch/instruction_set.h"
#include "base/macros.h"
#include "entrypoints/quick/runtime_entrypoints_list.h"

namespace art HIDDEN {

class QuickMethodFrameInfo;

// Representation of a thread's context on the executing machine, used to implement long jumps in
// the quick stack frame layout.
class Context {
 public:
  // Creates a context for the running architecture
  EXPORT static Context* Create();

  virtual ~Context() {}

  // Re-initializes the registers for context re-use.
  virtual void Reset() = 0;

  template <InstructionSet kIsa>
  static uintptr_t* CalleeSaveAddress(uint8_t* frame, int num, size_t frame_size) {
    static constexpr size_t kPointerSize = static_cast<size_t>(GetInstructionSetPointerSize(kIsa));
    // Callee saves are held at the top of the frame
    uint8_t* save_addr = frame + frame_size - ((num + 1) * kPointerSize);
    if (kIsa == InstructionSet::kX86 || kIsa == InstructionSet::kX86_64) {
      save_addr -= kPointerSize;  // account for return address
    }
    return reinterpret_cast<uintptr_t*>(save_addr);
  }

  // Reads values from callee saves in the given frame. The frame also holds
  // the method that holds the layout.
  virtual void FillCalleeSaves(uint8_t* frame, const QuickMethodFrameInfo& fr) = 0;

  // Sets the stack pointer value.
  virtual void SetSP(uintptr_t new_sp) = 0;

  // Sets the program counter value.
  virtual void SetPC(uintptr_t new_pc) = 0;

  // Sets the first argument register.
  virtual void SetArg0(uintptr_t new_arg0_value) = 0;

  // Returns whether the given GPR is accessible (read or write).
  virtual bool IsAccessibleGPR(uint32_t reg) = 0;

  // Gets the given GPRs address.
  virtual uintptr_t* GetGPRAddress(uint32_t reg) = 0;

  // Reads the given GPR. The caller is responsible for checking the register
  // is accessible with IsAccessibleGPR.
  virtual uintptr_t GetGPR(uint32_t reg) = 0;

  // Sets the given GPR. The caller is responsible for checking the register
  // is accessible with IsAccessibleGPR.
  virtual void SetGPR(uint32_t reg, uintptr_t value) = 0;

  // Returns whether the given FPR is accessible (read or write).
  virtual bool IsAccessibleFPR(uint32_t reg) = 0;

  // Reads the given FPR. The caller is responsible for checking the register
  // is accessible with IsAccessibleFPR.
  virtual uintptr_t GetFPR(uint32_t reg) = 0;

  // Sets the given FPR. The caller is responsible for checking the register
  // is accessible with IsAccessibleFPR.
  virtual void SetFPR(uint32_t reg, uintptr_t value) = 0;

  // Smashes the caller save registers. If we're throwing, we don't want to return bogus values.
  virtual void SmashCallerSaves() = 0;

  // Set `new_value` to the physical register containing the dex PC pointer in
  // an nterp frame.
  virtual void SetNterpDexPC([[maybe_unused]] uintptr_t new_value) { abort(); }

  // Copies the values of GPRs and FPRs registers from this context to external buffers;
  // the use case is to do a long jump afterwards.
  virtual void CopyContextTo(uintptr_t* gprs, uintptr_t* fprs) = 0;

  enum {
    kBadGprBase = 0xebad6070,
    kBadFprBase = 0xebad8070,
  };
};

}  // namespace art

#endif  // ART_RUNTIME_ARCH_CONTEXT_H_
