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

#include "gtest/gtest.h"

#include "chre/util/memory_pool.h"
#include "chre/util/nested_data_ptr.h"

#include <random>
#include <vector>

using ::chre::MemoryPool;
using ::chre::NestedDataPtr;

TEST(MemoryPool, ExhaustPool) {
  MemoryPool<int, 3> memoryPool;
  EXPECT_EQ(memoryPool.getFreeBlockCount(), 3);
  EXPECT_NE(memoryPool.allocate(), nullptr);
  EXPECT_EQ(memoryPool.getFreeBlockCount(), 2);
  EXPECT_NE(memoryPool.allocate(), nullptr);
  EXPECT_EQ(memoryPool.getFreeBlockCount(), 1);
  EXPECT_NE(memoryPool.allocate(), nullptr);
  EXPECT_EQ(memoryPool.getFreeBlockCount(), 0);
  EXPECT_EQ(memoryPool.allocate(), nullptr);
  EXPECT_EQ(memoryPool.getFreeBlockCount(), 0);
}

TEST(MemoryPool, OwnershipDeallocation) {
  MemoryPool<int, 3> firstMemoryPool;
  MemoryPool<int, 3> secondMemoryPool;

  int *firstMemoryElement = firstMemoryPool.allocate();
  EXPECT_TRUE(firstMemoryPool.containsAddress(firstMemoryElement));
  EXPECT_FALSE(secondMemoryPool.containsAddress(firstMemoryElement));

  EXPECT_DEATH(secondMemoryPool.deallocate(firstMemoryElement), "");
  firstMemoryPool.deallocate(firstMemoryElement);
}

TEST(MemoryPool, ExhaustPoolThenDeallocateOneAndAllocateOne) {
  MemoryPool<int, 3> memoryPool;

  // Exhaust the pool.
  int *element1 = memoryPool.allocate();
  int *element2 = memoryPool.allocate();
  int *element3 = memoryPool.allocate();

  // Perform some simple assignments. There is a chance we crash here if things
  // are not implemented correctly.
  *element1 = 0xcafe;
  *element2 = 0xbeef;
  *element3 = 0xface;

  // Free one element and then allocate another.
  memoryPool.deallocate(element1);
  EXPECT_EQ(memoryPool.getFreeBlockCount(), 1);
  element1 = memoryPool.allocate();
  EXPECT_NE(element1, nullptr);

  // Ensure that the pool remains exhausted.
  EXPECT_EQ(memoryPool.allocate(), nullptr);

  // Perform another simple assignment. There is a hope that this can crash if
  // the pointer returned is very bad (like nullptr).
  *element1 = 0xfade;

  // Verify that the values stored were not corrupted by the deallocate
  // allocate cycle.
  EXPECT_EQ(*element1, 0xfade);
  EXPECT_EQ(*element2, 0xbeef);
  EXPECT_EQ(*element3, 0xface);
}

/*
 * Pair an allocated pointer with the expected value that should be stored in
 * that location.
 */
struct AllocationExpectedValuePair {
  size_t *allocation;
  size_t expectedValue;
};

TEST(MemoryPool, ExhaustPoolThenRandomDeallocate) {
  // The number of times to allocate and deallocate in random order.
  const size_t kStressTestCount = 64;

  // Construct a memory pool and a vector to maintain a list of all allocations.
  const size_t kMemoryPoolSize = 64;
  MemoryPool<size_t, kMemoryPoolSize> memoryPool;
  std::vector<AllocationExpectedValuePair> allocations;

  for (size_t i = 0; i < kStressTestCount; i++) {
    // Exhaust the memory pool.
    for (size_t j = 0; j < kMemoryPoolSize; j++) {
      AllocationExpectedValuePair allocation = {
          .allocation = memoryPool.allocate(),
          .expectedValue = j,
      };

      *allocation.allocation = j;
      allocations.push_back(allocation);
    }

    // Seed a random number generator with the loop iteration so that order is
    // preserved across test runs.
    std::mt19937 randomGenerator(i);

    while (!allocations.empty()) {
      // Generate a number with a uniform distribution between zero and the
      // number of allocations remaining.
      std::uniform_int_distribution<> distribution(0, allocations.size() - 1);
      size_t deallocateIndex = distribution(randomGenerator);

      // Verify the expected value and free the allocation.
      EXPECT_EQ(*allocations[deallocateIndex].allocation,
                allocations[deallocateIndex].expectedValue);
      memoryPool.deallocate(allocations[deallocateIndex].allocation);

      // Remove the freed allocation from the allocation list.
      allocations.erase(allocations.begin() + deallocateIndex);
    }
  }
}

TEST(MemoryPool, FindAnElement) {
  MemoryPool<int, 4> memoryPool;

  // Exhaust the pool.
  int *element1 = memoryPool.allocate();
  int *element2 = memoryPool.allocate();
  int *element3 = memoryPool.allocate();
  int *element4 = memoryPool.allocate();

  // Perform some simple assignments. There is a chance we crash here if things
  // are not implemented correctly.
  *element1 = 0xcafe;
  *element2 = 0xbeef;
  *element3 = 0xface;
  *element4 = 0xface;

  // Do a find with a known element.
  int *foundElement = memoryPool.find(
      [](int *element, void *data) {
        NestedDataPtr<int> value(data);
        return *element == value;
      },
      NestedDataPtr<int>(0xface));
  EXPECT_NE(foundElement, nullptr);
  EXPECT_EQ(foundElement, element3);

  // Do a find with an unknown element.
  foundElement = memoryPool.find(
      [](int *element, void *data) {
        NestedDataPtr<int> value(data);
        return *element == value;
      },
      NestedDataPtr<int>(0xaaaa));
  EXPECT_EQ(foundElement, nullptr);
}

TEST(MemoryPool, FindAnElementAfterDeallocation) {
  MemoryPool<int, 4> memoryPool;

  // Exhaust the pool.
  int *element1 = memoryPool.allocate();
  int *element2 = memoryPool.allocate();
  int *element3 = memoryPool.allocate();
  int *element4 = memoryPool.allocate();

  // Perform some simple assignments. There is a chance we crash here if things
  // are not implemented correctly.
  *element1 = 0xcafe;
  *element2 = 0xbeef;
  *element3 = 0xface;
  *element4 = 0xface;

  // Deallocate element 3 then try to find element 4.
  memoryPool.deallocate(element3);
  int *foundElement = memoryPool.find(
      [](int *element, void *data) {
        NestedDataPtr<int> value(data);
        return *element == value;
      },
      NestedDataPtr<int>(0xface));
  EXPECT_NE(foundElement, nullptr);
  EXPECT_EQ(foundElement, element4);
}

TEST(MemoryPool, FindAnElementAfterAllMatchingAreDeallocated) {
  MemoryPool<int, 4> memoryPool;

  // Exhaust the pool.
  int *element1 = memoryPool.allocate();
  int *element2 = memoryPool.allocate();
  int *element3 = memoryPool.allocate();
  int *element4 = memoryPool.allocate();

  // Perform some simple assignments. There is a chance we crash here if things
  // are not implemented correctly.
  *element1 = 0xcafe;
  *element2 = 0xbeef;
  *element3 = 0xface;
  *element4 = 0xface;

  // Deallocate element 3 then try to find element 4.
  memoryPool.deallocate(element3);
  int *foundElement = memoryPool.find(
      [](int *element, void *data) {
        NestedDataPtr<int> value(data);
        return *element == value;
      },
      NestedDataPtr<int>(0xface));
  EXPECT_NE(foundElement, nullptr);
  EXPECT_EQ(foundElement, element4);

  // Deallocate element 4 and try to find again.
  memoryPool.deallocate(element4);
  foundElement = memoryPool.find(
      [](int *element, void *data) {
        NestedDataPtr<int> value(data);
        return *element == value;
      },
      NestedDataPtr<int>(0xface));
  EXPECT_EQ(foundElement, nullptr);
}

TEST(MemoryPool, FindAnElementAfterDeallocationLargeSize) {
  constexpr size_t kNumElements = 1000;
  MemoryPool<int, kNumElements> memoryPool;
  int *elements[kNumElements];

  for (size_t i = 0; i < kNumElements; ++i) {
    elements[i] = memoryPool.allocate();
    *elements[i] = i;
  }

  // Deallocate even elements.
  for (size_t i = 0; i < kNumElements; ++i) {
    if (i % 2 == 0) {
      memoryPool.deallocate(elements[i]);
    }
  }

  // Find odd elements.
  for (size_t i = 0; i < kNumElements; ++i) {
    int *foundElement = memoryPool.find(
        [](int *element, void *data) {
          NestedDataPtr<int> value(data);
          return *element == value;
        },
        NestedDataPtr<int>(i));
    if (i % 2 == 0) {
      EXPECT_EQ(foundElement, nullptr);
    } else {
      EXPECT_NE(foundElement, nullptr);
      EXPECT_EQ(foundElement, elements[i]);
    }
  }
}
