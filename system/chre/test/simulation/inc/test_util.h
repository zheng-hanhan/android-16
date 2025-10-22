/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef CHRE_SIMULATION_TEST_UTIL_H_
#define CHRE_SIMULATION_TEST_UTIL_H_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "android-base/thread_annotations.h"

#include "chre/core/event_loop_manager.h"
#include "chre/core/nanoapp.h"
#include "chre/nanoapp.h"
#include "chre/util/unique_ptr.h"
#include "test_event.h"
#include "test_event_queue.h"

namespace chre {

//! Gets/Sets the timeout for wait/triggerWait() calls.
std::chrono::nanoseconds getWaitTimeout();
void setWaitTimeout(uint64_t timeout);

constexpr uint64_t kDefaultTestNanoappId = 0x0123456789abcdef;

/**
 * Unregister all nanoapps.
 *
 * This is called by the test framework to unregister all nanoapps after each
 * test. The destructor is called when the nanoapp is unregistered.
 */
void unregisterAllTestNanoapps();

/**
 * Information about a test nanoapp.
 */
struct TestNanoappInfo {
  const char *name = "Test";
  uint64_t id = kDefaultTestNanoappId;
  uint32_t version = 0;
  uint32_t perms = NanoappPermissions::CHRE_PERMS_NONE;
};

/**
 * Test nanoapp.
 *
 * Tests typically inherit this class and override the entry points to test
 * the nanoapp behavior.
 *
 * The bulk of the code should be in the handleEvent method to respond to
 * events sent to the nanoapp by the platform and by the sendEventToNanoapp
 * function. start and end can be use to setup and cleanup the test
 * environment around each test.
 *
 * Note: end is only executed when the nanoapp is explicitly unloaded.
 */
class TestNanoapp {
 public:
  TestNanoapp() = default;
  explicit TestNanoapp(TestNanoappInfo info) : mTestNanoappInfo(info) {}
  virtual ~TestNanoapp() {}

  // NanoappStart Entrypoint.
  virtual bool start() {
    return true;
  }

  // nanoappHandleEvent Entrypoint.
  virtual void handleEvent(uint32_t /*senderInstanceId*/,
                           uint16_t /*eventType*/, const void * /*eventData*/) {
  }

  // nanoappEnd Entrypoint.
  virtual void end() {}

  const char *name() {
    return mTestNanoappInfo.name;
  }

  uint64_t id() {
    return mTestNanoappInfo.id;
  }

  uint32_t version() {
    return mTestNanoappInfo.version;
  }

  uint32_t perms() {
    return mTestNanoappInfo.perms;
  }

  //! Call this function to trigger the wait condition and release the
  //! waiting thread. This should only be called by the nanoapp in handleEvent.
  void triggerWait(uint16_t eventType) {
    mWaitingEventTypes.insert(eventType);
    mCondVar.notify_one();
  }

  //! Completes action and expects it to return true, then waits for
  //! triggerWait() to be called by the nanoapp with the given eventType.
  void doActionAndWait(const std::function<bool()> &action,
                       uint16_t eventType) {
    std::unique_lock<std::mutex> lock(mMutex);
    EXPECT_TRUE(action());
    mCondVar.wait_for(lock, getWaitTimeout(), [this, eventType]() {
      return mWaitingEventTypes.find(eventType) != mWaitingEventTypes.end();
    });
    auto iter = mWaitingEventTypes.find(eventType);
    ASSERT_NE(iter, mWaitingEventTypes.end());
    mWaitingEventTypes.erase(iter);
  }

  //! Waits for triggerWait() to be called by the nanoapp with the given
  //! eventType.
  void wait(uint16_t eventType) {
    doActionAndWait([]() { return true; }, eventType);
  }

  std::mutex &mutex() {
    return mMutex;
  }

 private:
  const TestNanoappInfo mTestNanoappInfo;

  //! Mutex and condition variable used to wait for triggerWait() to be
  //! called.
  std::mutex mMutex;
  std::condition_variable mCondVar;
  std::set<uint16_t> mWaitingEventTypes GUARDED_BY(mMutex);
};

/**
 * @return a pointer to a registered nanoapp or nullptr if the appId is not
 *         registered.
 */
TestNanoapp *queryNanoapp(uint64_t appId);

/**
 * @return the statically loaded nanoapp based on the arguments.
 *
 * @see chreNslNanoappInfo for param descriptions.
 */
UniquePtr<Nanoapp> createStaticNanoapp(
    const char *name, uint64_t appId, uint32_t appVersion, uint32_t appPerms,
    decltype(nanoappStart) *startFunc,
    decltype(nanoappHandleEvent) *handleEventFunc,
    decltype(nanoappEnd) *endFunc);

/**
 * @return the statically loaded nanoapp based on the arguments, additionally
 * sets info struct version
 *
 * @see chreNslNanoappInfo for param descriptions.
 */
UniquePtr<Nanoapp> createStaticNanoapp(
    uint8_t infoStructVersion, const char *name, uint64_t appId,
    uint32_t appVersion, uint32_t appPerms, decltype(nanoappStart) *startFunc,
    decltype(nanoappHandleEvent) *handleEventFunc,
    decltype(nanoappEnd) *endFunc);

/**
 * Deletes memory allocated by createStaticNanoapp.
 *
 * This function must be called when the nanoapp is no more used.
 */
void deleteNanoappInfos();

/**
 * Default CHRE nanoapp entry points that don't do anything.
 */
bool defaultNanoappStart();
void defaultNanoappHandleEvent(uint32_t senderInstanceId, uint16_t eventType,
                               const void *eventData);
void defaultNanoappEnd();

/**
 * Create static nanoapp and load it in CHRE.
 *
 * This function returns after the nanoapp start has been executed.
 *
 * @see createStatic Nanoapp.
 */
void loadNanoapp(const char *name, uint64_t appId, uint32_t appVersion,
                 uint32_t appPerms, decltype(nanoappStart) *startFunc,
                 decltype(nanoappHandleEvent) *handleEventFunc,
                 decltype(nanoappEnd) *endFunc);

/**
 * Create a static nanoapp and load it in CHRE.
 *
 * This function returns after the nanoapp start has been executed.
 *
 * @return The id of the nanoapp.
 */
uint64_t loadNanoapp(UniquePtr<TestNanoapp> app);

/**
 * Unload nanoapp corresponding to appId.
 *
 * This function returns after the nanoapp end has been executed.
 *
 * @param appId App Id of nanoapp to be unloaded.
 */
void unloadNanoapp(uint64_t appId);

/**
 * A convenience deferred callback function that can be used to start an
 * already loaded nanoapp.
 *
 * @param type The callback type.
 * @param nanoapp A pointer to the nanoapp that is already loaded.
 */
void testFinishLoadingNanoappCallback(SystemCallbackType type,
                                      UniquePtr<Nanoapp> &&nanoapp);

/**
 * A convenience deferred callback function to unload a nanoapp.
 *
 * @param type The callback type.
 * @param data The data containing the appId.
 * @param extraData Extra data.
 */
void testFinishUnloadingNanoappCallback(uint16_t type, void *data,
                                        void *extraData);

/**
 * Deallocate the memory allocated for a TestEvent.
 */
void freeTestEventDataCallback(uint16_t /*eventType*/, void *eventData);

/**
 * Sends a message to a nanoapp.
 *
 * This function is typically used to execute code in the context of the
 * nanoapp in its handleEvent method.
 *
 * @param appId ID of the nanoapp.
 * @param eventType The event to send.
 */
void sendEventToNanoapp(uint64_t appId, uint16_t eventType);

/**
 * Sends a message to a nanoapp and waits for the nanoapp code to call
 * triggerWait(). The nanoapp code must call triggerWait() to release the
 * waiting thread or this function will never return.
 *
 * This function is typically used to execute code in the context of the
 * nanoapp in its handleEvent method.
 *
 * @param appId ID of the nanoapp.
 * @param eventType The event to send.
 * @param waitEventType The event to wait for.
 */
void sendEventToNanoappAndWait(uint64_t appId, uint16_t eventType,
                               uint16_t waitEventType);

/**
 * Sends a message to a nanoapp with data.
 *
 * This function is typically used to execute code in the context of the
 * nanoapp in its handleEvent method.
 *
 * The nanoapp handleEvent function will receive a a TestEvent instance
 * populated with the eventType and a pointer to as copy of the evenData as
 * a CHRE_EVENT_TEST_EVENT event.
 *
 * @param appId ID of the nanoapp.
 * @param eventType The event to send.
 * @param eventData The data to send.
 */
template <typename T>
void sendEventToNanoapp(uint64_t appId, uint16_t eventType,
                        const T &eventData) {
  static_assert(std::is_trivial<T>::value);
  uint16_t instanceId;
  if (EventLoopManagerSingleton::get()
          ->getEventLoop()
          .findNanoappInstanceIdByAppId(appId, &instanceId)) {
    auto event = memoryAlloc<TestEvent>();
    ASSERT_NE(event, nullptr);
    event->type = eventType;
    auto ptr = memoryAlloc<T>();
    ASSERT_NE(ptr, nullptr);
    *ptr = eventData;
    event->data = ptr;
    EventLoopManagerSingleton::get()->getEventLoop().postEventOrDie(
        CHRE_EVENT_TEST_EVENT, static_cast<void *>(event),
        freeTestEventDataCallback, instanceId);
  } else {
    LOGE("No instance found for nanoapp id = 0x%016" PRIx64, appId);
  }
}

/**
 * Sends a message to a nanoapp with data and waits for the nanoapp code to call
 * triggerWait(). The nanoapp code must call triggerWait() to release the
 * waiting thread or this function will never return.
 *
 * This function is typically used to execute code in the context of the
 * nanoapp in its handleEvent method.
 *
 * The nanoapp handleEvent function will receive a a TestEvent instance
 * populated with the eventType and a pointer to as copy of the evenData as
 * a CHRE_EVENT_TEST_EVENT event.
 *
 * @param appId ID of the nanoapp.
 * @param eventType The event to send.
 * @param eventData The data to send.
 * @param waitEventType The event to wait for.
 */
template <typename T>
void sendEventToNanoappAndWait(uint64_t appId, uint16_t eventType,
                               const T &eventData, uint16_t waitEventType) {
  TestNanoapp *app = queryNanoapp(appId);
  ASSERT_NE(app, nullptr);
  app->doActionAndWait(
      [appId, eventType, &eventData]() {
        sendEventToNanoapp(appId, eventType, eventData);
        return true;
      },
      waitEventType);
}

}  // namespace chre

#endif  // CHRE_SIMULATION_TEST_UTIL_H_
