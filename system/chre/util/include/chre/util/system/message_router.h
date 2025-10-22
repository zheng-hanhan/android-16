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

#ifndef CHRE_UTIL_SYSTEM_MESSAGE_ROUTER_H_
#define CHRE_UTIL_SYSTEM_MESSAGE_ROUTER_H_

#include "chre/platform/mutex.h"
#include "chre/util/dynamic_vector.h"
#include "chre/util/memory.h"
#include "chre/util/singleton.h"
#include "chre/util/system/intrusive_ref_base.h"
#include "chre/util/system/message_common.h"

#include "pw_allocator/unique_ptr.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_intrusive_ptr/recyclable.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace chre::message {

//! MessageRouter routes messages between endpoints connected to MessageHubs. It
//! provides an API for registering MessageHubs, opening and closing sessions,
//! and sending messages between endpoints. Each MessageHub is expected to
//! register a callback to handle messages sent to its endpoints and other
//! functions to provide information about the endpoints connected to it.
//!
//! MessageRouter is thread-safe.
//!
//! Usage:
//! 1. Create a MessageRouter instance.
//! 2. Register MessageHubs with the MessageRouter. Each MessageHub must have
//!    a unique ID and a callback to handle messages sent to its endpoints.
//! 3. Open sessions from endpoints connected to MessageHubs to endpoints
//!    connected to other MessageHubs.
//! 4. Send messages to endpoints using the MessageRouter API.
//! 5. Close sessions when they are no longer needed.
class MessageRouter {
 public:
  //! The callback used to register a MessageHub with the MessageRouter
  class MessageHubCallback : public IntrusiveRefBase,
                             public pw::Recyclable<MessageHubCallback> {
   public:
    virtual ~MessageHubCallback() = default;

    //! Message processing callback. If this function returns true,
    //! the MessageHub received the message and will deliver it to the
    //! receiving endpoint, or close the session if an error occurs.
    //! @see sendMessage
    //! @param session The session that the message was sent on (this reference
    //!                is only valid for the duration of the callback)
    //! @param sentBySessionInitiator Whether the message was sent by the
    //! initiator of the session
    //! @return true if the message was accepted for processing
    virtual bool onMessageReceived(pw::UniquePtr<std::byte[]> &&data,
                                   uint32_t messageType,
                                   uint32_t messagePermissions,
                                   const Session &session,
                                   bool sentBySessionInitiator) = 0;

    //! Callback called when a session has been requested to be opened. The
    //! message hub should call onSessionOpenComplete or closeSession to
    //! accept or reject the session, respectively.
    //! This function is called before returning from openSession in the
    //! requestor hub.
    virtual void onSessionOpenRequest(const Session &session) = 0;

    //! Callback called when the peer message hub has accepted the session
    //! and the session is now open for messages
    virtual void onSessionOpened(const Session &session) = 0;

    //! Callback called when the session is closed
    virtual void onSessionClosed(const Session &session, Reason reason) = 0;

    //! Callback called to iterate over all endpoints connected to the
    //! MessageHub. Underlying endpoint storage must not change during this
    //! callback. If function returns true, the MessageHub can stop iterating
    //! over future endpoints.
    virtual void forEachEndpoint(
        const pw::Function<bool(const EndpointInfo &)> &function) = 0;

    //! @return The EndpointInfo for the given endpoint ID.
    virtual std::optional<EndpointInfo> getEndpointInfo(
        EndpointId endpointId) = 0;

    //! @return The first endpoint that has the given service descriptor, a
    //! null-terminated ASCII string. If no endpoint has the service descriptor,
    //! std::nullopt is returned.
    virtual std::optional<EndpointId> getEndpointForService(
        const char *serviceDescriptor) = 0;

    //! @return true if the endpoint has the given service descriptor, a
    //! null-terminated ASCII string, false otherwise.
    virtual bool doesEndpointHaveService(EndpointId endpointId,
                                         const char *serviceDescriptor) = 0;

    //! Callback called to iterate over all services provided by endpoints
    //! connected to the MessageHub. Underlying endpoint and service storage
    //! must not change during this callback. If function returns true, the
    //! MessageHub can stop iterating over future endpoints. The service
    //! descriptor must be valid for the duration of the callback.
    virtual void forEachService(
        const pw::Function<bool(const EndpointInfo &, const ServiceInfo &)>
            &function) = 0;

    //! Callback called when a message hub except this one is registered.
    virtual void onHubRegistered(const MessageHubInfo &info) = 0;

    //! Callback called when a message hub except this one is unregistered.
    virtual void onHubUnregistered(MessageHubId id) = 0;

    //! Callback called when an endpoint is registered to any MessageHub,
    //! except for this MessageHub.
    virtual void onEndpointRegistered(MessageHubId messageHubId,
                                      EndpointId endpointId) = 0;

    //! Callback called when an endpoint is unregistered from any MessageHub,
    //! except for this MessageHub.
    virtual void onEndpointUnregistered(MessageHubId messageHubId,
                                        EndpointId endpointId) = 0;

    //! Recycle function called by pw::IntrusivePtr when the MessageHubCallback
    //! is no longer in use. The default behavior in Pigweed is to `delete
    //! this`. The callbacks derived from this class should also inherit from
    //! pw::Recyclable and override this function.
    virtual void pw_recycle() = 0;
  };

  //! The API returned when registering a MessageHub with the MessageRouter.
  class MessageHub {
   public:
    //! Creates an empty MessageHub that is not usable, similar to a moved-from
    //! MessageHub. Attempting to call any method on this object results in
    //! undefined behavior.
    MessageHub();

    // There can only be one live MessageHub instance for a given hub ID, so
    // only move operations are supported.
    MessageHub(const MessageHub &) = delete;
    MessageHub &operator=(const MessageHub &) = delete;
    MessageHub(MessageHub &&other);
    MessageHub &operator=(MessageHub &&other);

    //! Destructor. Unregisters the MessageHub from the MessageRouter.
    ~MessageHub();

    //! Accepts the session open request from the peer message hub.
    //! onSessionOpened will be called on both hubs.
    void onSessionOpenComplete(SessionId sessionId);

    //! Opens a session from an endpoint connected to the current MessageHub
    //! to the listed MessageHub ID and endpoint ID, with the given service
    //! descriptor, a null-terminated ASCII string.
    //! onSessionOpenRequest will be called to request the session to be
    //! opened. Once the peer message hub calls onSessionOpenComplete or
    //! closeSession, onSessionOpened or onSessionClosed will be called,
    //! depending on the result. If the session ID is provided (not
    //! SESSION_ID_INVALID), it must be unique and from the reserved session ID
    //! range. MessageRouter does not guarantee anything about the session ID if
    //! it is provided in this API. If the session ID is not provided,
    //! MessageRouter will assign a session ID normally.
    //! @return The session ID or SESSION_ID_INVALID if the session could
    //! not be opened
    SessionId openSession(EndpointId fromEndpointId,
                          MessageHubId toMessageHubId, EndpointId toEndpointId,
                          const char *serviceDescriptor = nullptr,
                          SessionId sessionId = SESSION_ID_INVALID);

    //! Closes the session with sessionId and reason
    //! @return true if the session was closed, false if the session was not
    //! found
    bool closeSession(SessionId sessionId,
                      Reason reason = Reason::CLOSE_ENDPOINT_SESSION_REQUESTED);

    //! Returns a session if it exists
    //! @return The session or std::nullopt if the session was not found
    std::optional<Session> getSessionWithId(SessionId sessionId);

    //! Sends a message to the session specified by sessionId.
    //! @see chreSendReliableMessageAsync. Sends the message in a reliable
    //! manner if possible. If the message cannot be delivered, the session
    //! is closed and subsequent calls to this function with the same sessionId
    //! will return false.
    //! @param data The data to send
    //! @param messageType The type of the message, a bit flagged value
    //! @param messagePermissions The permissions of the message, a bit flagged
    //! value
    //! @param sessionId The session to send the message on
    //! @param fromEndpointId The endpoint ID of the sender or ENDPOINT_ID_ANY
    //! to allow MessageRouter to infer the sender endpoint ID. If the
    //! sender endpoint ID cannot be inferred, (i.e. the session is between
    //! endpoints on the same message hub), this function will return false.
    //! @return true if the message was sent, false if the message could not be
    //! sent
    bool sendMessage(pw::UniquePtr<std::byte[]> &&data, uint32_t messageType,
                     uint32_t messagePermissions, SessionId sessionId,
                     EndpointId fromEndpointId = ENDPOINT_ID_ANY);

    //! Registers an endpoint with the MessageHub.
    //! @return true if the endpoint was registered, otherwise false.
    bool registerEndpoint(EndpointId endpointId);

    //! Unregisters an endpoint from the MessageHub.
    //! @return true if the endpoint was unregistered, otherwise false.
    bool unregisterEndpoint(EndpointId endpointId);

    //! @return The MessageHub ID of the currently connected MessageHub
    MessageHubId getId();

    //! @return If the MessageHub is active and registered with the
    //! MessageRouter.
    bool isRegistered();

    //! Unregisters this MessageHub from the MessageRouter.
    void unregister();

   private:
    friend class MessageRouter;

    MessageHub(MessageRouter &router, MessageHubId id);

    //! The MessageRouter that this MessageHub is connected to
    MessageRouter *mRouter;

    //! The id of this message hub
    MessageHubId mHubId;
  };

  //! Represents a MessageHub and its connected endpoints
  struct MessageHubRecord {
    MessageHubInfo info;
    pw::IntrusivePtr<MessageHubCallback> callback;
  };

  //! The default reserved session ID value
  static constexpr SessionId kDefaultReservedSessionId = 0x8000;

  MessageRouter() = delete;

  //! Constructor for the MessageRouter.
  //! @param messageHubs The list of MessageHubs connected to the MessageRouter
  //! @param sessions The list of sessions connected to the MessageRouter
  //! @param reservedSessionId The first reserved session ID - MessageRouter
  //! will not assign session IDs greater than or equal to this value
  MessageRouter(pw::Vector<MessageHubRecord> &messageHubs,
                pw::Vector<Session> &sessions,
                SessionId reservedSessionId = kDefaultReservedSessionId)
      : kReservedSessionId(reservedSessionId),
        mMessageHubs(messageHubs),
        mSessions(sessions) {}

  //! Registers a MessageHub with the MessageRouter.
  //! The provided name must be unique and not registered before and be a valid
  //! C string. The data underlying name must outlive the MessageHub. The
  //! callback must outlive the MessageHub. The ID must be unique and not
  //! registered before. When the returned MessageHub is destroyed, it will
  //! unregister itself from the MessageRouter.
  //! @param name The name of the MessageHub
  //! @param id The ID of the MessageHub
  //! @param callback The callback to handle messages sent to the MessageHub
  //! @return The MessageHub API or std::nullopt if the MessageHub could not be
  //! registered
  std::optional<MessageHub> registerMessageHub(
      const char *name, MessageHubId id,
      pw::IntrusivePtr<MessageHubCallback> callback);

  //! Executes the function for each endpoint connected to this MessageHub.
  //! If function returns true, the iteration will stop.
  //! @return true if the MessageHub is found, false otherwise
  bool forEachEndpointOfHub(
      MessageHubId messageHubId,
      const pw::Function<bool(const EndpointInfo &)> &function);

  //! Executes the function for each endpoint connected to all Message Hubs.
  //! @return true if successful, false otherwise
  bool forEachEndpoint(
      const pw::Function<void(const MessageHubInfo &, const EndpointInfo &)>
          &function);

  //! @return The EndpointInfo for the given hub and endpoint IDs
  std::optional<EndpointInfo> getEndpointInfo(MessageHubId messageHubId,
                                              EndpointId endpointId);

  //! @return The Endpoint for the given service descriptor. If multiple
  //! endpoints have the same service descriptor, the first one is returned.
  //! If the message hub ID is MESSAGE_HUB_ID_ANY, all message hubs are
  //! searched.
  std::optional<Endpoint> getEndpointForService(MessageHubId messageHubId,
                                                const char *serviceDescriptor);

  //! @return true if the endpoint has the given service descriptor, a
  //! null-terminated ASCII string, false otherwise.
  bool doesEndpointHaveService(MessageHubId messageHubId, EndpointId endpointId,
                               const char *serviceDescriptor);

  //! Executes the function for each service provided by an endpoint connected
  //! to this MessageHub. If function returns true, the iteration will stop.
  //! @return true if successful, false otherwise
  bool forEachService(
      const pw::Function<bool(const MessageHubInfo &, const EndpointInfo &,
                              const ServiceInfo &)> &function);

  //! Executes the function for each MessageHub connected to the
  //! MessageRouter. If function returns true, the iteration will stop.
  //! @return true if successful, false if failed
  bool forEachMessageHub(
      const pw::Function<bool(const MessageHubInfo &)> &function);

 private:
  //! Unregisters a MessageHub from the MessageRouter. This function will
  //! close all sessions that were initiated by or connected to the MessageHub
  //! and destroy the MessageHubRecord. This function will call the callback
  //! for each session that was closed only for the other message hub in the
  //! session.
  //! @return true if the MessageHub was unregistered, false if the MessageHub
  //! was not found.
  bool unregisterMessageHub(MessageHubId fromMessageHubId);

  //! Accepts the session open request from the peer message hub.
  //! onSessionOpened will be called on both hubs.
  void onSessionOpenComplete(MessageHubId fromMessageHubId,
                             SessionId sessionId);

  //! Opens a session from an endpoint connected to the current MessageHub
  //! to the listed MessageHub ID and endpoint ID, with the given service
  //! descriptor, a null-terminated ASCII string.
  //! onSessionOpenRequest will be called to request the session to be
  //! opened. Once the peer message hub calls onSessionOpenComplete or
  //! closeSession, onSessionOpened or onSessionClosed will be called,
  //! depending on the result. If the session ID is provided (not
  //! SESSION_ID_INVALID), it must be unique and from the reserved session ID
  //! range. MessageRouter does not guarantee anything about the session ID if
  //! it is provided in this API. If the session ID is not provided,
  //! MessageRouter will assign a session ID normally.
  //! @return The session ID or SESSION_ID_INVALID if the session could not be
  //! opened
  SessionId openSession(MessageHubId fromMessageHubId,
                        EndpointId fromEndpointId, MessageHubId toMessageHubId,
                        EndpointId toEndpointId,
                        const char *serviceDescriptor = nullptr,
                        SessionId sessionId = SESSION_ID_INVALID);

  //! Closes the session with sessionId and reason
  //! @return true if the session was closed, false if the session was not
  //! found
  bool closeSession(MessageHubId fromMessageHubId, SessionId sessionId,
                    Reason reason = Reason::CLOSE_ENDPOINT_SESSION_REQUESTED);

  //! Finalizes the session with sessionId and reason. If reason is provided,
  //! the session will be closed, else the session will be fully opened.
  //! @return true if the session was finalized, false if the session was not
  //! found or one of the message hubs were not found or not linked to the
  //! session.
  bool finalizeSession(MessageHubId fromMessageHubId, SessionId sessionId,
                       std::optional<Reason> reason);

  //! Returns a session if it exists
  //! @return The session or std::nullopt if the session was not found
  std::optional<Session> getSessionWithId(MessageHubId fromMessageHubId,
                                          SessionId sessionId);

  //! Sends a message to the session specified by sessionId.
  //! @see chreSendReliableMessageAsync. Sends the message in a reliable
  //! manner if possible. If the message cannot be delivered, the session
  //! is closed and subsequent calls to this function with the same sessionId
  //! will return false.
  //! @see MessageHub::sendMessage
  //! @return true if the message was sent, false if the message could not be
  //! sent
  bool sendMessage(pw::UniquePtr<std::byte[]> &&data, uint32_t messageType,
                   uint32_t messagePermissions, SessionId sessionId,
                   EndpointId fromEndpointId, MessageHubId fromMessageHubId);

  //! Registers an endpoint with the MessageHub.
  //! @return true if the endpoint was registered, otherwise false.
  bool registerEndpoint(MessageHubId messageHubId, EndpointId endpointId);

  //! Unregisters an endpoint from the MessageHub.
  //! @return true if the endpoint was unregistered, otherwise false.
  bool unregisterEndpoint(MessageHubId messageHubId, EndpointId endpointId);

  //! Helper function for registering or unregistering an endpoint with a
  //! MessageHub.
  //! @return true if the endpoint was registered or unregistered, otherwise
  //! false.
  bool onEndpointRegistrationStateChanged(MessageHubId messageHubId,
                                          EndpointId endpointId,
                                          bool isRegistered);

  //! @return The a copy of the list of MessageHubRecords
  std::optional<DynamicVector<MessageHubRecord>> getMessageHubRecords();

  //! @return A copy of the list of MessageHubRecords while holding mMutex.
  std::optional<DynamicVector<MessageHubRecord>> getMessageHubRecordsLocked();

  //! @return The MessageHubRecord for the given MessageHub ID
  const MessageHubRecord *getMessageHubRecordLocked(MessageHubId messageHubId);

  //! @return The index of the session if it exists
  //! Requires the caller to hold the mutex
  std::optional<size_t> findSessionIndexLocked(MessageHubId fromMessageHubId,
                                               SessionId sessionId);

  //! @return The callback for the given MessageHub ID or nullptr if not found
  //! Requires the caller to hold the mutex
  pw::IntrusivePtr<MessageHubCallback> getCallbackFromMessageHubId(
      MessageHubId messageHubId);

  //! @return The callback for the given MessageHub ID or nullptr if not found
  pw::IntrusivePtr<MessageHubCallback> getCallbackFromMessageHubIdLocked(
      MessageHubId messageHubId);

  //! @return true if the endpoint exists in the MessageHub with the given
  //! callback
  bool checkIfEndpointExists(
      const pw::IntrusivePtr<MessageHubCallback> &callback,
      EndpointId endpointId);

  //! @return The next available Session ID. Will wrap around if needed and
  //! ensures the returned ID is not in the reserved range nor is it already in
  //! use. Requires the caller to hold the mutex.
  SessionId getNextSessionIdLocked();

  //! The mutex to protect MessageRouter state
  Mutex mMutex;

  //! The next available Session ID
  SessionId mNextSessionId = 0;

  //! The start of the reserved session ID range
  const SessionId kReservedSessionId;

  //! The list of MessageHubs connected to the MessageRouter
  pw::Vector<MessageHubRecord> &mMessageHubs;

  //! The list of sessions connected to the MessageRouter
  pw::Vector<Session> &mSessions;
};

//! Define the singleton instance of the MessageRouter
typedef Singleton<MessageRouter> MessageRouterSingleton;

//! Routes messages between MessageHubs
template <size_t kMaxMessageHubs, size_t kMaxSessions>
class MessageRouterWithStorage : public MessageRouter {
 public:
  MessageRouterWithStorage(
      SessionId reservedSessionId = MessageRouter::kDefaultReservedSessionId)
      : MessageRouter(mMessageHubs, mSessions, reservedSessionId) {}

 private:
  //! The list of MessageHubs connected to the MessageRouter
  pw::Vector<MessageHubRecord, kMaxMessageHubs> mMessageHubs;

  //! The list of sessions connected to the MessageRouter
  pw::Vector<Session, kMaxSessions> mSessions;
};

}  // namespace chre::message

#endif  // CHRE_UTIL_SYSTEM_MESSAGE_ROUTER_H_
