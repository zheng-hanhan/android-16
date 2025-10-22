/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef CHRE_PLATFORM_MEMORY_H_
#define CHRE_PLATFORM_MEMORY_H_

#include <cstddef>
#include <cstdint>

namespace chre {

/**
 * A platform abstraction for memory allocation. The semantics are the same as
 * malloc.
 */
void *memoryAlloc(size_t size);

/**
 * A platform abstraction for aligned memory allocation. The semantics are the
 * same as aligned_malloc.
 *
 * This implementation is optional and is not typically needed.
 */
template <typename T>
T *memoryAlignedAlloc();

/**
 * A platform abstraction for aligned memory allocation for an array. The
 * semantics are the same as aligned_malloc.
 *
 * This implementation is optional and is not typically needed.
 */
template <typename T>
T *memoryAlignedAllocArray(size_t count);

/**
 * A platform abstraction for memory free. The semantics are the same as free.
 */
void memoryFree(void *pointer);

}  // namespace chre

#include "chre/target_platform/memory_impl.h"

#endif  // CHRE_PLATFORM_MEMORY_H_
