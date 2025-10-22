/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <general_test/estimated_host_time_test.h>

#include <shared/macros.h>
#include <shared/nano_endian.h>
#include <shared/nano_string.h>
#include <shared/send_message.h>

#include "chre_api/chre.h"

namespace general_test {

EstimatedHostTimeTest::EstimatedHostTimeTest()
    : Test(CHRE_API_VERSION_1_1),
      mTimerHandle(CHRE_TIMER_INVALID),
      mRemainingIterations(25) {}

void EstimatedHostTimeTest::setUp(uint32_t /* messageSize */,
                                  const void * /* message */) {
  mPriorHostTime = chreGetEstimatedHostTime();

  constexpr uint64_t timerInterval = 100000000;  // 100 ms

  mTimerHandle =
      chreTimerSet(timerInterval, &mTimerHandle, false /* oneShot */);

  if (mTimerHandle == CHRE_TIMER_INVALID) {
    EXPECT_FAIL_RETURN("Unable to set timer for time verification");
  }
}

void EstimatedHostTimeTest::handleEvent(uint32_t /* senderInstanceId */,
                                        uint16_t eventType,
                                        const void * /* eventData */) {
  if (eventType == CHRE_EVENT_TIMER) {
    verifyIncreasingTime();
  } else {
    // Send the estimated host time to the AP for validation.
    uint64_t currentHostTime = chreGetEstimatedHostTime();
    nanoapp_testing::sendMessageToHost(nanoapp_testing::MessageType::kContinue,
                                       &currentHostTime,
                                       sizeof(currentHostTime));
  }
}

void EstimatedHostTimeTest::verifyIncreasingTime() {
  if (mRemainingIterations > 0) {
    uint64_t currentHostTime = chreGetEstimatedHostTime();

    if (currentHostTime > mPriorHostTime) {
      chreTimerCancel(mTimerHandle);
      nanoapp_testing::sendMessageToHost(
          nanoapp_testing::MessageType::kContinue);
    } else {
      mPriorHostTime = currentHostTime;
    }

    --mRemainingIterations;
  } else {
    EXPECT_FAIL_RETURN("Unable to verify increasing time");
  }
}

}  // namespace general_test
