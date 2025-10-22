/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "chre/pal/util/wifi_scan_cache.h"

#include <inttypes.h>

#include "chre/util/macros.h"

/************************************************
 *  Constants
 ***********************************************/

// Constants used in chreWifiScanCacheInitialAgeMsValue() and
// chreWifiScanCacheFinalizeAgeMs()
// These values are selected because msec = nsec / 1000000 and
// 1000000 = 64 * 15625 = (1 << 6) * 15625
#define CHRE_WIFI_SCAN_CACHE_AGE_MS_SHIFT (6)
#define CHRE_WIFI_SCAN_CACHE_AGE_MS_DIVISOR (15625)

/************************************************
 *  Prototypes
 ***********************************************/

struct chreWifiScanCacheState {
  //! true if the scan cache has started, i.e. chreWifiScanCacheScanEventBegin
  //! was invoked and has not yet ended.
  bool started;

  //! true if the current scan cache is a result of a CHRE active scan request.
  bool scanRequestedByChre;

  //! The number of chreWifiScanResults dropped due to OOM.
  uint16_t numWifiScanResultsDropped;

  //! Stores the WiFi cache elements
  struct chreWifiScanEvent event;
  struct chreWifiScanResult resultList[CHRE_PAL_WIFI_SCAN_CACHE_CAPACITY];

  //! The number of chreWifiScanEvent data pending release via
  //! chreWifiScanCacheReleaseScanEvent().
  uint8_t numWifiEventsPendingRelease;

  bool scanMonitoringEnabled;

  uint32_t scannedFreqList[CHRE_WIFI_FREQUENCY_LIST_MAX_LEN];

  uint64_t scanStartTimeNs;
};

/************************************************
 *  Global variables
 ***********************************************/
static const struct chrePalSystemApi *gSystemApi = NULL;
static const struct chrePalWifiCallbacks *gCallbacks = NULL;

static struct chreWifiScanCacheState gWifiCacheState;

//! true if scan monitoring is enabled via
//! chreWifiScanCacheConfigureScanMonitor().
static bool gScanMonitoringEnabled;

static const uint64_t kOneMillisecondInNanoseconds = UINT64_C(1000000);

/************************************************
 *  Private functions
 ***********************************************/
static bool chreWifiScanCacheIsInitialized(void) {
  return (gSystemApi != NULL && gCallbacks != NULL);
}

static bool areAllScanEventsReleased(void) {
  return gWifiCacheState.numWifiEventsPendingRelease == 0;
}

static bool isFrequencyListValid(const uint32_t *frequencyList,
                                 uint16_t frequencyListLen) {
  return (frequencyListLen == 0) || (frequencyList != NULL);
}

static bool paramsMatchScanCache(const struct chreWifiScanParams *params) {
  uint64_t timeNs = gWifiCacheState.event.referenceTime;
  bool scan_within_age =
      gWifiCacheState.started ||
      (timeNs >= gSystemApi->getCurrentTime() -
                     (params->maxScanAgeMs * kOneMillisecondInNanoseconds));

  // Perform a conservative check for the params and scan cache.
  // TODO(b/174510035): Consider optimizing for the case for channelSet ==
  // CHRE_WIFI_CHANNEL_SET_ALL.
  bool params_non_dfs =
      (params->scanType == CHRE_WIFI_SCAN_TYPE_ACTIVE) ||
      ((params->scanType == CHRE_WIFI_SCAN_TYPE_NO_PREFERENCE) &&
       (params->channelSet == CHRE_WIFI_CHANNEL_SET_NON_DFS));
  bool cache_non_dfs =
      (gWifiCacheState.event.scanType == CHRE_WIFI_SCAN_TYPE_ACTIVE) ||
      (gWifiCacheState.event.scanType == CHRE_WIFI_SCAN_TYPE_PASSIVE);

  bool cache_all_freq = (gWifiCacheState.event.scannedFreqListLen == 0);
  bool cache_all_ssid = (gWifiCacheState.event.ssidSetSize == 0);

  return scan_within_age && (params_non_dfs || !cache_non_dfs) &&
         cache_all_freq && cache_all_ssid;
}

static bool isWifiScanCacheBusy(bool logOnBusy) {
  bool busy = true;
  if (gWifiCacheState.started) {
    if (logOnBusy) {
      gSystemApi->log(CHRE_LOG_ERROR, "Scan cache already started");
    }
  } else if (!areAllScanEventsReleased()) {
    if (logOnBusy) {
      gSystemApi->log(CHRE_LOG_ERROR, "Scan cache events pending release");
    }
  } else {
    busy = false;
  }

  return busy;
}

static void chreWifiScanCacheDispatchAll(void) {
  gSystemApi->log(CHRE_LOG_DEBUG, "Dispatching %" PRIu8 " events",
                  gWifiCacheState.event.resultTotal);
  if (gWifiCacheState.event.resultTotal == 0) {
    gWifiCacheState.event.eventIndex = 0;
    gWifiCacheState.event.resultCount = 0;
    gWifiCacheState.event.results = NULL;
    gCallbacks->scanEventCallback(&gWifiCacheState.event);
  } else {
    uint8_t eventIndex = 0;
    for (uint16_t i = 0; i < gWifiCacheState.event.resultTotal;
         i += CHRE_PAL_WIFI_SCAN_CACHE_MAX_RESULT_COUNT) {
      gWifiCacheState.event.resultCount =
          MIN(CHRE_PAL_WIFI_SCAN_CACHE_MAX_RESULT_COUNT,
              (uint8_t)(gWifiCacheState.event.resultTotal - i));
      gWifiCacheState.event.eventIndex = eventIndex++;
      gWifiCacheState.event.results = &gWifiCacheState.resultList[i];

      // TODO(b/174511061): The current approach only works for situations where
      // the event is released immediately. Add a way to handle this scenario
      // (e.g. an array of chreWifiScanEvent's).
      gWifiCacheState.numWifiEventsPendingRelease++;
      gCallbacks->scanEventCallback(&gWifiCacheState.event);
      if (gWifiCacheState.numWifiEventsPendingRelease != 0) {
        gSystemApi->log(CHRE_LOG_ERROR, "Scan event not released immediately");
      }
    }
  }
}

static bool isWifiScanResultInCache(const struct chreWifiScanResult *result,
                                    size_t *index) {
  for (uint8_t i = 0; i < gWifiCacheState.event.resultTotal; i++) {
    const struct chreWifiScanResult *cacheResult =
        &gWifiCacheState.resultList[i];
    // Filtering based on BSSID + SSID + frequency based on Linux cfg80211.
    // https://github.com/torvalds/linux/blob/master/net/wireless/scan.c
    if ((result->primaryChannel == cacheResult->primaryChannel) &&
        (memcmp(result->bssid, cacheResult->bssid, CHRE_WIFI_BSSID_LEN) == 0) &&
        (result->ssidLen == cacheResult->ssidLen) &&
        (memcmp(result->ssid, cacheResult->ssid, result->ssidLen) == 0)) {
      *index = i;
      return true;
    }
  }

  return false;
}

static bool isLowerRssiScanResultInCache(
    const struct chreWifiScanResult *result, size_t *index) {
  int8_t lowestRssi = result->rssi;
  bool foundWeakerResult = false;
  for (uint8_t i = 0; i < gWifiCacheState.event.resultTotal; i++) {
    const struct chreWifiScanResult *cacheResult =
        &gWifiCacheState.resultList[i];
    // Filter based on RSSI to determine weakest result in cache.
    if (cacheResult->rssi < lowestRssi) {
      lowestRssi = cacheResult->rssi;
      *index = i;
      foundWeakerResult = true;
    }
  }

  return foundWeakerResult;
}

static uint32_t chreWifiScanCacheInitialAgeMsValue() {
  // ageMs will be finalized via chreWifiScanCacheFinalizeAgeMs() once the scan
  // finishes, because it is relative to the scan end time that we can't know
  // yet. Before the end of the scan, populate ageMs with the time since the
  // start of the scan.
  //
  // We avoid 64-bit integer division by:
  //  - Only considering the delta between this result and the start of the
  //    scan, which constrains the range of expected values to what should be
  //    only a few seconds
  //  - Instead of directly dividing by 1000000, we first divide by 64 (right
  //    shift by 6), then truncate to 32 bits, then later we'll do integer
  //    division by 15625 to get milliseconds
  //    - This works because x/1000000 = x/(64 * 15625) = (x/64)/15625
  //    - The largest delta we can fit here is 2^32/15625 ms = 274877 ms or
  //      about 4.5 minutes
  uint64_t timeSinceScanStartNs =
      (gSystemApi->getCurrentTime() - gWifiCacheState.scanStartTimeNs);
  return (timeSinceScanStartNs >> CHRE_WIFI_SCAN_CACHE_AGE_MS_SHIFT);
}

static void chreWifiScanCacheFinalizeAgeMs() {
  // Convert ageMs from the chreWifiScanCacheInitialAgeMsValue() to its final,
  // correct value using the formula derived from these steps:
  //  ageMs = (referenceTimeNs - absoluteScanResultTimeNs) / 1000000
  //        = (referenceTimeNs - (scanStartTimeNs + scanOffsetNs)) / 1000000
  //        = ((referenceTimeNs - scanStartTimeNs) - scanOffsetNs) / 1000000
  //        = (scanDuration / 64 - scanOffsetNs / 64) / 15625
  //  ageMs = (scanDurationShifted - currentAgeMsValue) / 15625
  uint64_t referenceTimeNs = gWifiCacheState.event.referenceTime;
  uint64_t scanStartTimeNs = gWifiCacheState.scanStartTimeNs;
  uint32_t scanDurationShifted =
      (referenceTimeNs - scanStartTimeNs) >> CHRE_WIFI_SCAN_CACHE_AGE_MS_SHIFT;
  if (referenceTimeNs < scanStartTimeNs) {
    gSystemApi->log(CHRE_LOG_ERROR, "Invalid scan timestamp, clamping");
    // Use a smaller number to avoid very large ageMs values
    scanDurationShifted = 78125000;  // 5 seconds --> 5*10e9 / 64
  }

  for (uint16_t i = 0; i < gWifiCacheState.event.resultTotal; i++) {
    if (scanDurationShifted < gWifiCacheState.resultList[i].ageMs) {
      gSystemApi->log(CHRE_LOG_ERROR,
                      "Invalid result timestamp %" PRIu32 " vs. %" PRIu32,
                      gWifiCacheState.resultList[i].ageMs, scanDurationShifted);
      gWifiCacheState.resultList[i].ageMs = 0;
    } else {
      gWifiCacheState.resultList[i].ageMs =
          (scanDurationShifted - gWifiCacheState.resultList[i].ageMs) /
          CHRE_WIFI_SCAN_CACHE_AGE_MS_DIVISOR;
    }
  }
}

/************************************************
 *  Public functions
 ***********************************************/
bool chreWifiScanCacheInit(const struct chrePalSystemApi *systemApi,
                           const struct chrePalWifiCallbacks *callbacks) {
  if (systemApi == NULL || callbacks == NULL) {
    return false;
  }

  gSystemApi = systemApi;
  gCallbacks = callbacks;
  memset(&gWifiCacheState, 0, sizeof(gWifiCacheState));
  gScanMonitoringEnabled = false;

  return true;
}

void chreWifiScanCacheDeinit(void) {
  gSystemApi = NULL;
  gCallbacks = NULL;
}

bool chreWifiScanCacheScanEventBegin(enum chreWifiScanType scanType,
                                     uint8_t ssidSetSize,
                                     const uint32_t *scannedFreqList,
                                     uint16_t scannedFreqListLength,
                                     uint8_t radioChainPref,
                                     bool scanRequestedByChre) {
  bool success = false;
  if (chreWifiScanCacheIsInitialized()) {
    enum chreError error = CHRE_ERROR_NONE;
    if (!isFrequencyListValid(scannedFreqList, scannedFreqListLength)) {
      gSystemApi->log(CHRE_LOG_ERROR, "Invalid frequency argument");
      error = CHRE_ERROR_INVALID_ARGUMENT;
    } else if (isWifiScanCacheBusy(true /* logOnBusy */)) {
      error = CHRE_ERROR_BUSY;
    } else {
      success = true;
      memset(&gWifiCacheState, 0, sizeof(gWifiCacheState));

      gWifiCacheState.event.version = CHRE_WIFI_SCAN_EVENT_VERSION;
      gWifiCacheState.event.scanType = scanType;
      gWifiCacheState.event.ssidSetSize = ssidSetSize;

      scannedFreqListLength =
          MIN(scannedFreqListLength, CHRE_WIFI_FREQUENCY_LIST_MAX_LEN);
      if (scannedFreqList != NULL) {
        memcpy(gWifiCacheState.scannedFreqList, scannedFreqList,
               scannedFreqListLength * sizeof(uint32_t));
      }
      gWifiCacheState.event.scannedFreqListLen = scannedFreqListLength;
      gWifiCacheState.event.radioChainPref = radioChainPref;

      gWifiCacheState.scanRequestedByChre = scanRequestedByChre;
      gWifiCacheState.started = true;
      gWifiCacheState.scanStartTimeNs = gSystemApi->getCurrentTime();
    }

    if (scanRequestedByChre && !success) {
      gCallbacks->scanResponseCallback(false /* pending */, error);
    }
  }

  return success;
}

void chreWifiScanCacheScanEventAdd(const struct chreWifiScanResult *result) {
  if (!gWifiCacheState.started) {
    gSystemApi->log(CHRE_LOG_ERROR, "Cannot add to cache before starting it");
    return;
  }

  size_t index;
  if (!isWifiScanResultInCache(result, &index)) {
    if (gWifiCacheState.event.resultTotal >=
        CHRE_PAL_WIFI_SCAN_CACHE_CAPACITY) {
      gWifiCacheState.numWifiScanResultsDropped++;
      // Determine weakest result in cache to replace with the new result.
      if (!isLowerRssiScanResultInCache(result, &index)) {
        return;
      }
    } else {
      // Result was not already cached, add new entry to the end of the cache
      index = gWifiCacheState.event.resultTotal;
      gWifiCacheState.event.resultTotal++;
    }
  }

  memcpy(&gWifiCacheState.resultList[index], result,
         sizeof(const struct chreWifiScanResult));

  gWifiCacheState.resultList[index].ageMs =
      chreWifiScanCacheInitialAgeMsValue();
}

void chreWifiScanCacheScanEventEnd(enum chreError errorCode) {
  if (gWifiCacheState.started) {
    if (gWifiCacheState.numWifiScanResultsDropped > 0) {
      gSystemApi->log(CHRE_LOG_WARN,
                      "Dropped total of %" PRIu32 " access points",
                      gWifiCacheState.numWifiScanResultsDropped);
    }
    if (gWifiCacheState.scanRequestedByChre) {
      gCallbacks->scanResponseCallback(
          errorCode == CHRE_ERROR_NONE /* pending */, errorCode);
    }

    if (errorCode == CHRE_ERROR_NONE &&
        (gWifiCacheState.scanRequestedByChre || gScanMonitoringEnabled)) {
      gWifiCacheState.event.referenceTime = gSystemApi->getCurrentTime();
      gWifiCacheState.event.scannedFreqList = gWifiCacheState.scannedFreqList;
      chreWifiScanCacheFinalizeAgeMs();
      chreWifiScanCacheDispatchAll();
    }

    gWifiCacheState.started = false;
    gWifiCacheState.scanRequestedByChre = false;
  }
}

bool chreWifiScanCacheDispatchFromCache(
    const struct chreWifiScanParams *params) {
  if (!chreWifiScanCacheIsInitialized()) {
    return false;
  }

  if (paramsMatchScanCache(params)) {
    if (!isWifiScanCacheBusy(false /* logOnBusy */)) {
      // Satisfied by cache
      gCallbacks->scanResponseCallback(true /* pending */, CHRE_ERROR_NONE);
      chreWifiScanCacheDispatchAll();
      return true;
    } else if (gWifiCacheState.started) {
      // Will be satisfied by cache once the scan completes
      gSystemApi->log(CHRE_LOG_INFO, "Using in-progress scan for CHRE request");
      gWifiCacheState.scanRequestedByChre = true;
      return true;
    } else {
      // Assumed: busy because !areAllScanEventsReleased()
      // TODO(b/174511061): the current code assumes scan events are released
      // synchronously, so this should never happen
      gSystemApi->log(CHRE_LOG_ERROR,
                      "Unexpected scan request while delivering results");
      return false;
    }
  } else {
    // Cache contains results from incompatible scan parameters (either too old
    // or different scan type), so a new scan is needed
    return false;
  }
}

void chreWifiScanCacheReleaseScanEvent(struct chreWifiScanEvent *event) {
  if (!chreWifiScanCacheIsInitialized()) {
    return;
  }

  if (event != &gWifiCacheState.event) {
    gSystemApi->log(CHRE_LOG_ERROR, "Invalid event pointer %p", event);
  } else if (gWifiCacheState.numWifiEventsPendingRelease > 0) {
    gWifiCacheState.numWifiEventsPendingRelease--;
  }
}

void chreWifiScanCacheConfigureScanMonitor(bool enable) {
  if (!chreWifiScanCacheIsInitialized()) {
    return;
  }

  gScanMonitoringEnabled = enable;
}
