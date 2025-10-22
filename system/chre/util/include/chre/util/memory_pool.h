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

#ifndef CHRE_UTIL_MEMORY_POOL_H_
#define CHRE_UTIL_MEMORY_POOL_H_

#include <cstddef>

#include "chre/util/non_copyable.h"
#include "chre/util/raw_storage.h"

namespace chre {

/**
 * A memory pool (slab allocator) used for very efficient allocation and
 * deallocation of objects with a uniform size. The goal is to avoid costly
 * malloc/free calls.
 *
 * This implementation is based on the following paper:
 *
 * Fast Efficient Fixed-Size Memory Pool
 * No Loops and No Overhead
 * Ben Kenwright
 *
 * Allocations and deallocation are handled in O(1) time and memory. The list
 * of unused blocks is stored in the space of unused blocks. This means that
 * this data structure has a minimum element size of sizeof(size_t) and means
 * it may not be suitable for very small objects (whose size is less than
 * sizeof(size_t)).
 *
 * One variation is made relative to the allocator described in the paper. To
 * minimize allocation/deallocation latency, the free list is built at
 * construction time.
 */
template <typename ElementType, size_t kSize>
class MemoryPool : public NonCopyable {
 public:
  /**
   * Function used to decide if an element in the pool matches a certain
   * condition.
   *
   * @param element The element.
   * @param data The data passed to the find function.
   * @see find.
   * @return true if the element matched, false otherwise.
   */
  using MatchingFunction = bool(ElementType *element, void *data);

  /**
   * Constructs a MemoryPool and initializes the initial conditions of the
   * allocator.
   */
  MemoryPool();

  /**
   * Allocates space for an object, constructs it and returns the pointer to
   * that object.
   *
   * @param  The arguments to be forwarded to the constructor of the object.
   * @return A pointer to a constructed object or nullptr if the allocation
   *         fails.
   */
  template <typename... Args>
  ElementType *allocate(Args &&... args);

  /**
   * Releases the memory of a previously allocated element. The pointer provided
   * here must be one that was produced by a previous call to the allocate()
   * function. The destructor is invoked on the object.
   *
   * @param A pointer to an element that was previously allocated by the
   *        allocate() function.
   */
  void deallocate(ElementType *element);

  /**
   * Checks if the address of the element provided is within the range managed
   * by this event pool.
   * NOTE: If this method returns true, that does not mean that the provided
   * element has not already been deallocated. It's up to the caller to ensure
   * they don't double deallocate a given element.
   *
   * @param element Address to the element to check if it is in the current
   * memory pool.
   * @return true Returns true if the address of the provided element is
   * managed by this pool.
   */
  bool containsAddress(ElementType *element);

  /**
   * Searches the active blocks in the memory pool, calling
   * the matchingFunction. The first element such that
   * matchingFunction returns true is returned, else nullptr.
   *
   * @param matchingFunction The function to match.
   * @param data The data passed to matchingFunction.
   * @return the first matching element or nullptr if not found.
   */
  ElementType *find(MatchingFunction *matchingFunction, void *data);

  /**
   * @return the number of unused blocks in this memory pool.
   */
  size_t getFreeBlockCount() const;

  /**
   * @return true Return true if this memory pool is empty.
   */
  bool empty() {
    return mFreeBlockCount == kSize;
  }

 private:
  /**
   * The unused storage for this MemoryPool maintains the list of free slots.
   * As such, a union is used to allow storage of both the Element and the index
   * of the next free block in the same space.
   */
  union MemoryPoolBlock {
    //! Intentionally not destructing any leaked blocks, will consider doing
    //! this differently later if required.
    ~MemoryPoolBlock() = delete;

    //! The element stored in the slot.
    ElementType mElement;

    //! The index of the next free block in the unused storage.
    size_t mNextFreeBlockIndex;
  };

  //! The number of bits in a uint32_t.
  static constexpr size_t kBitSizeOfUInt32 = sizeof(uint32_t) * 8;

  //! the number of active tracker blocks - one bit per element.
  static constexpr uint32_t kNumActiveTrackerBlocks =
      kSize % kBitSizeOfUInt32 == 0 ? kSize / kBitSizeOfUInt32
                                    : (kSize / kBitSizeOfUInt32) + 1;

  /**
   * Obtains a pointer to the underlying storage for the memory pool blocks.
   *
   * @return A pointer to the memory pool block storage.
   */
  MemoryPoolBlock *blocks();

  /**
   * Calculate the block index that allocates the element if it belongs to
   * this memory pool.
   *
   * @param element Address to the element.
   * @param indexOutput Calculated block index output.
   * @return false if the address of element does not belong to this memory
   * pool.
   */
  bool getBlockIndex(ElementType *element, size_t *indexOutput);

  /**
   * Sets the bit inside the active tracker blocks to 1 if isActive
   * is true, else 0.
   *
   * @param blockIndex The index of the block inside the storage.
   * @param isActive If the block should be marked active.
   */
  void setBlockActiveStatus(size_t blockIndex, bool isActive);

  //! Storage for memory pool blocks.
  RawStorage<MemoryPoolBlock, kSize> mBlocks;

  //! The index of the head of the free slot list.
  size_t mNextFreeBlockIndex = 0;

  //! The number of free slots available.
  size_t mFreeBlockCount = kSize;

  //! Used to track the active blocks using one bit per block.
  uint32_t mActiveTrackerBlocks[kNumActiveTrackerBlocks];
};

}  // namespace chre

#include "chre/util/memory_pool_impl.h"  // IWYU pragma: export

#endif  // CHRE_UTIL_MEMORY_POOL_H_
