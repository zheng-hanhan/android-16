/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>

#if ABSL_INTERNAL_CPLUSPLUS_LANG >= 202002L || __cplusplus >= 202002L
#error This trivial span.h should not be used if the platform supports std::span
#endif  // ABSL_INTERNAL_CPLUSPLUS_LANG >= 202002L || __cplusplus >= 202002L

namespace cppbor {

template <class T>
class span {
  public:
    constexpr span() : mBegin(nullptr), mLen(0) {}
    explicit constexpr span(T* begin, size_t len) : mBegin(begin), mLen(len) {}

    constexpr T* begin() const noexcept { return mBegin; }
    constexpr T* end() const noexcept { return mBegin + mLen; }
    constexpr T* data() const noexcept { return mBegin; }

    constexpr size_t size() const noexcept { return mLen; }

  private:
    T* mBegin;
    size_t mLen;
};

}  // namespace cppbor
