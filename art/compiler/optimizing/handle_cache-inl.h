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

#ifndef ART_COMPILER_OPTIMIZING_HANDLE_CACHE_INL_H_
#define ART_COMPILER_OPTIMIZING_HANDLE_CACHE_INL_H_

#include "handle_cache.h"

#include "handle_scope-inl.h"

namespace art HIDDEN {

template <typename T>
MutableHandle<T> HandleCache::NewHandle(T* object) {
  return handles_->NewHandle(object);
}

template <typename T>
MutableHandle<T> HandleCache::NewHandle(ObjPtr<T> object) {
  return handles_->NewHandle(object);
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_HANDLE_CACHE_INL_H_
