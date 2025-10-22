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

#ifndef CHRE_UTIL_SYSTEM_FIXED_SIZE_BLOCKING_QUEUE_IMPL_H_
#define CHRE_UTIL_SYSTEM_FIXED_SIZE_BLOCKING_QUEUE_IMPL_H_

// IWYU pragma: private
#include "chre/util/lock_guard.h"
#include "chre/util/system/fixed_size_blocking_queue.h"

namespace chre {

namespace blocking_queue_internal {

template <typename ElementType, typename QueueStorageType>
bool BlockingQueueCore<ElementType, QueueStorageType>::empty() {
  LockGuard<Mutex> lock(mMutex);
  return QueueStorageType::empty();
}

template <typename ElementType, typename QueueStorageType>
size_t BlockingQueueCore<ElementType, QueueStorageType>::size() {
  LockGuard<Mutex> lock(mMutex);
  return QueueStorageType::size();
}

template <typename ElementType, typename QueueStorageType>
bool BlockingQueueCore<ElementType, QueueStorageType>::remove(size_t index) {
  LockGuard<Mutex> lock(mMutex);
  return QueueStorageType::remove(index);
}

template <typename ElementType, typename QueueStorageType>
ElementType &BlockingQueueCore<ElementType, QueueStorageType>::operator[](
    size_t index) {
  LockGuard<Mutex> lock(mMutex);
  return QueueStorageType::operator[](index);
}

template <typename ElementType, typename QueueStorageType>
const ElementType &BlockingQueueCore<ElementType, QueueStorageType>::operator[](
    size_t index) const {
  LockGuard<Mutex> lock(mMutex);
  return QueueStorageType::operator[](index);
}

template <typename ElementType, typename QueueStorageType>
bool BlockingQueueCore<ElementType, QueueStorageType>::push(
    const ElementType &element) {
  bool success;
  {
    LockGuard<Mutex> lock(mMutex);
    success = QueueStorageType::push(element);
  }
  if (success) {
    mConditionVariable.notify_one();
  }
  return success;
}

template <typename ElementType, typename QueueStorageType>
bool BlockingQueueCore<ElementType, QueueStorageType>::push(
    ElementType &&element) {
  bool success;
  {
    LockGuard<Mutex> lock(mMutex);
    success = QueueStorageType::push(std::move(element));
  }
  if (success) {
    mConditionVariable.notify_one();
  }
  return success;
}

template <typename ElementType, typename QueueStorageType>
ElementType BlockingQueueCore<ElementType, QueueStorageType>::pop() {
  LockGuard<Mutex> lock(mMutex);
  while (QueueStorageType::empty()) {
    mConditionVariable.wait(mMutex);
  }

  ElementType element(std::move(QueueStorageType::front()));
  QueueStorageType::pop();
  return element;
}

}  // namespace blocking_queue_internal

}  // namespace chre

#endif  // CHRE_UTIL_BLOCKING_QUEUE_IMPL_H_
