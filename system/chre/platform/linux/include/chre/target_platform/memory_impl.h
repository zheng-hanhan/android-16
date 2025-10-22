/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef CHRE_PLATFORM_LINUX_MEMORY_IMPL_H_
#define CHRE_PLATFORM_LINUX_MEMORY_IMPL_H_

#include <stdlib.h>
#include <cstring>

namespace chre {

template <typename T>
inline T *memoryAlignedAlloc() {
  void *ptr;
  int result = posix_memalign(&ptr, alignof(T), sizeof(T));
  if (result != 0) {
    ptr = nullptr;
  }
  return static_cast<T *>(ptr);
}

template <typename T>
inline T *memoryAlignedAllocArray(size_t count) {
  void *ptr;
  int result = posix_memalign(&ptr, alignof(T), sizeof(T) * count);
  if (result != 0) {
    ptr = nullptr;
  }
  return static_cast<T *>(ptr);
}

}  // namespace chre

#endif  // CHRE_PLATFORM_LINUX_MEMORY_IMPL_H_
