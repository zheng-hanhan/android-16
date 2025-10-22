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

#include "bluetooth_socket_fbs_hal.h"

#include <cstdint>
#include <future>

#include "chre/platform/shared/host_protocol_common.h"
#include "chre_host/generated/host_messages_generated.h"
#include "chre_host/host_protocol_host.h"
#include "chre_host/log.h"
#include "flatbuffers/flatbuffers.h"

namespace aidl::android::hardware::bluetooth::socket::impl {

using ::android::chre::getStringFromByteVector;
using ndk::ScopedAStatus;

ScopedAStatus BluetoothSocketFbsHal::registerCallback(
    const std::shared_ptr<IBluetoothSocketCallback> &callback) {
  mCallback = callback;
  return ScopedAStatus::ok();
}

ScopedAStatus BluetoothSocketFbsHal::getSocketCapabilities(
    SocketCapabilities *result) {
  std::future<SocketCapabilities> future = mCapabilitiesPromise.get_future();

  flatbuffers::FlatBufferBuilder builder(64);
  auto socketCapabilitiesRequest =
      ::chre::fbs::CreateBtSocketCapabilitiesRequest(builder);
  ::chre::HostProtocolCommon::finalize(
      builder, ::chre::fbs::ChreMessage::BtSocketCapabilitiesRequest,
      socketCapabilitiesRequest.Union());

  if (!mOffloadLinkAvailable) {
    LOGE("BT Socket Offload Link not available");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "BT offload link not available");
  }

  if (!mOffloadLink->sendMessageToOffloadStack(builder.GetBufferPointer(),
                                               builder.GetSize())) {
    LOGE("Failed to send BT socket capabilities request message");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "Failed to send BT socket message");
  }

  std::future_status status = future.wait_for(std::chrono::seconds(5));
  if (status != std::future_status::ready) {
    LOGE("BT Socket capabilities request timed out");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "BT socket capabilities request timed out");
  }

  *result = future.get();
  return ScopedAStatus::ok();
}

ScopedAStatus BluetoothSocketFbsHal::opened(const SocketContext &context) {
  LOGD("Host opened BT offload socket ID=%" PRIu64, context.socketId);
  if (!mOffloadLinkAvailable) {
    LOGE("BT Socket Offload Link not available");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "BT offload link not available");
  }
  if (context.channelInfo.getTag() != ChannelInfo::Tag::leCocChannelInfo) {
    LOGE("Got open request for unsupported socket type %" PRId32,
         context.channelInfo.getTag());
    sendOpenedCompleteMessage(context.socketId, Status::FAILURE,
                              "Unsupported socket type");
    return ScopedAStatus::ok();
  }
  flatbuffers::FlatBufferBuilder builder(1028);
  auto socketName = ::chre::HostProtocolCommon::addStringAsByteVector(
      builder, context.name.c_str());
  const auto &socketChannelInfo =
      context.channelInfo.get<ChannelInfo::Tag::leCocChannelInfo>();
  auto leCocChannelInfo = ::chre::fbs::CreateLeCocChannelInfo(
      builder, socketChannelInfo.localCid, socketChannelInfo.remoteCid,
      socketChannelInfo.psm, socketChannelInfo.localMtu,
      socketChannelInfo.remoteMtu, socketChannelInfo.localMps,
      socketChannelInfo.remoteMps, socketChannelInfo.initialRxCredits,
      socketChannelInfo.initialTxCredits);

  auto socketOpen = ::chre::fbs::CreateBtSocketOpen(
      builder, context.socketId, socketName, context.aclConnectionHandle,
      ::chre::fbs::ChannelInfo::LeCocChannelInfo, leCocChannelInfo.Union(),
      context.endpointId.hubId, context.endpointId.id);
  ::chre::HostProtocolCommon::finalize(
      builder, ::chre::fbs::ChreMessage::BtSocketOpen, socketOpen.Union());

  if (!mOffloadLink->sendMessageToOffloadStack(builder.GetBufferPointer(),
                                               builder.GetSize())) {
    LOGE("Failed to send BT socket opened message");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "Failed to send BT socket message");
  }
  return ScopedAStatus::ok();
}

ScopedAStatus BluetoothSocketFbsHal::closed(int64_t socketId) {
  LOGD("Host closed BT offload socket ID=%" PRIu64, socketId);
  if (!mOffloadLinkAvailable) {
    LOGE("BT Socket Offload Link not available");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "BT offload link not available");
  }

  flatbuffers::FlatBufferBuilder builder(64);
  auto socketCloseResponse =
      ::chre::fbs::CreateBtSocketCloseResponse(builder, socketId);
  ::chre::HostProtocolCommon::finalize(
      builder, ::chre::fbs::ChreMessage::BtSocketCloseResponse,
      socketCloseResponse.Union());

  if (!mOffloadLink->sendMessageToOffloadStack(builder.GetBufferPointer(),
                                               builder.GetSize())) {
    LOGE("Failed to send BT socket closed message");
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
        static_cast<int32_t>(STATUS_UNKNOWN_ERROR),
        "Failed to send BT socket message");
  }

  return ScopedAStatus::ok();
}

void BluetoothSocketFbsHal::handleMessageFromOffloadStack(const void *message,
                                                          size_t length) {
  if (!chre::HostProtocolCommon::verifyMessage(message, length)) {
    LOGE("Could not decode Bluetooth Socket message");
  } else {
    std::unique_ptr<chre::fbs::MessageContainerT> container =
        chre::fbs::UnPackMessageContainer(message);
    chre::fbs::ChreMessageUnion &msg = container->message;
    switch (container->message.type) {
      case chre::fbs::ChreMessage::BtSocketOpenResponse:
        handleBtSocketOpenResponse(*msg.AsBtSocketOpenResponse());
        break;

      case chre::fbs::ChreMessage::BtSocketClose:
        handleBtSocketClose(*msg.AsBtSocketClose());
        break;

      case chre::fbs::ChreMessage::BtSocketCapabilitiesResponse:
        handleBtSocketCapabilitiesResponse(
            *msg.AsBtSocketCapabilitiesResponse());
        break;

      default:
        LOGW("Got unexpected Bluetooth Socket message type %" PRIu8,
             static_cast<uint8_t>(msg.type));
        break;
    }
  }
}

void BluetoothSocketFbsHal::handleBtSocketOpenResponse(
    const ::chre::fbs::BtSocketOpenResponseT &response) {
  std::string reason = std::string(getStringFromByteVector(response.reason));
  LOGD("Got BT Socket open response, socket ID=%" PRIu64
       ", status=%d, reason=%s",
       response.socketId, response.status, reason.c_str());
  sendOpenedCompleteMessage(
      response.socketId,
      response.status == ::chre::fbs::BtSocketOpenStatus::SUCCESS
          ? Status::SUCCESS
          : Status::FAILURE,
      reason);
}

void BluetoothSocketFbsHal::handleBtSocketClose(
    const ::chre::fbs::BtSocketCloseT &message) {
  std::string reason = std::string(getStringFromByteVector(message.reason));
  LOGD("Got BT Socket close, socket ID=%" PRIu64 ", reason=%s",
       message.socketId, reason.c_str());
  if (mCallback == nullptr) {
    LOGE("Received socket close message with no registered callback");
    return;
  }
  mCallback->close(message.socketId, reason);
}

void BluetoothSocketFbsHal::handleBtSocketCapabilitiesResponse(
    const ::chre::fbs::BtSocketCapabilitiesResponseT &response) {
  LOGD("Got BT Socket capabilities response");
  SocketCapabilities capabilities = {
      .leCocCapabilities =
          {
              .numberOfSupportedSockets =
                  response.leCocCapabilities->numberOfSupportedSockets,
              .mtu = response.leCocCapabilities->mtu,
          },
      .rfcommCapabilities =
          {
              .numberOfSupportedSockets =
                  response.rfcommCapabilities->numberOfSupportedSockets,
              .maxFrameSize = response.rfcommCapabilities->maxFrameSize,
          },
  };
  mCapabilitiesPromise.set_value(capabilities);
}

void BluetoothSocketFbsHal::sendOpenedCompleteMessage(int64_t socketId,
                                                      Status status,
                                                      std::string reason) {
  if (mCallback == nullptr) {
    LOGE("Sending socket opened complete with no registered callback");
    return;
  }
  mCallback->openedComplete(socketId, status, reason);
}

}  // namespace aidl::android::hardware::bluetooth::socket::impl
