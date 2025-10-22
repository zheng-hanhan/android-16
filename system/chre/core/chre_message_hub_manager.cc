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

#ifdef CHRE_MESSAGE_ROUTER_SUPPORT_ENABLED

#include "chre/core/chre_message_hub_manager.h"
#include "chre/core/event_loop.h"
#include "chre/core/event_loop_manager.h"
#include "chre/core/nanoapp.h"
#include "chre/platform/context.h"
#include "chre/platform/fatal_error.h"
#include "chre/target_platform/log.h"
#include "chre/util/conditional_lock_guard.h"
#include "chre/util/lock_guard.h"
#include "chre/util/nested_data_ptr.h"
#include "chre/util/system/event_callbacks.h"
#include "chre/util/system/message_common.h"
#include "chre/util/system/message_router.h"
#include "chre/util/system/service_helpers.h"
#include "chre/util/system/system_callback_type.h"
#include "chre/util/unique_ptr.h"
#include "chre_api/chre.h"

#include "pw_allocator/unique_ptr.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"

#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

using ::chre::message::Endpoint;
using ::chre::message::ENDPOINT_ID_ANY;
using ::chre::message::ENDPOINT_ID_INVALID;
using ::chre::message::EndpointId;
using ::chre::message::EndpointInfo;
using ::chre::message::EndpointType;
using ::chre::message::extractNanoappIdAndServiceId;
using ::chre::message::Message;
using ::chre::message::MESSAGE_HUB_ID_ANY;
using ::chre::message::MESSAGE_HUB_ID_INVALID;
using ::chre::message::MessageHubId;
using ::chre::message::MessageHubInfo;
using ::chre::message::MessageRouter;
using ::chre::message::MessageRouterSingleton;
using ::chre::message::Reason;
using ::chre::message::RpcFormat;
using ::chre::message::ServiceInfo;
using ::chre::message::Session;
using ::chre::message::SESSION_ID_INVALID;
using ::chre::message::SessionId;

namespace chre {

namespace {

//! Sends a ready event to the nanoapp with the given instance ID. If
//! serviceDescriptor is null, then the ready event is for an endpoint, else it
//! is for a service.
template <typename T>
void sendReadyEventToNanoapp(uint16_t nanoappInstanceId,
                             MessageHubId messageHubId, EndpointId endpointId,
                             const char *serviceDescriptor) {
  static_assert(std::is_same_v<T, chreMsgServiceReadyEvent> ||
                std::is_same_v<T, chreMsgEndpointReadyEvent>);

  UniquePtr<T> event = MakeUnique<T>();
  if (event.isNull()) {
    FATAL_ERROR_OOM();
    return;
  }

  event->hubId = messageHubId;
  event->endpointId = endpointId;
  if constexpr (std::is_same_v<T, chreMsgServiceReadyEvent>) {
    std::strncpy(event->serviceDescriptor, serviceDescriptor,
                 CHRE_MSG_MAX_SERVICE_DESCRIPTOR_LEN);
    event->serviceDescriptor[CHRE_MSG_MAX_SERVICE_DESCRIPTOR_LEN - 1] = '\0';
  }

  EventLoopManagerSingleton::get()->getEventLoop().postEventOrDie(
      std::is_same_v<T, chreMsgServiceReadyEvent>
          ? CHRE_EVENT_MSG_SERVICE_READY
          : CHRE_EVENT_MSG_ENDPOINT_READY,
      event.release(), freeEventDataCallback, nanoappInstanceId);
}

//! Sends a ready event to the nanoapp with the given instance ID. If
//! serviceDescriptor is null, then the ready event is for an endpoint, else it
//! is for a service.
void sendReadyEventToNanoapp(uint16_t nanoappInstanceId,
                             MessageHubId messageHubId, EndpointId endpointId,
                             const char *serviceDescriptor) {
  if (serviceDescriptor == nullptr) {
    sendReadyEventToNanoapp<chreMsgEndpointReadyEvent>(
        nanoappInstanceId, messageHubId, endpointId, serviceDescriptor);
  } else {
    sendReadyEventToNanoapp<chreMsgServiceReadyEvent>(
        nanoappInstanceId, messageHubId, endpointId, serviceDescriptor);
  }
}

}  // anonymous namespace

ChreMessageHubManager::ChreMessageHubManager()
    : mAllocator(ChreMessageHubManager::onMessageFreeCallback,
                 mFreeCallbackRecords, /* doEraseRecord= */ false) {}

ChreMessageHubManager::~ChreMessageHubManager() {
  mChreMessageHub.unregister();
  mChreMessageHubCallback->clearManager();
}

void ChreMessageHubManager::init() {
  ChreMessageHubCallback *callbackPtr =
      memoryAlloc<ChreMessageHubCallback>(*this);
  if (callbackPtr == nullptr) {
    FATAL_ERROR_OOM();
    return;
  }
  mChreMessageHubCallback =
      pw::IntrusivePtr<ChreMessageHubCallback>(callbackPtr);

  std::optional<MessageRouter::MessageHub> chreMessageHub =
      MessageRouterSingleton::get()->registerMessageHub(
          "CHRE", kChreMessageHubId, mChreMessageHubCallback);
  if (chreMessageHub.has_value()) {
    mChreMessageHub = std::move(*chreMessageHub);
  } else {
    FATAL_ERROR("Failed to register the CHRE MessageHub");
  }
}

bool ChreMessageHubManager::getEndpointInfo(MessageHubId hubId,
                                            EndpointId endpointId,
                                            chreMsgEndpointInfo &info) {
  std::optional<EndpointInfo> endpointInfo =
      MessageRouterSingleton::get()->getEndpointInfo(hubId, endpointId);
  if (!endpointInfo.has_value()) {
    return false;
  }

  info.hubId = hubId;
  info.endpointId = endpointId;
  info.type = toChreEndpointType(endpointInfo->type);
  info.version = endpointInfo->version;
  info.requiredPermissions = endpointInfo->requiredPermissions;
  // TODO(b/404241918): populate maxMessageSize from MessageRouter
  info.maxMessageSize = chreGetMessageToHostMaxSize();
  std::strncpy(info.name, endpointInfo->name, CHRE_MAX_ENDPOINT_NAME_LEN);
  info.name[CHRE_MAX_ENDPOINT_NAME_LEN - 1] = '\0';
  return true;
}

bool ChreMessageHubManager::configureReadyEvents(
    uint16_t nanoappInstanceId, EndpointId fromEndpointId, MessageHubId hubId,
    EndpointId endpointId, const char *serviceDescriptor, bool enable) {
  CHRE_ASSERT(inEventLoopThread());

  if (hubId == MESSAGE_HUB_ID_INVALID && endpointId == ENDPOINT_ID_INVALID &&
      serviceDescriptor == nullptr) {
    LOGE(
        "Invalid arguments to configureReadyEvents: hubId, endpointId and "
        "serviceDescriptor cannot all be invalid");
    return false;
  }

  if (!enable) {
    disableReadyEvents(fromEndpointId, hubId, endpointId, serviceDescriptor);
    return true;
  }

  if (!mEndpointReadyEventRequests.push_back(
          EndpointReadyEventData{.fromEndpointId = fromEndpointId,
                                 .messageHubId = hubId,
                                 .endpointId = endpointId,
                                 .serviceDescriptor = serviceDescriptor})) {
    LOG_OOM();
    return false;
  }

  std::optional<Endpoint> endpoint =
      searchForEndpoint(hubId, endpointId, serviceDescriptor);
  if (endpoint.has_value()) {
    sendReadyEventToNanoapp(nanoappInstanceId, endpoint->messageHubId,
                            endpoint->endpointId, serviceDescriptor);
  }
  return true;
}

bool ChreMessageHubManager::getSessionInfo(EndpointId fromEndpointId,
                                           SessionId sessionId,
                                           chreMsgSessionInfo &info) {
  std::optional<Session> session = mChreMessageHub.getSessionWithId(sessionId);
  if (!session.has_value()) {
    return false;
  }

  bool initiatorIsNanoapp =
      session->initiator.messageHubId == kChreMessageHubId &&
      session->initiator.endpointId == fromEndpointId;
  bool peerIsNanoapp = session->peer.messageHubId == kChreMessageHubId &&
                       session->peer.endpointId == fromEndpointId;
  if (!initiatorIsNanoapp && !peerIsNanoapp) {
    LOGE("Nanoapp with ID 0x%" PRIx64
         " is not the initiator or peer of session with ID %" PRIu16,
         fromEndpointId, sessionId);
    return false;
  }

  info.hubId = initiatorIsNanoapp ? session->peer.messageHubId
                                  : session->initiator.messageHubId;
  info.endpointId = initiatorIsNanoapp ? session->peer.endpointId
                                       : session->initiator.endpointId;

  if (session->hasServiceDescriptor) {
    std::strncpy(info.serviceDescriptor, session->serviceDescriptor,
                 CHRE_MSG_MAX_SERVICE_DESCRIPTOR_LEN);
    info.serviceDescriptor[CHRE_MSG_MAX_SERVICE_DESCRIPTOR_LEN - 1] = '\0';
  } else {
    info.serviceDescriptor[0] = '\0';
  }

  info.sessionId = sessionId;
  info.reason = chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_UNSPECIFIED;
  return true;
}

bool ChreMessageHubManager::openSessionAsync(EndpointId fromEndpointId,
                                             MessageHubId toHubId,
                                             EndpointId toEndpointId,
                                             const char *serviceDescriptor) {
  SessionId sessionId = EventLoopManagerSingleton::get()
                            ->getChreMessageHubManager()
                            .getMessageHub()
                            .openSession(fromEndpointId, toHubId, toEndpointId,
                                         serviceDescriptor);
  return sessionId != SESSION_ID_INVALID;
}

bool ChreMessageHubManager::openDefaultSessionAsync(
    EndpointId fromEndpointId, MessageHubId toHubId, EndpointId toEndpointId,
    const char *serviceDescriptor) {
  std::optional<Endpoint> endpoint =
      searchForEndpoint(toHubId, toEndpointId, serviceDescriptor);
  return endpoint.has_value() &&
         openSessionAsync(fromEndpointId, endpoint->messageHubId,
                          endpoint->endpointId, serviceDescriptor);
}

bool ChreMessageHubManager::closeSession(EndpointId fromEndpointId,
                                         SessionId sessionId) {
  std::optional<Session> session = mChreMessageHub.getSessionWithId(sessionId);
  if (!session.has_value()) {
    LOGE("Failed to close session with ID %" PRIu16 ": session not found",
         sessionId);
    return false;
  }

  Endpoint nanoapp(kChreMessageHubId, fromEndpointId);
  if (session->initiator != nanoapp && session->peer != nanoapp) {
    LOGE("Nanoapp with ID 0x%" PRIx64
         " is not the initiator or peer of session with ID %" PRIu16,
         fromEndpointId, sessionId);
    return false;
  }
  return mChreMessageHub.closeSession(sessionId);
}

bool ChreMessageHubManager::sendMessage(void *message, size_t messageSize,
                                        uint32_t messageType,
                                        uint16_t sessionId,
                                        uint32_t messagePermissions,
                                        chreMessageFreeFunction *freeCallback,
                                        EndpointId fromEndpointId) {
  bool success = false;
  if ((message == nullptr) != (freeCallback == nullptr)) {
    // We don't allow this because a null callback with non-null message is
    // susceptible to bugs where the nanoapp modifies the data while it is still
    // being used by the system, and a non-null callback with null message is
    // not meaningful since there is no data to release and we make no
    // guarantees about when the callback is invoked.
    LOGE("Mixing null and non-null message and free callback is not allowed");
  } else {
    pw::UniquePtr<std::byte[]> messageData =
        mAllocator.MakeUniqueArrayWithCallback(
            reinterpret_cast<std::byte *>(message), messageSize,
            MessageFreeCallbackData{.freeCallback = freeCallback,
                                    .nanoappId = fromEndpointId});
    if (messageData == nullptr) {
      LOG_OOM();
    } else {
      success = mChreMessageHub.sendMessage(std::move(messageData), messageType,
                                            messagePermissions, sessionId,
                                            fromEndpointId);
    }
  }

  if (!success && freeCallback != nullptr) {
    freeCallback(message, messageSize);
  }
  return success;
}

bool ChreMessageHubManager::publishServices(
    EndpointId fromEndpointId, const chreMsgServiceInfo *serviceInfos,
    size_t numServices) {
  CHRE_ASSERT(inEventLoopThread());

  LockGuard<Mutex> lockGuard(mNanoappPublishedServicesMutex);
  if (!validateServicesLocked(fromEndpointId, serviceInfos, numServices)) {
    return false;
  }

  if (!mNanoappPublishedServices.reserve(mNanoappPublishedServices.size() +
                                         numServices)) {
    LOG_OOM();
    return false;
  }

  for (size_t i = 0; i < numServices; ++i) {
    // Cannot fail as we reserved space for the push above
    mNanoappPublishedServices.push_back(NanoappServiceData{
        .nanoappId = fromEndpointId, .serviceInfo = serviceInfos[i]});
  }
  return true;
}

void ChreMessageHubManager::unregisterEndpoint(EndpointId endpointId) {
  UniquePtr<EndpointId> endpointIdPtr = MakeUnique<EndpointId>(endpointId);
  if (endpointIdPtr.isNull()) {
    FATAL_ERROR_OOM();
    return;
  }

  EventLoopManagerSingleton::get()->deferCallback(
      SystemCallbackType::EndpointCleanupNanoappEvent, std::move(endpointIdPtr),
      [](SystemCallbackType /* type */, UniquePtr<EndpointId> &&endpointId) {
        EventLoopManagerSingleton::get()
            ->getChreMessageHubManager()
            .cleanupEndpointResources(*endpointId);
      });

  mChreMessageHub.unregisterEndpoint(endpointId);
}

void ChreMessageHubManager::cleanupEndpointResources(EndpointId endpointId) {
  CHRE_ASSERT(inEventLoopThread());

  {
    LockGuard<Mutex> lockGuard(mNanoappPublishedServicesMutex);
    for (size_t i = 0; i < mNanoappPublishedServices.size();) {
      if (mNanoappPublishedServices[i].nanoappId == endpointId) {
        mNanoappPublishedServices.erase(i);
      } else {
        ++i;
      }
    }
  }

  for (size_t i = 0; i < mEndpointReadyEventRequests.size(); ++i) {
    if (mEndpointReadyEventRequests[i].fromEndpointId == endpointId) {
      mEndpointReadyEventRequests.erase(i);
    } else {
      ++i;
    }
  }
}

chreMsgEndpointType ChreMessageHubManager::toChreEndpointType(
    EndpointType type) {
  switch (type) {
    case EndpointType::HOST_FRAMEWORK:
      return chreMsgEndpointType::CHRE_MSG_ENDPOINT_TYPE_HOST_FRAMEWORK;
    case EndpointType::HOST_APP:
      return chreMsgEndpointType::CHRE_MSG_ENDPOINT_TYPE_HOST_APP;
    case EndpointType::HOST_NATIVE:
      return chreMsgEndpointType::CHRE_MSG_ENDPOINT_TYPE_HOST_NATIVE;
    case EndpointType::NANOAPP:
      return chreMsgEndpointType::CHRE_MSG_ENDPOINT_TYPE_NANOAPP;
    case EndpointType::GENERIC:
      return chreMsgEndpointType::CHRE_MSG_ENDPOINT_TYPE_GENERIC;
    default:
      LOGE("Unknown endpoint type: %" PRIu8, type);
      return chreMsgEndpointType::CHRE_MSG_ENDPOINT_TYPE_INVALID;
  }
}

chreMsgEndpointReason ChreMessageHubManager::toChreEndpointReason(
    Reason reason) {
  switch (reason) {
    case Reason::UNSPECIFIED:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_UNSPECIFIED;
    case Reason::OUT_OF_MEMORY:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_OUT_OF_MEMORY;
    case Reason::TIMEOUT:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_TIMEOUT;
    case Reason::OPEN_ENDPOINT_SESSION_REQUEST_REJECTED:
      return chreMsgEndpointReason::
          CHRE_MSG_ENDPOINT_REASON_OPEN_ENDPOINT_SESSION_REQUEST_REJECTED;
    case Reason::CLOSE_ENDPOINT_SESSION_REQUESTED:
      return chreMsgEndpointReason::
          CHRE_MSG_ENDPOINT_REASON_CLOSE_ENDPOINT_SESSION_REQUESTED;
    case Reason::ENDPOINT_INVALID:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_ENDPOINT_INVALID;
    case Reason::ENDPOINT_GONE:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_ENDPOINT_GONE;
    case Reason::ENDPOINT_CRASHED:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_ENDPOINT_CRASHED;
    case Reason::HUB_RESET:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_HUB_RESET;
    case Reason::PERMISSION_DENIED:
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_PERMISSION_DENIED;
    default:
      LOGE("Unknown endpoint reason: %" PRIu8, reason);
      return chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_UNSPECIFIED;
  }
}

void ChreMessageHubManager::onMessageToNanoappCallback(
    SystemCallbackType /* type */, UniquePtr<MessageCallbackData> &&data) {
  bool success = false;
  Nanoapp *nanoapp =
      EventLoopManagerSingleton::get()->getEventLoop().findNanoappByAppId(
          data->nanoappId);
  uint32_t messagePermissions = data->messageToNanoapp.messagePermissions;
  if (nanoapp == nullptr) {
    LOGE("Unable to find nanoapp with ID 0x%" PRIx64
         " to receive message with type %" PRIu32 " and permissions %" PRIu32
         " with session ID %" PRIu16,
         data->nanoappId, data->messageToNanoapp.messageType,
         data->messageToNanoapp.messagePermissions,
         data->messageToNanoapp.sessionId);
  } else if (!nanoapp->hasPermissions(messagePermissions)) {
    LOGE("nanoapp with ID 0x%" PRIx64
         " does not have permissions to receive "
         "message with type %" PRIu32 " and permissions 0x%" PRIx32,
         nanoapp->getAppId(), data->messageToNanoapp.messageType,
         data->messageToNanoapp.messagePermissions);
  } else if (!EventLoopManagerSingleton::get()
                  ->getEventLoop()
                  .distributeEventSync(CHRE_EVENT_MSG_FROM_ENDPOINT,
                                       &data->messageToNanoapp,
                                       nanoapp->getInstanceId())) {
    LOGE("Unable to distribute message to nanoapp with ID 0x%" PRIx64,
         nanoapp->getAppId());
  } else {
    success = true;
  }

  // Close session on failure so sender knows there was an issue
  if (!success) {
    EventLoopManagerSingleton::get()
        ->getChreMessageHubManager()
        .getMessageHub()
        .closeSession(data->messageToNanoapp.sessionId);
  }
}

void ChreMessageHubManager::onSessionStateChangedCallback(
    SystemCallbackType /* type */, UniquePtr<SessionCallbackData> &&data) {
  Nanoapp *nanoapp =
      EventLoopManagerSingleton::get()->getEventLoop().findNanoappByAppId(
          data->nanoappId);
  if (nanoapp == nullptr) {
    LOGE("Unable to find nanoapp with ID 0x%" PRIx64
         " to close the session with ID %" PRIu16,
         data->nanoappId, data->sessionData.sessionId);
    return;
  }

  bool success =
      EventLoopManagerSingleton::get()->getEventLoop().distributeEventSync(
          data->isClosed ? CHRE_EVENT_MSG_SESSION_CLOSED
                         : CHRE_EVENT_MSG_SESSION_OPENED,
          &data->sessionData, nanoapp->getInstanceId());
  if (!success) {
    LOGE("Unable to process session closed event to nanoapp with ID 0x%" PRIx64,
         nanoapp->getAppId());
  }
}

void ChreMessageHubManager::onSessionOpenCompleteCallback(
    uint16_t /* type */, void *data, void * /* extraData */) {
  NestedDataPtr<SessionId> sessionId(data);
  EventLoopManagerSingleton::get()
      ->getChreMessageHubManager()
      .getMessageHub()
      .onSessionOpenComplete(sessionId);
}

void ChreMessageHubManager::onMessageFreeCallback(
    std::byte *message, size_t /* length */,
    MessageFreeCallbackData && /* callbackData */) {
  EventLoopManagerSingleton::get()->deferCallback(
      SystemCallbackType::EndpointMessageFreeEvent, message,
      ChreMessageHubManager::handleMessageFreeCallback);
}

void ChreMessageHubManager::handleMessageFreeCallback(uint16_t /* type */,
                                                      void *data,
                                                      void * /* extraData */) {
  std::optional<CallbackAllocator<MessageFreeCallbackData>::CallbackRecord>
      record = EventLoopManagerSingleton::get()
                   ->getChreMessageHubManager()
                   .getAndRemoveFreeCallbackRecord(data);
  if (!record.has_value()) {
    LOGE("Unable to find free callback record for message with message: %p",
         data);
    return;
  }

  if (record->metadata.freeCallback == nullptr) {
    return;
  }

  EventLoopManagerSingleton::get()->getEventLoop().invokeMessageFreeFunction(
      record->metadata.nanoappId, record->metadata.freeCallback,
      record->message, record->messageSize);
}

void ChreMessageHubManager::onSessionStateChanged(
    const Session &session, std::optional<Reason> reason) {
  for (const Endpoint &endpoint : {session.initiator, session.peer}) {
    if (endpoint.messageHubId != kChreMessageHubId) {
      continue;
    }

    auto sessionCallbackData = MakeUnique<SessionCallbackData>();
    if (sessionCallbackData.isNull()) {
      FATAL_ERROR_OOM();
      return;
    }

    const Endpoint &otherParty =
        session.initiator == endpoint ? session.peer : session.initiator;
    uint64_t nanoappId = endpoint.endpointId;
    sessionCallbackData->nanoappId = nanoappId;
    sessionCallbackData->isClosed = reason.has_value();
    sessionCallbackData->sessionData = {
        .hubId = otherParty.messageHubId,
        .endpointId = otherParty.endpointId,
        .sessionId = session.sessionId,
    };
    sessionCallbackData->sessionData.reason =
        reason.has_value()
            ? toChreEndpointReason(*reason)
            : chreMsgEndpointReason::CHRE_MSG_ENDPOINT_REASON_UNSPECIFIED;
    if (session.serviceDescriptor[0] != '\0') {
      std::strncpy(sessionCallbackData->sessionData.serviceDescriptor,
                   session.serviceDescriptor,
                   CHRE_MSG_MAX_SERVICE_DESCRIPTOR_LEN);
      sessionCallbackData->sessionData
          .serviceDescriptor[CHRE_MSG_MAX_SERVICE_DESCRIPTOR_LEN - 1] = '\0';
    } else {
      sessionCallbackData->sessionData.serviceDescriptor[0] = '\0';
    }

    EventLoopManagerSingleton::get()->deferCallback(
        SystemCallbackType::EndpointSessionStateChangedEvent,
        std::move(sessionCallbackData),
        ChreMessageHubManager::onSessionStateChangedCallback);

    if (session.initiator == session.peer) {
      // Session between self - only deliver one event
      return;
    }
  }
}

//! Called when a session open is requested.
void ChreMessageHubManager::onSessionOpenComplete(
    message::SessionId sessionId) {
  EventLoopManagerSingleton::get()->deferCallback(
      SystemCallbackType::EndpointSessionRequestedEvent,
      NestedDataPtr<SessionId>(sessionId),
      ChreMessageHubManager::onSessionOpenCompleteCallback);
}

void ChreMessageHubManager::onEndpointReadyEvent(MessageHubId messageHubId,
                                                 EndpointId endpointId) {
  CHRE_ASSERT(inEventLoopThread());

  for (size_t i = 0; i < mEndpointReadyEventRequests.size(); ++i) {
    EndpointReadyEventData &data = mEndpointReadyEventRequests[i];
    bool messageHubIdMatches = data.messageHubId == MESSAGE_HUB_ID_ANY ||
                               data.messageHubId == messageHubId;
    bool endpointIdMatches =
        data.endpointId == ENDPOINT_ID_ANY || data.endpointId == endpointId;
    if (messageHubIdMatches && endpointIdMatches) {
      Nanoapp *nanoapp =
          EventLoopManagerSingleton::get()->getEventLoop().findNanoappByAppId(
              data.fromEndpointId);
      if (nanoapp == nullptr) {
        LOGW("Could not find nanoapp with ID 0x%" PRIx64 " to send ready event",
             data.fromEndpointId);
        continue;
      }

      if (data.serviceDescriptor == nullptr ||
          MessageRouterSingleton::get()->doesEndpointHaveService(
              messageHubId, endpointId, data.serviceDescriptor)) {
        sendReadyEventToNanoapp(nanoapp->getInstanceId(), messageHubId,
                                endpointId, data.serviceDescriptor);
      }
    }
  }
}

MessageHubId ChreMessageHubManager::findDefaultMessageHubId(
    EndpointId endpointId) {
  struct SearchContext {
    MessageHubId toMessageHubId = MESSAGE_HUB_ID_INVALID;
    EndpointId toEndpointId;
  };
  SearchContext context = {
      .toEndpointId = endpointId,
  };

  MessageRouterSingleton::get()->forEachEndpoint(
      [&context](const MessageHubInfo &hubInfo,
                 const EndpointInfo &endpointInfo) {
        if (context.toMessageHubId == MESSAGE_HUB_ID_INVALID &&
            endpointInfo.id == context.toEndpointId) {
          context.toMessageHubId = hubInfo.id;
        }
      });
  return context.toMessageHubId;
}

bool ChreMessageHubManager::doesNanoappHaveLegacyService(uint64_t nanoappId,
                                                         uint64_t serviceId) {
  struct SearchContext {
    uint64_t nanoappId;
    uint64_t serviceId;
    bool found;
  };
  SearchContext context = {
      .nanoappId = nanoappId,
      .serviceId = serviceId,
      .found = false,
  };

  EventLoopManagerSingleton::get()->getEventLoop().forEachNanoapp(
      [](const Nanoapp *nanoapp, void *data) {
        SearchContext *context = static_cast<SearchContext *>(data);
        if (!context->found && nanoapp->getAppId() == context->nanoappId) {
          context->found = nanoapp->hasRpcService(context->serviceId);
        }
      },
      &context);
  return context.found;
}

bool ChreMessageHubManager::validateServicesLocked(
    uint64_t nanoappId, const chreMsgServiceInfo *serviceInfos,
    size_t numServices) {
  if (serviceInfos == nullptr || numServices == 0) {
    LOGE("Failed to publish service for nanoapp with ID 0x%" PRIx64
         ": serviceInfos is null or numServices is 0",
         nanoappId);
    return false;
  }

  for (size_t i = 0; i < numServices; ++i) {
    const chreMsgServiceInfo &serviceInfo = serviceInfos[i];

    if (serviceInfo.serviceDescriptor == nullptr ||
        serviceInfo.serviceDescriptor[0] == '\0') {
      LOGE("Failed to publish service for nanoapp with ID 0x%" PRIx64
           ": service descriptor is null or empty",
           nanoappId);
      return false;
    }

    uint64_t unused;
    if (extractNanoappIdAndServiceId(serviceInfo.serviceDescriptor, unused,
                                     unused)) {
      LOGE("Failed to publish service for nanoapp with ID 0x%" PRIx64
           ": service descriptor is in the legacy format",
           nanoappId);
      return false;
    }

    for (const NanoappServiceData &service : mNanoappPublishedServices) {
      if (std::strcmp(service.serviceInfo.serviceDescriptor,
                      serviceInfo.serviceDescriptor) == 0) {
        LOGE("Failed to publish service for nanoapp with ID 0x%" PRIx64
             ": service descriptor: %s is already published by another "
             "nanoapp",
             nanoappId, service.serviceInfo.serviceDescriptor);
        return false;
      }
    }

    for (size_t j = i + 1; j < numServices; ++j) {
      if (std::strcmp(serviceInfo.serviceDescriptor,
                      serviceInfos[j].serviceDescriptor)) {
        LOGE("Failed to publish service for nanoapp with ID 0x%" PRIx64
             ": service descriptor: %s repeats in list of services to publish",
             nanoappId, serviceInfo.serviceDescriptor);
        return false;
      }
    }
  }
  return true;
}

std::optional<Endpoint> ChreMessageHubManager::searchForEndpoint(
    MessageHubId messageHubId, EndpointId endpointId,
    const char *serviceDescriptor) {
  if (endpointId == ENDPOINT_ID_INVALID) {
    if (serviceDescriptor == nullptr) {
      LOGD(
          "Failed to search for an endpoint: no endpoint ID or service "
          "descriptor");
      return std::nullopt;
    }
    return MessageRouterSingleton::get()->getEndpointForService(
        messageHubId, serviceDescriptor);
  }

  if (serviceDescriptor != nullptr) {
    if (messageHubId == MESSAGE_HUB_ID_INVALID) {
      LOGD(
          "Failed to search for an endpoint: no message hub ID provided with "
          "endpoint and service descriptor");
      return std::nullopt;
    }

    if (!MessageRouterSingleton::get()->doesEndpointHaveService(
            messageHubId, endpointId, serviceDescriptor)) {
      LOGD("Failed to search for an endpoint: endpoint 0x%" PRIx64
           " on hub 0x%" PRIx64 " does not have service %s",
           messageHubId, endpointId, serviceDescriptor);
      return std::nullopt;
    }
    return Endpoint(messageHubId, endpointId);
  }

  if (messageHubId == MESSAGE_HUB_ID_INVALID) {
    messageHubId = findDefaultMessageHubId(endpointId);
    if (messageHubId == MESSAGE_HUB_ID_INVALID) {
      LOGD(
          "Failed to search for an endpoint: no default message hub ID "
          "found");
      return std::nullopt;
    }
  } else if (!MessageRouterSingleton::get()
                  ->getEndpointInfo(messageHubId, endpointId)
                  .has_value()) {
    LOGD("Failed to search for an endpoint: endpoint 0x%" PRIx64
         " on hub 0x%" PRIx64 " does not exist",
         messageHubId, endpointId);
    return std::nullopt;
  }
  return Endpoint(messageHubId, endpointId);
}

void ChreMessageHubManager::disableReadyEvents(EndpointId fromEndpointId,
                                               MessageHubId hubId,
                                               EndpointId endpointId,
                                               const char *serviceDescriptor) {
  for (size_t i = 0; i < mEndpointReadyEventRequests.size(); ++i) {
    EndpointReadyEventData &request = mEndpointReadyEventRequests[i];
    if (request.fromEndpointId == fromEndpointId &&
        request.messageHubId == hubId && request.endpointId == endpointId) {
      bool servicesAreNull =
          request.serviceDescriptor == nullptr && serviceDescriptor == nullptr;
      bool servicesAreSame =
          request.serviceDescriptor != nullptr &&
          serviceDescriptor != nullptr &&
          std::strcmp(request.serviceDescriptor, serviceDescriptor) == 0;
      if (servicesAreNull || servicesAreSame) {
        mEndpointReadyEventRequests.erase(i);
        break;
      }
    }
  }
}

RpcFormat ChreMessageHubManager::toMessageRpcFormat(
    chreMsgEndpointServiceFormat format) {
  switch (format) {
    case chreMsgEndpointServiceFormat::CHRE_MSG_ENDPOINT_SERVICE_FORMAT_AIDL:
      return RpcFormat::AIDL;
    case chreMsgEndpointServiceFormat::
        CHRE_MSG_ENDPOINT_SERVICE_FORMAT_PW_RPC_PROTOBUF:
      return RpcFormat::PW_RPC_PROTOBUF;
    default:
      return RpcFormat::CUSTOM;
  }
}

void ChreMessageHubManager::ChreMessageHubCallback::clearManager() {
  LockGuard<Mutex> managerLock(mManagerLock);
  mChreMessageHubManager = nullptr;
}

bool ChreMessageHubManager::ChreMessageHubCallback::onMessageReceived(
    pw::UniquePtr<std::byte[]> &&data, uint32_t messageType,
    uint32_t messagePermissions, const Session &session,
    bool sentBySessionInitiator) {
  Endpoint receiver = sentBySessionInitiator ? session.peer : session.initiator;
  auto messageCallbackData = MakeUnique<MessageCallbackData>();
  if (messageCallbackData.isNull()) {
    LOG_OOM();
    return false;
  }

  messageCallbackData->messageToNanoapp = {
      .messageType = messageType,
      .messagePermissions = messagePermissions,
      .message = data.get(),
      .messageSize = data.size(),
      .sessionId = session.sessionId,
  };
  messageCallbackData->data = std::move(data);
  messageCallbackData->nanoappId = receiver.endpointId;

  return EventLoopManagerSingleton::get()->deferCallback(
      SystemCallbackType::EndpointMessageToNanoappEvent,
      std::move(messageCallbackData),
      ChreMessageHubManager::onMessageToNanoappCallback);
}

void ChreMessageHubManager::ChreMessageHubCallback::onSessionOpenRequest(
    const Session &session) {
  LockGuard<Mutex> managerLock(mManagerLock);
  if (mChreMessageHubManager == nullptr) {
    LOGW("The ChreMessageHubManager has been destroyed.");
    return;
  }

  mChreMessageHubManager->onSessionOpenComplete(session.sessionId);
}

void ChreMessageHubManager::ChreMessageHubCallback::onSessionOpened(
    const Session &session) {
  LockGuard<Mutex> managerLock(mManagerLock);
  if (mChreMessageHubManager == nullptr) {
    LOGW("The ChreMessageHubManager has been destroyed.");
    return;
  }

  mChreMessageHubManager->onSessionStateChanged(session,
                                                /* reason= */ std::nullopt);
}

void ChreMessageHubManager::ChreMessageHubCallback::onSessionClosed(
    const Session &session, Reason reason) {
  LockGuard<Mutex> managerLock(mManagerLock);
  if (mChreMessageHubManager == nullptr) {
    LOGW("The ChreMessageHubManager has been destroyed.");
    return;
  }

  mChreMessageHubManager->onSessionStateChanged(session, reason);
}

void ChreMessageHubManager::ChreMessageHubCallback::forEachEndpoint(
    const pw::Function<bool(const EndpointInfo &)> &function) {
  EventLoopManagerSingleton::get()->getEventLoop().onMatchingNanoappEndpoint(
      function);
}

std::optional<EndpointInfo>
ChreMessageHubManager::ChreMessageHubCallback::getEndpointInfo(
    EndpointId endpointId) {
  return EventLoopManagerSingleton::get()->getEventLoop().getEndpointInfo(
      endpointId);
}

std::optional<EndpointId>
ChreMessageHubManager::ChreMessageHubCallback::getEndpointForService(
    const char *serviceDescriptor) {
  LockGuard<Mutex> managerLock(mManagerLock);
  if (mChreMessageHubManager == nullptr) {
    LOGW("The ChreMessageHubManager has been destroyed.");
    return std::nullopt;
  }

  if (serviceDescriptor == nullptr || serviceDescriptor[0] == '\0') {
    return std::nullopt;
  }

  {
    ConditionalLockGuard<Mutex> lockGuard(
        mChreMessageHubManager->mNanoappPublishedServicesMutex,
        !inEventLoopThread());
    for (const NanoappServiceData &service :
         mChreMessageHubManager->mNanoappPublishedServices) {
      if (std::strcmp(serviceDescriptor,
                      service.serviceInfo.serviceDescriptor) == 0) {
        return service.nanoappId;
      }
    }
  }

  // Check for the legacy service format
  uint64_t nanoappId;
  uint64_t serviceId;
  return extractNanoappIdAndServiceId(serviceDescriptor, nanoappId,
                                      serviceId) &&
                 mChreMessageHubManager->doesNanoappHaveLegacyService(nanoappId,
                                                                      serviceId)
             ? std::make_optional(nanoappId)
             : std::nullopt;
}

bool ChreMessageHubManager::ChreMessageHubCallback::doesEndpointHaveService(
    EndpointId endpointId, const char *serviceDescriptor) {
  // Endpoints are unique, so if we find it, then the endpoint has the service
  // if and only if the endpoint ID matches the endpoint ID we are looking for
  std::optional<EndpointId> endpoint = getEndpointForService(serviceDescriptor);
  return endpoint.has_value() && endpoint.value() == endpointId;
}

void ChreMessageHubManager::ChreMessageHubCallback::forEachService(
    const pw::Function<bool(const EndpointInfo &, const ServiceInfo &)>
        &function) {
  LockGuard<Mutex> managerLock(mManagerLock);
  if (mChreMessageHubManager == nullptr) {
    LOGW("The ChreMessageHubManager has been destroyed.");
    return;
  }

  {
    ConditionalLockGuard<Mutex> lockGuard(
        mChreMessageHubManager->mNanoappPublishedServicesMutex,
        !inEventLoopThread());
    for (const NanoappServiceData &service :
         mChreMessageHubManager->mNanoappPublishedServices) {
      std::optional<EndpointInfo> endpointInfo =
          EventLoopManagerSingleton::get()->getEventLoop().getEndpointInfo(
              service.nanoappId);
      if (endpointInfo.has_value()) {
        ServiceInfo serviceInfo(service.serviceInfo.serviceDescriptor,
                                service.serviceInfo.majorVersion,
                                service.serviceInfo.minorVersion,
                                mChreMessageHubManager->toMessageRpcFormat(
                                    static_cast<chreMsgEndpointServiceFormat>(
                                        service.serviceInfo.serviceFormat)));
        if (function(endpointInfo.value(), serviceInfo)) {
          return;
        }
      }
    }
  }

  EventLoopManagerSingleton::get()->getEventLoop().onMatchingNanoappService(
      function);
}

void ChreMessageHubManager::ChreMessageHubCallback::onHubRegistered(
    const MessageHubInfo & /*info*/) {
  // We don't depend on this notification.
}

void ChreMessageHubManager::ChreMessageHubCallback::onHubUnregistered(
    MessageHubId /*id*/) {
  // We don't depend on this notification.
}

void ChreMessageHubManager::ChreMessageHubCallback::onEndpointRegistered(
    MessageHubId messageHubId, EndpointId endpointId) {
  if (messageHubId == MESSAGE_HUB_ID_INVALID ||
      endpointId == ENDPOINT_ID_INVALID) {
    LOGE(
        "Invalid input to onEndpointRegistered: %s %s",
        messageHubId == MESSAGE_HUB_ID_INVALID ? "messageHubId is invalid" : "",
        endpointId == ENDPOINT_ID_INVALID ? "endpointId is invalid" : "");
    return;
  }

  UniquePtr<Endpoint> endpoint = MakeUnique<Endpoint>(messageHubId, endpointId);
  if (endpoint.isNull()) {
    FATAL_ERROR_OOM();
    return;
  }

  // We defer here to do all processing in the event loop thread. This allows
  // for no locks as well as fast callbacks due to the potentially large
  // number of nanoapps that may be waiting for events generated by this
  // callback.
  EventLoopManagerSingleton::get()->deferCallback(
      SystemCallbackType::EndpointRegisteredEvent, std::move(endpoint),
      [](SystemCallbackType /* type */, UniquePtr<Endpoint> &&data) {
        EventLoopManagerSingleton::get()
            ->getChreMessageHubManager()
            .onEndpointReadyEvent(data->messageHubId, data->endpointId);
      });
}

void ChreMessageHubManager::ChreMessageHubCallback::onEndpointUnregistered(
    MessageHubId /* messageHubId */, EndpointId /* endpointId */) {
  // Ignore - we only care about registered endpoints
}

void ChreMessageHubManager::ChreMessageHubCallback::pw_recycle() {
  memoryFreeAndDestroy(this);
}

}  // namespace chre

#endif  // CHRE_MESSAGE_ROUTER_SUPPORT_ENABLED
