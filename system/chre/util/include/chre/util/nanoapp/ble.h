/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef CHRE_UTIL_NANOAPP_BLE_H_
#define CHRE_UTIL_NANOAPP_BLE_H_

#include <inttypes.h>
#include <cstdint>

#include "chre_api/chre.h"

namespace chre {

namespace ble_constants {

/**
 * The minimum threshold for RSSI. Used to filter out RSSI values below this.
 */
constexpr int8_t kRssiThreshold = -128;

/**
 * The length of the UUID data at the beginning of the data in the BLE packet.
 */
constexpr uint16_t kGoogleUuidDataLength = 2;

/** The mask to get the UUID from the data in the BLE packet. */
constexpr uint8_t kGoogleUuidMask[kGoogleUuidDataLength] = {0xFF, 0xFF};

/** The Google Eddystone BLE beacon UUID. */
constexpr uint8_t kGoogleEddystoneUuid[kGoogleUuidDataLength] = {0xAA, 0xFE};

/** The Google Nearby Fastpair BLE beacon UUID. */
constexpr uint8_t kGoogleNearbyFastpairUuid[kGoogleUuidDataLength] = {0x2C,
                                                                      0xFE};
/** Length of Google manufacturer data filter. */
constexpr uint16_t kGoogleManufactureDataLength = 4;

/**
 * The public address of the known (bonded) BLE advertiser in big endian byte
 * order. Change this address to the public identity address of the advertiser
 * in the test.
 *
 * Example: To filter on the address (01:02:03:AB:CD:EF), use
 * {0x01:0x02:0x03:0xAB:0xCD:0xEF}.
 */
constexpr uint8_t kBroadcasterAddress[CHRE_BLE_ADDRESS_LEN] = {
    0x01, 0x02, 0x03, 0xAB, 0xCD, 0xEF};

/** The Google manufacturer ID followed by some data. */
constexpr uint8_t kGoogleManufactureData[kGoogleManufactureDataLength] = {
    0xE0, 0x00, 0xAA, 0xFE};

/** Manufacturer data filter mask. */
constexpr uint8_t kGoogleManufactureDataMask[kGoogleManufactureDataLength] = {
    0xFF, 0xFF, 0xFF, 0xFF};

/** The number of generic filters (equal to the number of known beacons). */
constexpr uint8_t kNumScanFilters = 2;
/** The number of manufacturer data filters. */
constexpr uint8_t kNumManufacturerDataFilters = 1;

/**
 * The number of broadcaster address filters (equal to the number of known
 * public advertiser addresses).
 */
constexpr uint8_t kNumBroadcasterFilters = 1;
}  // namespace ble_constants

/**
 * Create a BLE generic filter object.
 *
 * @param type                              the filter type.
 * @param len                               the filter length.
 * @param data                              the filter data.
 * @param mask                              the filter mask.
 * @return                                  the filter.
 */
chreBleGenericFilter createBleGenericFilter(uint8_t type, uint8_t len,
                                            const uint8_t *data,
                                            const uint8_t *mask);

/**
 * Creates a chreBleScanFilter that filters for the Google eddystone UUID,
 * the Google nearby fastpair UUID, and a RSSI threshold of kRssiThreshold.
 *
 * @param filter                            (out) the output filter.
 * @param genericFilters                    (out) the output generic filters
 * array.
 * @param numGenericFilters                 the size of the generic filters
 * array. must be >= kNumScanFilters.
 *
 * @return true                             the operation was successful
 * @return false                            the operation was not successful
 */
bool createBleScanFilterForKnownBeacons(struct chreBleScanFilter &filter,
                                        chreBleGenericFilter *genericFilters,
                                        uint8_t numGenericFilters);

/**
 * Similar to createBleScanFilterForKnownBeacons but creates a
 * chreBleScanFilterV1_9 instead of a chreBleScanFilter. The
 * broadcasterAddressFilters are set to empty.
 */
bool createBleScanFilterForKnownBeaconsV1_9(
    struct chreBleScanFilterV1_9 &filter, chreBleGenericFilter *genericFilters,
    uint8_t numGenericFilters);

/**
 * Creates a chreBleScanFilterV1_9 that filters for advertisements with the
 * manufacturer data in kGoogleManufactureData.
 *
 * @param numGenericFilters         The size of the generic filters array. Must
 * be >= kNumManufacturerDataFilters.
 * @param genericFilters            (out) The output generic filters.
 * @param filter                    (out) The output filter.
 *
 * @return true if the filter was created successfully.
 */
bool createBleManufacturerDataFilter(uint8_t numGenericFilters,
                                     chreBleGenericFilter *genericFilters,
                                     struct chreBleScanFilterV1_9 &filter);

/**
 * Creates a chreBleScanFilter that filters for the Google eddystone UUID,
 * the Google nearby fastpair UUID, public identity address of a bonded device,
 * and a RSSI threshold of kRssiThreshold.
 *
 * @param filter                   (out) the output filter.
 * @param broadcasterFilters       (out) the output broadcaster address filters
 * array.
 * @param numBroadcasterFilters    the size of the broadcaster address filters
 * array. must be >= kNumBroadcasterFilters.
 *
 * @return true                    the operation was successful
 * @return false                   the operation was not successful
 */
bool createBleScanFilterForAdvertiser(
    struct chreBleScanFilterV1_9 &filter,
    chreBleBroadcasterAddressFilter *broadcasterFilters,
    uint8_t numBroadcasterFilters);
}  // namespace chre

#endif  // CHRE_UTIL_NANOAPP_BLE_H_
