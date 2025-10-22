/* * Copyright (C) 2020 The Android Open Source Project
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

#include "chre_cross_validator_wifi_manager.h"

#include <cinttypes>
#include <cstring>

#include "chre/util/nanoapp/log.h"
#include "chre/util/nanoapp/wifi.h"
#include "chre_api/chre.h"
#include "send_message.h"

namespace chre::cross_validator_wifi {

// Fake scan monitor cookie which is not used
constexpr uint32_t kScanMonitoringCookie = 0;

void Manager::handleEvent(uint32_t senderInstanceId, uint16_t eventType,
                          const void *eventData) {
  switch (eventType) {
    case CHRE_EVENT_MESSAGE_FROM_HOST:
      handleMessageFromHost(
          senderInstanceId,
          static_cast<const chreMessageFromHostData *>(eventData));
      break;
    case CHRE_EVENT_WIFI_ASYNC_RESULT:
      handleWifiAsyncResult(static_cast<const chreAsyncResult *>(eventData));
      break;
    case CHRE_EVENT_WIFI_SCAN_RESULT:
      handleWifiScanResult(static_cast<const chreWifiScanEvent *>(eventData));
      break;
    default:
      LOGE("Unknown message type %" PRIu16 "received when handling event",
           eventType);
  }
}

void Manager::handleMessageFromHost(uint32_t senderInstanceId,
                                    const chreMessageFromHostData *hostData) {
  if (senderInstanceId != CHRE_INSTANCE_ID) {
    LOGE("Incorrect sender instance id: %" PRIu32, senderInstanceId);
    return;
  }
  mCrossValidatorState.hostEndpoint = hostData->hostEndpoint;
  switch (hostData->messageType) {
    case chre_cross_validation_wifi_MessageType_STEP_START: {
      pb_istream_t stream = pb_istream_from_buffer(
          static_cast<const pb_byte_t *>(
              const_cast<const void *>(hostData->message)),
          hostData->messageSize);
      chre_cross_validation_wifi_StepStartCommand stepStartCommand;
      if (!pb_decode(&stream,
                     chre_cross_validation_wifi_StepStartCommand_fields,
                     &stepStartCommand)) {
        LOGE("Error decoding StepStartCommand");
        break;
      }
      handleStepStartMessage(stepStartCommand);
      break;
    }
    case chre_cross_validation_wifi_MessageType_SCAN_RESULT:
      handleDataMessage(hostData);
      break;
    default:
      LOGE("Unknown message type %" PRIu32 " for host message",
           hostData->messageType);
  }
}

void Manager::handleStepStartMessage(
    chre_cross_validation_wifi_StepStartCommand stepStartCommand) {
  switch (stepStartCommand.step) {
    case chre_cross_validation_wifi_Step_INIT:
      LOGE("Received StepStartCommand for INIT step");
      break;
    case chre_cross_validation_wifi_Step_CAPABILITIES: {
      LOGD("%s: Received Step_CAPABILITIES", __func__);
      chre_cross_validation_wifi_WifiCapabilities wifiCapabilities =
          makeWifiCapabilitiesMessage(chreWifiGetCapabilities());
      test_shared::sendMessageToHost(
          mCrossValidatorState.hostEndpoint, &wifiCapabilities,
          chre_cross_validation_wifi_WifiCapabilities_fields,
          chre_cross_validation_wifi_MessageType_WIFI_CAPABILITIES);
      break;
    }
    case chre_cross_validation_wifi_Step_SETUP: {
      if (!chreWifiConfigureScanMonitorAsync(/* enable= */ true,
                                             &kScanMonitoringCookie)) {
        LOGE("chreWifiConfigureScanMonitorAsync() failed");
        test_shared::sendTestResultWithMsgToHost(
            mCrossValidatorState.hostEndpoint,
            chre_cross_validation_wifi_MessageType_STEP_RESULT,
            /*success=*/false,
            /* errMessage= */ "setupWifiScanMonitoring failed",
            /*abortOnFailure=*/false);
        break;
      }
      LOGD("chreWifiConfigureScanMonitorAsync() succeeded");
      if (stepStartCommand.has_chreScanCapacity) {
        mExpectedMaxChreResultCanHandle = stepStartCommand.chreScanCapacity;
      }
      break;
    }
    case chre_cross_validation_wifi_Step_VALIDATE:
      break;
    default:
      LOGE("Unexpected start step: %" PRIu8,
           static_cast<uint8_t>(stepStartCommand.step));
  }
  mStep = stepStartCommand.step;
}

void Manager::handleDataMessage(const chreMessageFromHostData *hostData) {
  pb_istream_t stream =
      pb_istream_from_buffer(reinterpret_cast<const pb_byte_t *>(
                                 const_cast<const void *>(hostData->message)),
                             hostData->messageSize);
  WifiScanResult scanResult(&stream);
  uint8_t scanResultIndex = scanResult.getResultIndex();
  if (scanResultIndex > scanResult.getTotalNumResults()) {
    LOGE("AP scan result index is greater than scan results size");
    return;
  }
  if (!mApScanResults.push_back(scanResult)) {
    LOG_OOM();
  }
  LOGD("%s: AP wifi result %" PRIu8 "/%" PRIu8 " is received", __func__,
       static_cast<uint8_t>(scanResultIndex + 1),
       scanResult.getTotalNumResults());
  if (!scanResult.isLastMessage()) {
    return;
  }
  mApDataCollectionDone = true;
  if (mChreDataCollectionDone) {
    compareAndSendResultToHost();
  }
}

void Manager::handleWifiScanResult(const chreWifiScanEvent *event) {
  if (!mScanStartSeen && event->eventIndex != 0) {
    LOGW("Dropping chreWifiScanEvent because we haven't seen eventIndex=0");
    return;
  }
  mScanStartSeen = true;
  for (uint8_t i = 0; i < event->resultCount; i++) {
    mChreScanResults.push_back(event->results[i]);
  }
  LOGD("%s: CHRE wifi result %zu/%" PRIu8 " is received", __func__,
       mChreScanResults.size(), event->resultTotal);
  if (mChreScanResults.size() < event->resultTotal) {
    return;
  }
  mChreDataCollectionDone = true;
  if (mApDataCollectionDone) {
    compareAndSendResultToHost();
  }
}

void Manager::compareAndSendResultToHost() {
  chre_test_common_TestResult testResult;
  bool belowMaxSizeCheck =
      mApScanResults.size() <= mExpectedMaxChreResultCanHandle &&
      mApScanResults.size() != mChreScanResults.size();
  bool aboveMaxSizeCheck =
      mApScanResults.size() > mExpectedMaxChreResultCanHandle &&
      mApScanResults.size() < mChreScanResults.size();

  LOGI("Wifi scan result counts, AP = %zu, CHRE = %zu, MAX = %" PRIu8,
       mApScanResults.size(), mChreScanResults.size(),
       mExpectedMaxChreResultCanHandle);

  verifyScanResults(&testResult);

  if (belowMaxSizeCheck || aboveMaxSizeCheck) {
    LOGE("Scan results differ: AP = %zu, CHRE = %zu, MAX = %" PRIu8,
         mApScanResults.size(), mChreScanResults.size(),
         mExpectedMaxChreResultCanHandle);
    sendTestResult(/*success=*/false,
                   /* errorMessage= */
                   "There is a different number of AP and CHRE scan results.");
    return;
  }

  test_shared::sendMessageToHost(
      mCrossValidatorState.hostEndpoint, &testResult,
      chre_test_common_TestResult_fields,
      chre_cross_validation_wifi_MessageType_STEP_RESULT);
}

void Manager::verifyScanResults(chre_test_common_TestResult *testResultOut) {
  bool allResultsValid = true;
  for (const chreWifiScanResult &result : mChreScanResults) {
    const WifiScanResult chreWifiScanResult = WifiScanResult(result);
    bool isValidResult = true;
    size_t index =
        getMatchingScanResultIndex(mApScanResults, chreWifiScanResult);

    const char *bssidStr = "<non-printable>";
    char bssidBuffer[chre::kBssidStrLen];
    if (chre::parseBssidToStr(chreWifiScanResult.getBssid(), bssidBuffer,
                              sizeof(bssidBuffer))) {
      bssidStr = bssidBuffer;
    }

    // chreWifiScanResult is found
    if (index != SIZE_MAX) {
      WifiScanResult &apScanResult = mApScanResults[index];
      if (apScanResult.getSeen()) {
        *testResultOut = test_shared::makeTestResultProtoMessage(
            /*success=*/false, "Saw a CHRE scan result with a duplicate BSSID");
        isValidResult = false;
        LOGE("CHRE Scan Result with bssid: %s has a duplicate BSSID", bssidStr);
      }
      if (!WifiScanResult::areEqual(chreWifiScanResult, apScanResult)) {
        *testResultOut = test_shared::makeTestResultProtoMessage(
            /*success=*/false,
            "Fields differ between an AP and CHRE scan result with same Bssid");
        isValidResult = false;
        LOGE(
            "CHRE Scan Result with bssid: %s found fields differ with "
            "an AP scan result with same Bssid",
            bssidStr);
      }
      // Mark this scan result as already seen so that the next time it is used
      // as a match the test will fail because of duplicate scan results.
      apScanResult.didSee();
    } else {
      // Error CHRE BSSID does not match any AP
      *testResultOut = test_shared::makeTestResultProtoMessage(
          /*success=*/false,
          "Could not find an AP scan result with the same Bssid in CHRE "
          "result");
      isValidResult = false;
      LOGE(
          "CHRE Scan Result with bssid: %s fail to find an AP scan "
          "with same Bssid",
          bssidStr);
    }

    if (!isValidResult) {
      LOGE("False CHRE Scan Result with the following info:");
      logChreWifiResult(result);
      allResultsValid = false;
    }
  }

  for (const WifiScanResult &scanResult : mApScanResults) {
    if (!scanResult.getSeen()) {
      const char *bssidStr = "<non-printable>";
      char bssidBuffer[chre::kBssidStrLen];
      if (chre::parseBssidToStr(scanResult.getBssid(), bssidBuffer,
                                sizeof(bssidBuffer))) {
        bssidStr = bssidBuffer;
      }
      LOGW("AP %s with bssid %s is not seen in CHRE", scanResult.getSsid(),
           bssidStr);
      // Since CHRE is more constrained in memory, it is expected that if we
      // receive over a cretin amount of AP, we will drop some of them.
      if (mApScanResults.size() <= mExpectedMaxChreResultCanHandle) {
        *testResultOut = test_shared::makeTestResultProtoMessage(
            /*success=*/false,
            "Extra AP information shown in host "
            "when small number of AP results presenting");
        allResultsValid = false;
      }
    }
  }

  if (allResultsValid) {
    *testResultOut = test_shared::makeTestResultProtoMessage(true);
  }
}

size_t Manager::getMatchingScanResultIndex(
    const DynamicVector<WifiScanResult> &results,
    const WifiScanResult &queryResult) {
  for (size_t i = 0; i < results.size(); i++) {
    if (WifiScanResult::bssidsAreEqual(results[i], queryResult)) {
      return i;
    }
  }
  return SIZE_MAX;
}

void Manager::sendTestResult(bool success, const char *errorMessage) const {
  test_shared::sendTestResultWithMsgToHost(
      mCrossValidatorState.hostEndpoint,
      chre_cross_validation_wifi_MessageType_STEP_RESULT, success, errorMessage,
      /* abortOnFailure= */ false);
}

chre_cross_validation_wifi_WifiCapabilities
Manager::makeWifiCapabilitiesMessage(uint32_t capabilitiesFromChre) {
  chre_cross_validation_wifi_WifiCapabilities capabilities;
  capabilities.has_wifiCapabilities = true;
  capabilities.wifiCapabilities = capabilitiesFromChre;
  return capabilities;
}

void Manager::handleWifiAsyncResult(const chreAsyncResult *result) {
  if (result->requestType != CHRE_WIFI_REQUEST_TYPE_CONFIGURE_SCAN_MONITOR) {
    sendTestResult(/*success=*/false,
                   /*errorMessage=*/"Unknown chre async result type received");
    return;
  }
  if (mStep != chre_cross_validation_wifi_Step_SETUP) {
    sendTestResult(
        /*success=*/false,
        /*errorMessage=*/"Received scan monitor result but step is not SETUP");
    return;
  }
  if (result->success) {
    LOGI("Wifi scan monitoring setup successfully");
    sendTestResult(/*success=*/true);
  } else {
    LOGE("Wifi scan monitoring setup failed async w/ error code %" PRIu8,
         result->errorCode);
    sendTestResult(/*success=*/false,
                   /*errorMessage=*/"Wifi scan monitoring setup failed async");
  }
}

}  // namespace chre::cross_validator_wifi
