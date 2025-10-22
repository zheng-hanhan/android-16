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

#include <general_test/basic_sensor_test_base.h>

#include <cinttypes>
#include <cstddef>

#include <shared/macros.h>
#include <shared/send_message.h>
#include <shared/time_util.h>

#include "chre/util/nanoapp/log.h"
#include "chre_api/chre.h"

#define LOG_TAG "[BasicSensorTest]"

using nanoapp_testing::kOneMillisecondInNanoseconds;
using nanoapp_testing::kOneSecondInNanoseconds;
using nanoapp_testing::MessageType;

using nanoapp_testing::sendInternalFailureToHost;
using nanoapp_testing::sendStringToHost;
using nanoapp_testing::sendSuccessToHost;

/*
 * Our general test flow is as follows:
 *
 * Constructor: Send startEvent to self to start.
 * StartEvent: Get default sensor and perform various consistency checks.
 * Configure the sensor.
 *
 * At this point, it depends what kind of sensor we have for how we proceed
 * with the test.
 *
 * One-shot: finishTest()
 * On-change: Wait for one data event from sensor.  Then finishTest().
 * Continuous: Wait for two data events from sensor.  Then finishTest().
 *
 * We also look for and perform basic consistency checking on sampling
 * status change events, as well as bias data reports.
 */

namespace general_test {

namespace {
constexpr uint16_t kStartEvent = CHRE_EVENT_FIRST_USER_VALUE;
constexpr uint64_t kEventLoopSlack = 100 * kOneMillisecondInNanoseconds;

uint64_t getEventDuration(const chreSensorThreeAxisData *event) {
  uint64_t duration = 0;
  for (size_t i = 0; i < event->header.readingCount; i++) {
    duration += event->readings[i].timestampDelta;
  }

  return duration;
}

bool isBiasEventType(uint16_t eventType) {
  return (eventType == CHRE_EVENT_SENSOR_ACCELEROMETER_BIAS_INFO) ||
         (eventType == CHRE_EVENT_SENSOR_GYROSCOPE_BIAS_INFO) ||
         (eventType == CHRE_EVENT_SENSOR_GEOMAGNETIC_FIELD_BIAS_INFO);
}

// Helper function to call chreSensorConfigure and log the result
bool configureSensor(uint32_t handle, enum chreSensorConfigureMode mode,
                     uint64_t intervalNs, uint64_t latencyNs) {
  bool success = chreSensorConfigure(handle, mode, intervalNs, latencyNs);
  LOGI("Enabled sensor with handle %" PRIu32 " mode %d interval %" PRIu64
       " latency %" PRIu64 " success=%d",
       handle, mode, intervalNs, latencyNs, success);
  return success;
}

}  // anonymous namespace

BasicSensorTestBase::BasicSensorTestBase()
    : Test(CHRE_API_VERSION_1_0),
      mInMethod(true),
      mExternalSamplingStatusChange(false),
      mState(State::kPreStart),
      mInstanceId(chreGetInstanceId())
/* All other members initialized later */ {}

void BasicSensorTestBase::setUp(uint32_t messageSize,
                                const void * /* message */) {
  if (messageSize != 0) {
    EXPECT_FAIL_RETURN("Beginning message expects 0 additional bytes, got ",
                       &messageSize);
  }

  sendStartTestMessage();
}

void BasicSensorTestBase::sendStartTestMessage() {
  mState = State::kPreStart;
  // Most tests start running in the constructor.  However, since this
  // is a base class, and we invoke abstract methods when running our
  // test, we don't start until after the class has been fully
  // constructed.
  if (!chreSendEvent(kStartEvent, nullptr, nullptr, mInstanceId)) {
    EXPECT_FAIL_RETURN("Failed chreSendEvent to begin test");
  }
  mInMethod = false;
}

void BasicSensorTestBase::checkPassiveConfigure() {
  chreSensorConfigureMode mode =
      isOneShotSensor() ? CHRE_SENSOR_CONFIGURE_MODE_PASSIVE_ONE_SHOT
                        : CHRE_SENSOR_CONFIGURE_MODE_PASSIVE_CONTINUOUS;

  if (mApiVersion == CHRE_API_VERSION_1_0) {
    // Any attempt to make a PASSIVE call with a non-default interval
    // or latency should fail.
    if (configureSensor(mSensorHandle, mode, CHRE_SENSOR_INTERVAL_DEFAULT,
                        999)) {
      EXPECT_FAIL_RETURN(
          "chreSensorConfigure() allowed passive with different latency");
    }
    if (configureSensor(mSensorHandle, mode, 999,
                        CHRE_SENSOR_LATENCY_DEFAULT)) {
      EXPECT_FAIL_RETURN(
          "chreSensorConfigure() allowed passive with different interval");
    }
    // TODO: In a more in-depth test, we should test passive mode
    //     receiving data.  This is somewhat complicated by the fact that
    //     pretty much by definition, we don't control whether a sensor
    //     we're passively listening to is enabled or not.  We could try
    //     to control this with an additional test nanoapp toggling sensor
    //     usage, but there's still the complication of other nanoapps in
    //     the system.
  } else {
    bool configureSuccess =
        configureSensor(mSensorHandle, mode, CHRE_SENSOR_INTERVAL_DEFAULT,
                        kOneSecondInNanoseconds);
    if (mSupportsPassiveMode && !configureSuccess) {
      EXPECT_FAIL_RETURN(
          "chreSensorConfigure() failed passive with default interval and "
          "non-default latency");
    } else if (!mSupportsPassiveMode && configureSuccess) {
      EXPECT_FAIL_RETURN(
          "chreSensorConfigure() accepted passive with default interval and "
          "non-default latency");
    }

    if (!isOneShotSensor()) {
      configureSuccess =
          configureSensor(mSensorHandle, mode, kOneSecondInNanoseconds,
                          CHRE_SENSOR_LATENCY_DEFAULT);
      if (mSupportsPassiveMode && !configureSuccess) {
        EXPECT_FAIL_RETURN(
            "chreSensorConfigure() failed passive with non-default interval "
            "and default latency");
      } else if (!mSupportsPassiveMode && configureSuccess) {
        EXPECT_FAIL_RETURN(
            "chreSensorConfigure() accepted passive with non-default "
            "interval and default latency");
      }

      configureSuccess =
          configureSensor(mSensorHandle, mode, kOneSecondInNanoseconds,
                          kOneSecondInNanoseconds);
      if (mSupportsPassiveMode && !configureSuccess) {
        EXPECT_FAIL_RETURN(
            "chreSensorConfigure() failed passive with non-default interval "
            "and latency");
      } else if (!mSupportsPassiveMode && configureSuccess) {
        EXPECT_FAIL_RETURN(
            "chreSensorConfigure() accepted passive with non-default interval "
            "and latency");
      }
    }
  }
}

void BasicSensorTestBase::startTest() {
  mState = State::kPreConfigure;

  bool found = false;
  uint8_t mSensorType = getSensorType();
  // TODO(b/286604767): CHRE should only expose the default light sensor to
  // nanoapps.
  if (mApiVersion >= CHRE_API_VERSION_1_5 &&
      mSensorType != CHRE_SENSOR_TYPE_LIGHT) {
    found = chreSensorFind(mSensorType, mCurrentSensorIndex, &mSensorHandle);
    if (!found &&
        chreSensorFind(mSensorType, mCurrentSensorIndex + 1, &mSensorHandle)) {
      EXPECT_FAIL_RETURN_UINT8("Missing sensor index ", mCurrentSensorIndex);
      return;
    }
  } else {
    found = chreSensorFindDefault(mSensorType, &mSensorHandle);
  }

  if (!found) {
    sendStringToHost(MessageType::kSkipped,
                     "No default sensor found for optional sensor.");
    return;
  }

  LOGI("Starting test for sensor index %" PRIu8, mCurrentSensorIndex);

  chreSensorInfo info;
  if (!chreGetSensorInfo(mSensorHandle, &info)) {
    EXPECT_FAIL_RETURN("GetSensorInfo() call failed");
  }
  if (info.sensorName == nullptr) {
    EXPECT_FAIL_RETURN("chreSensorInfo::sensorName is NULL");
  }
  if (info.sensorType != mSensorType) {
    uint32_t type = info.sensorType;
    EXPECT_FAIL_RETURN("chreSensorInfo::sensorType is not expected value, is:",
                       &type);
  }
  if (info.isOnChange != isOnChangeSensor()) {
    EXPECT_FAIL_RETURN(
        "chreSensorInfo::isOnChange is opposite of what we expected");
  }
  if (info.isOneShot != isOneShotSensor()) {
    EXPECT_FAIL_RETURN(
        "chreSensorInfo::isOneShot is opposite of what we expected");
  }
  if (mApiVersion >= CHRE_API_VERSION_1_4) {
    mSupportsPassiveMode = info.supportsPassiveMode;
  } else if (info.supportsPassiveMode != 0) {
    EXPECT_FAIL_RETURN("chreSensorInfo::supportsPassiveMode should be 0");
  }

  if (!chreGetSensorSamplingStatus(mSensorHandle, &mOriginalStatus)) {
    EXPECT_FAIL_RETURN("chreGetSensorSamplingStatus() failed");
  } else {
    LOGI("Original sampling status interval=%" PRIu64 " latency=%" PRIu64
         " enabled %d",
         mOriginalStatus.interval, mOriginalStatus.latency,
         mOriginalStatus.enabled);
  }

  // Set the base timestamp to compare against before configuring the sensor.
  mPreTimestamp = chreGetTime();

  // Default interval/latency must be accepted by all sensors.
  mNewStatus = {
      CHRE_SENSOR_INTERVAL_DEFAULT, /* interval */
      CHRE_SENSOR_LATENCY_DEFAULT,  /* latency */
      true                          /* enabled */
  };
  chreSensorConfigureMode mode = isOneShotSensor()
                                     ? CHRE_SENSOR_CONFIGURE_MODE_ONE_SHOT
                                     : CHRE_SENSOR_CONFIGURE_MODE_CONTINUOUS;

  if (!configureSensor(mSensorHandle, mode, mNewStatus.interval,
                       mNewStatus.latency)) {
    EXPECT_FAIL_RETURN(
        "chreSensorConfigure() call failed with default interval and latency");
  }

  // handleEvent may start getting events, and our testing continues there.
  // (Note: The CHRE is not allow to call handleEvent() while we're still
  // in this method, so it's not a race to set this state here.)

  // Set a new request so the test can receive events before test timeout.
  mNewStatus = {
      // This will be valid on all required sensors.
      // TODO: A more in-depth test could try to change this interval
      //     from what it currently is for the sensor, and confirm it
      //     changes back when we're DONE.  But that's beyond the current
      //     scope of this 'basic' test.
      kOneSecondInNanoseconds, /* interval */
      // We want the test to run as quickly as possible.
      // TODO: Similar to the interval, we could try to test changes in
      //     this value, but it's beyond our 'basic' scope for now.
      CHRE_SENSOR_LATENCY_ASAP, /* latency */
      true                      /* enabled */
  };

  // Skip one-shot sensors for non-default interval configurations.
  if (!isOneShotSensor() &&
      !configureSensor(mSensorHandle, mode, mNewStatus.interval,
                       mNewStatus.latency)) {
    EXPECT_FAIL_RETURN("chreSensorConfigure() call failed");
  }

  if (isOnChangeSensor()) {
    // We should receive the current state of this sensor after the
    // configure call.  However, we're not assured additional events,
    // since we don't know if this is going to change.  Thus, we jump
    // our testing state to waiting for the last event.
    mState = State::kExpectingLastDataEvent;
  } else if (isOneShotSensor()) {
    // There's no assurance we'll get any events from a one-shot
    // sensor, so we'll just skip to the end of the test.
    finishTest();
  } else {
    mState = State::kExpectingInitialDataEvent;
  }
}

void BasicSensorTestBase::finishTest() {
  checkPassiveConfigure();

  if (!chreSensorConfigureModeOnly(mSensorHandle,
                                   CHRE_SENSOR_CONFIGURE_MODE_DONE)) {
    EXPECT_FAIL_RETURN("Unable to configure sensor mode to DONE");
  } else {
    LOGI("Successfully disabled sensor");
  }
  mDoneTimestamp = chreGetTime();
  chreSensorSamplingStatus status;
  if (!chreGetSensorSamplingStatus(mSensorHandle, &status)) {
    EXPECT_FAIL_RETURN("Could not get final sensor info");
  }
  LOGI("Final sampling status interval=%" PRIu64 " latency=%" PRIu64
       " enabled %d",
       status.interval, status.latency, status.enabled);
  if (!mExternalSamplingStatusChange) {
    // No one else changed this, so it should be what we had before.
    if (status.enabled != mOriginalStatus.enabled) {
      EXPECT_FAIL_RETURN("SensorInfo.enabled not back to original");
    }
    // Interval and latency values are only relevent if the sensor is enabled.
    if (status.enabled) {
      if (status.interval != mOriginalStatus.interval) {
        EXPECT_FAIL_RETURN("SensorInfo.interval not back to original");
      }
      if (status.latency != mOriginalStatus.latency) {
        EXPECT_FAIL_RETURN("SensorInfo.latency not back to original");
      }
    }
  }

  LOGI("Test passed for sensor index %" PRIu8, mCurrentSensorIndex);

  bool finished = true;
  if (mApiVersion >= CHRE_API_VERSION_1_5) {
    mCurrentSensorIndex++;
    // TODO(b/286604767): CHRE should only expose the default light sensor to
    // nanoapps.
    uint32_t sensorHandle;
    uint8_t mSensorType = getSensorType();
    if (mSensorType != CHRE_SENSOR_TYPE_LIGHT &&
        chreSensorFind(getSensorType(), mCurrentSensorIndex, &sensorHandle)) {
      finished = false;
      mPrevSensorHandle = mSensorHandle;
      sendStartTestMessage();
    }
  }

  if (finished) {
    mState = State::kFinished;
    sendSuccessToHost();
  }
}

void BasicSensorTestBase::verifyEventHeader(const chreSensorDataHeader *header,
                                            uint16_t eventType,
                                            uint64_t eventDuration) {
  if (header->sensorHandle != mSensorHandle) {
    EXPECT_FAIL_RETURN("SensorDataHeader for wrong handle",
                       &header->sensorHandle);
  }

  // Bias and on-change sensor events may have timestamps from before any of our
  // requests started since they aren't generated in response to requests. For
  // these types of events, only ensure the provided timestamp is less than the
  // current time.
  if (!isOnChangeSensor() && !isBiasEventType(eventType)) {
    // An on-change sensor is supposed to send its current state, which
    // could be timestamped in the past.  Everything else should be
    // getting recent data.
    uint64_t *minTime = nullptr;
    uint64_t *timeToUpdate = nullptr;

    if (mState == State::kExpectingInitialDataEvent) {
      minTime = &mPreTimestamp;
      timeToUpdate = &mFirstEventTimestamp;
    } else if (mState == State::kExpectingLastDataEvent) {
      minTime = &mFirstEventTimestamp;
      timeToUpdate = &mLastEventTimestamp;
    } else {  // State::kFinished
      minTime = &mLastEventTimestamp;
      // Go ahead and update this timestamp again.
      timeToUpdate = &mLastEventTimestamp;
    }

    // If there's another CHRE client requesting batched sensor data,
    // baseTimestamp can be before mPreTimestamp. Also allow
    // kEventLoopSlack to handle this nanoapp before handling the sensor
    // event.
    uint64_t minTimeWithSlack =
        (*minTime > eventDuration + kEventLoopSlack)
            ? (*minTime - eventDuration - kEventLoopSlack)
            : 0;
    if (header->baseTimestamp < minTimeWithSlack) {
      LOGE("baseTimestamp %" PRIu64 " < minTimeWithSlack %" PRIu64
           ": minTime %" PRIu64 " eventDuration %" PRIu64
           " kEventLoopSlack %" PRIu64,
           header->baseTimestamp, minTimeWithSlack, *minTime, eventDuration,
           kEventLoopSlack);
      EXPECT_FAIL_RETURN("SensorDataHeader is in the past");
    }
    if ((mState == State::kFinished) &&
        (header->baseTimestamp > mDoneTimestamp)) {
      EXPECT_FAIL_RETURN("SensorDataHeader is from after DONE");
    }
    *timeToUpdate = header->baseTimestamp;
  }

  if (header->baseTimestamp > chreGetTime()) {
    EXPECT_FAIL_RETURN("SensorDataHeader is in the future");
  }

  if (header->readingCount == 0) {
    EXPECT_FAIL_RETURN("SensorDataHeader has readingCount of 0");
  }

  if (header->reserved != 0) {
    EXPECT_FAIL_RETURN("SensorDataHeader has non-zero reserved field");
  }

  if (mApiVersion < CHRE_API_VERSION_1_3) {
    if (header->accuracy != 0) {
      EXPECT_FAIL_RETURN("SensorDataHeader has non-zero reserved field");
    }
  } else if (header->accuracy > CHRE_SENSOR_ACCURACY_HIGH) {
    EXPECT_FAIL_RETURN_UINT8("Sensor accuracy is not within valid range: ",
                             header->accuracy);
  }
}

void BasicSensorTestBase::handleBiasEvent(
    uint16_t eventType, const chreSensorThreeAxisData *eventData) {
  uint8_t expectedSensorType = 0;
  uint32_t eType = eventType;
  if (eventType == CHRE_EVENT_SENSOR_GYROSCOPE_BIAS_INFO) {
    expectedSensorType = CHRE_SENSOR_TYPE_GYROSCOPE;
  } else if (eventType == CHRE_EVENT_SENSOR_GEOMAGNETIC_FIELD_BIAS_INFO) {
    expectedSensorType = CHRE_SENSOR_TYPE_GEOMAGNETIC_FIELD;
  } else if (eventType == CHRE_EVENT_SENSOR_ACCELEROMETER_BIAS_INFO) {
    expectedSensorType = CHRE_SENSOR_TYPE_ACCELEROMETER;
  } else {
    sendInternalFailureToHost("Illegal eventType in handleBiasEvent", &eType);
  }

  if (expectedSensorType != getSensorType()) {
    EXPECT_FAIL_RETURN("Unexpected bias event:", &eType);
  }
  verifyEventHeader(&eventData->header, eventType, getEventDuration(eventData));

  // TODO: consistency check the eventData.  This check is out-of-scope for
  //     Android N testing.
}

void BasicSensorTestBase::handleSamplingChangeEvent(
    const chreSensorSamplingStatusEvent *eventData) {
  LOGI("handleSamplingChangeEvent interval=%" PRIu64 " latency=%" PRIu64
       " enabled=%d",
       eventData->status.interval, eventData->status.latency,
       eventData->status.enabled);
  if (mPrevSensorHandle.has_value() &&
      (mPrevSensorHandle.value() == eventData->sensorHandle)) {
    // We can get a "DONE" event from the previous sensor for multi-sensor
    // devices, so we ignore these events.
    return;
  }

  if (eventData->sensorHandle != mSensorHandle) {
    EXPECT_FAIL_RETURN("SamplingChangeEvent for wrong sensor handle:",
                       &eventData->sensorHandle);
  }

  // TODO: If we strictly define whether this event is or isn't
  //     generated upon being DONE with a sensor, then we can perform
  //     a strict check here.  For now, we just let this go.
  if (mState != State::kFinished) {
    // We received a sensor disabled status change event. While it's not
    // expected, it's possible we received an event that was originated prior to
    // the test run, so we just log a warning rather than failing the test.
    if (!eventData->status.enabled) {
      LOGW("SamplingChangeEvent disabled the sensor.");
    }

    if ((mNewStatus.interval != eventData->status.interval) ||
        (mNewStatus.latency != eventData->status.latency)) {
      // This is from someone other than us.  Let's note that so we know
      // our consistency checks are invalid.
      mExternalSamplingStatusChange = true;
    }
  }
}

void BasicSensorTestBase::handleSensorDataEvent(uint16_t eventType,
                                                const void *eventData) {
  if ((mState == State::kPreStart) || (mState == State::kPreConfigure)) {
    EXPECT_FAIL_RETURN("SensorDataEvent sent too early.");
  }
  // Note, if mState is kFinished, we could be getting batched data which
  // hadn't been delivered yet at the time we were DONE.  We'll consistency
  // check it, even though in theory we're done testing.
  uint64_t eventDuration =
      getEventDuration(static_cast<const chreSensorThreeAxisData *>(eventData));
  verifyEventHeader(static_cast<const chreSensorDataHeader *>(eventData),
                    eventType, eventDuration);

  // Send to the sensor itself for any additional checks of actual data.
  confirmDataIsSane(eventData);
  if (mState == State::kExpectingInitialDataEvent) {
    mState = State::kExpectingLastDataEvent;
  } else if (mState == State::kExpectingLastDataEvent) {
    finishTest();
  } else if (mState != State::kFinished) {
    uint32_t value = static_cast<uint32_t>(mState);
    sendInternalFailureToHost("Illegal mState in handleSensorDataEvent:",
                              &value);
  }
}

void BasicSensorTestBase::handleEvent(uint32_t senderInstanceId,
                                      uint16_t eventType,
                                      const void *eventData) {
  if (mInMethod) {
    EXPECT_FAIL_RETURN("handleEvent() invoked while already in method.");
  }
  mInMethod = true;
  const uint16_t dataEventType =
      CHRE_EVENT_SENSOR_DATA_EVENT_BASE + getSensorType();

  if (senderInstanceId == mInstanceId) {
    if ((eventType == kStartEvent) && (mState == State::kPreStart)) {
      startTest();
    }
  } else if (senderInstanceId != CHRE_INSTANCE_ID) {
    EXPECT_FAIL_RETURN("Unexpected senderInstanceId:", &senderInstanceId);

  } else if (eventData == nullptr) {
    uint32_t eType = eventType;
    EXPECT_FAIL_RETURN("Got NULL eventData for event:", &eType);

  } else if (eventType == dataEventType) {
    handleSensorDataEvent(eventType, eventData);

  } else if (eventType == CHRE_EVENT_SENSOR_SAMPLING_CHANGE) {
    handleSamplingChangeEvent(
        static_cast<const chreSensorSamplingStatusEvent *>(eventData));

  } else if (isBiasEventType(eventType)) {
    handleBiasEvent(eventType,
                    static_cast<const chreSensorThreeAxisData *>(eventData));

  } else {
    unexpectedEvent(eventType);
  }

  mInMethod = false;
}

}  // namespace general_test
