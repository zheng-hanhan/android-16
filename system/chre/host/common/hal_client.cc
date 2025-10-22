/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef LOG_TAG
#define LOG_TAG "CHRE.HAL.CLIENT"
#endif

#include "chre_host/hal_client.h"
#include "chre_host/log.h"

#include <android-base/properties.h>
#include <android_chre_flags.h>
#include <utils/SystemClock.h>

#include <cinttypes>
#include <thread>

namespace android::chre {

using ::aidl::android::hardware::contexthub::IContextHub;
using ::aidl::android::hardware::contexthub::IContextHubCallback;
using ::android::base::GetBoolProperty;
using ::ndk::ScopedAStatus;

namespace {
constexpr char kHalEnabledProperty[]{"vendor.chre.multiclient_hal.enabled"};

// Multiclient HAL needs getUuid() added since V3 to identify each client.
constexpr int kMinHalInterfaceVersion = 3;
}  // namespace

bool HalClient::isServiceAvailable() {
  return GetBoolProperty(kHalEnabledProperty, /* default_value= */ false);
}

std::unique_ptr<HalClient> HalClient::create(
    const std::shared_ptr<IContextHubCallback> &callback,
    int32_t contextHubId) {
  if (callback == nullptr) {
    LOGE("Callback function must not be null");
    return nullptr;
  }

  if (!isServiceAvailable()) {
    LOGE("CHRE Multiclient HAL is not enabled on this device");
    return nullptr;
  }

  if (callback->version < kMinHalInterfaceVersion) {
    LOGE("Callback interface version is %" PRIi32 ". It must be >= %" PRIi32,
         callback->version, kMinHalInterfaceVersion);
    return nullptr;
  }

  return std::unique_ptr<HalClient>(new HalClient(callback, contextHubId));
}

HalError HalClient::initConnection() {
  std::lock_guard<std::shared_mutex> lockGuard{mConnectionLock};

  if (mContextHub != nullptr) {
    LOGW("%s is already connected to CHRE HAL", mClientName.c_str());
    return HalError::SUCCESS;
  }

  // Wait to connect to the service. Note that we don't do local retries
  // because we're relying on the internal retries in
  // AServiceManager_waitForService(). If HAL service has just restarted, it
  // can take a few seconds to connect.
  ndk::SpAIBinder binder{
      AServiceManager_waitForService(kAidlServiceName.c_str())};
  if (binder.get() == nullptr) {
    return HalError::BINDER_CONNECTION_FAILED;
  }

  // Link the death recipient to handle the binder disconnection event.
  if (AIBinder_linkToDeath(binder.get(), mDeathRecipient.get(), this) !=
      STATUS_OK) {
    LOGE("Failed to link the binder death recipient");
    return HalError::LINK_DEATH_RECIPIENT_FAILED;
  }

  // Retrieve a handle of context hub service.
  mContextHub = IContextHub::fromBinder(binder);
  if (mContextHub == nullptr) {
    LOGE("Got null context hub from the binder connection");
    return HalError::NULL_CONTEXT_HUB_FROM_BINDER;
  }

  // Enforce the required interface version for the service.
  int32_t version = 0;
  mContextHub->getInterfaceVersion(&version);
  if (version < kMinHalInterfaceVersion) {
    LOGE("CHRE multiclient HAL interface version is %" PRIi32
         ". It must be >= %" PRIi32,
         version, kMinHalInterfaceVersion);
    mContextHub = nullptr;
    return HalError::VERSION_TOO_LOW;
  }

  // Register an IContextHubCallback.
  ScopedAStatus status =
      mContextHub->registerCallback(kDefaultContextHubId, mCallback);
  if (!status.isOk()) {
    LOGE("Unable to register callback: %s", status.getDescription().c_str());
    // At this moment it's guaranteed that mCallback is not null and
    // kDefaultContextHubId is valid. So if the registerCallback() still fails
    // it's a hard failure and CHRE HAL is treated as disconnected.
    mContextHub = nullptr;
    return HalError::CALLBACK_REGISTRATION_FAILED;
  }
  mIsHalConnected = true;
  LOGI("%s is successfully (re)connected to CHRE HAL", mClientName.c_str());
  return HalError::SUCCESS;
}

void HalClient::onHalDisconnected(void *cookie) {
  int64_t startTime = ::android::elapsedRealtime();
  auto *halClient = static_cast<HalClient *>(cookie);
  {
    std::lock_guard<std::shared_mutex> lockGuard(halClient->mConnectionLock);
    halClient->mContextHub = nullptr;
    halClient->mIsHalConnected = false;
  }
  LOGW("%s is disconnected from CHRE HAL. Reconnecting...",
       halClient->mClientName.c_str());

  HalError result = halClient->initConnection();
  uint64_t duration = ::android::elapsedRealtime() - startTime;
  if (result != HalError::SUCCESS) {
    LOGE("Failed to fully reconnect to CHRE HAL after %" PRIu64
         "ms, HalErrorCode: %" PRIi32,
         duration, result);
    return;
  }
  tryReconnectEndpoints(halClient);
  LOGI("%s is reconnected to CHRE HAL after %" PRIu64 "ms",
       halClient->mClientName.c_str(), duration);
}

ScopedAStatus HalClient::connectEndpoint(
    const HostEndpointInfo &hostEndpointInfo) {
  HostEndpointId endpointId = hostEndpointInfo.hostEndpointId;
  if (isEndpointConnected(endpointId)) {
    // Connecting the endpoint again even though it is already connected to let
    // HAL and/or CHRE be the single place to control the behavior.
    LOGW("Endpoint id %" PRIu16 " of %s is already connected", endpointId,
         mClientName.c_str());
  }
  ScopedAStatus result = callIfConnected(
      [&hostEndpointInfo](const std::shared_ptr<IContextHub> &hub) {
        return hub->onHostEndpointConnected(hostEndpointInfo);
      });
  if (result.isOk()) {
    insertConnectedEndpoint(hostEndpointInfo);
  } else {
    LOGE("Failed to connect endpoint id %" PRIu16 " of %s",
         hostEndpointInfo.hostEndpointId, mClientName.c_str());
  }
  return result;
}

ScopedAStatus HalClient::disconnectEndpoint(HostEndpointId hostEndpointId) {
  if (!isEndpointConnected(hostEndpointId)) {
    // Disconnecting the endpoint again even though it is already disconnected
    // to let HAL and/or CHRE be the single place to control the behavior.
    LOGW("Endpoint id %" PRIu16 " of %s is already disconnected",
         hostEndpointId, mClientName.c_str());
  }
  ScopedAStatus result = callIfConnected(
      [&hostEndpointId](const std::shared_ptr<IContextHub> &hub) {
        return hub->onHostEndpointDisconnected(hostEndpointId);
      });
  if (result.isOk()) {
    removeConnectedEndpoint(hostEndpointId);
  } else {
    LOGE("Failed to disconnect the endpoint id %" PRIu16 " of %s",
         hostEndpointId, mClientName.c_str());
  }
  return result;
}

ScopedAStatus HalClient::sendMessage(const ContextHubMessage &message) {
  uint16_t hostEndpointId = message.hostEndPoint;
  if (!isEndpointConnected(hostEndpointId)) {
    // This is still allowed now but in the future an error will be returned.
    LOGW("Endpoint id %" PRIu16
         " of %s is unknown or disconnected. Message sending will be skipped "
         "in the future",
         hostEndpointId, mClientName.c_str());
  }
  return callIfConnected([&](const std::shared_ptr<IContextHub> &hub) {
    return hub->sendMessageToHub(mContextHubId, message);
  });
}

void HalClient::tryReconnectEndpoints(HalClient *halClient) {
  LOGW("CHRE has restarted. Reconnecting endpoints of %s",
       halClient->mClientName.c_str());
  std::lock_guard<std::shared_mutex> lockGuard(
      halClient->mConnectedEndpointsLock);
  for (const auto &[endpointId, endpointInfo] :
       halClient->mConnectedEndpoints) {
    if (!halClient
             ->callIfConnected(
                 [&endpointInfo](const std::shared_ptr<IContextHub> &hub) {
                   return hub->onHostEndpointConnected(endpointInfo);
                 })
             .isOk()) {
      LOGE("Failed to set up the connected state for endpoint %" PRIu16
           " of %s after HAL restarts.",
           endpointId, halClient->mClientName.c_str());
      halClient->mConnectedEndpoints.erase(endpointId);
    } else {
      LOGI("Reconnected endpoint %" PRIu16 " of %s to CHRE HAL", endpointId,
           halClient->mClientName.c_str());
    }
  }
}

HalClient::~HalClient() {
  std::lock_guard<std::mutex> lock(mBackgroundConnectionFuturesLock);
  for (const auto &future : mBackgroundConnectionFutures) {
    // Calling std::thread.join() has chance to hang if the background thread
    // being joined is still waiting for connecting to the service. Therefore
    // waiting for the thread to finish here instead and logging the timeout
    // every second until system kills the process to report the abnormality.
    while (future.wait_for(std::chrono::seconds(1)) !=
           std::future_status::ready) {
      LOGE(
          "Failed to finish a background connection in time when HalClient is "
          "being destructed. Waiting...");
    }
  }
}
}  // namespace android::chre