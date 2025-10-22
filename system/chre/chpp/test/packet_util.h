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

/**
 * @file Utilities for working with raw CHPP packets in a test setting
 */

#include <array>
#include <cinttypes>
#include <iostream>
#include <vector>

#include <gtest/gtest.h>

#include "chpp/app.h"
#include "chpp/crc.h"
#include "chpp/transport.h"

namespace chpp::test {

// Note: the preamble is actually sent in the reverse byte order one might
// expect (0x68 'h', 0x43 'C'); the simplification below assumes little endian
constexpr uint16_t kPreamble =
    (CHPP_PREAMBLE_BYTE_SECOND << 8) | CHPP_PREAMBLE_BYTE_FIRST;

struct ChppEmptyPacket {
  uint16_t preamble;
  ChppTransportHeader header;
  ChppTransportFooter footer;
} CHPP_PACKED_ATTR;

struct ChppResetPacket {
  uint16_t preamble;
  ChppTransportHeader header;
  ChppTransportConfiguration config;
  ChppTransportFooter footer;
} CHPP_PACKED_ATTR;

struct ChppPacketPrefix {
  uint16_t preamble;
  ChppTransportHeader header;
  uint8_t payload[1];  // Variable size per header.length
} CHPP_PACKED_ATTR;

template <size_t kPayloadSize>
struct ChppPacketWithPayload {
  uint16_t preamble;
  ChppTransportHeader header;
  uint8_t payload[kPayloadSize];
  ChppTransportFooter footer;
} CHPP_PACKED_ATTR;

struct ChppPacketWithAppHeader {
  uint16_t preamble;
  ChppTransportHeader transportHeader;
  ChppAppHeader appHeader;
  uint8_t payload[];
};

// Utilities for packet creation -----------------------------------------------

//! Computes the CRC of one of the complete packet types defined above
template <typename PktType>
uint32_t computeCrc(const PktType &pkt) {
  return chppCrc32(0, reinterpret_cast<const uint8_t *>(&pkt.header),
                   sizeof(pkt) - sizeof(pkt.preamble) - sizeof(pkt.footer));
}

ChppResetPacket generateResetPacket(uint8_t ackSeq = 0, uint8_t seq = 0,
                                    uint8_t error = CHPP_TRANSPORT_ERROR_NONE);
ChppResetPacket generateResetAckPacket(uint8_t ackSeq = 1, uint8_t seq = 0);
ChppEmptyPacket generateEmptyPacket(uint8_t ackSeq = 1, uint8_t seq = 0,
                                    uint8_t error = CHPP_TRANSPORT_ERROR_NONE);

//! Create an empty ACK packet for the given packet
ChppEmptyPacket generateAck(const std::vector<uint8_t> &pkt);

//! Create a packet with payload of the given size. If a payload array is not
//! provided, it is set to all-zeros.
template <size_t kPayloadSize>
ChppPacketWithPayload<kPayloadSize> generatePacketWithPayload(
    uint8_t ackSeq = 0, uint8_t seq = 0,
    const std::span<uint8_t, kPayloadSize> *payload = nullptr) {
  // clang-format off
  ChppPacketWithPayload<kPayloadSize> pkt = {
    .preamble = kPreamble,
    .header = {
      .flags = CHPP_TRANSPORT_FLAG_FINISHED_DATAGRAM,
      .packetCode = static_cast<uint8_t>(CHPP_ATTR_AND_ERROR_TO_PACKET_CODE(
          CHPP_TRANSPORT_ATTR_NONE, CHPP_TRANSPORT_ERROR_NONE)),
      .ackSeq = ackSeq,
      .seq = seq,
      .length = kPayloadSize,
      .reserved = 0,
    },
  };
  // clang-format on
  if (payload != nullptr) {
    std::memcpy(pkt.payload, payload->data(), sizeof(pkt.payload));
  }
  pkt.footer.checksum = computeCrc(pkt);
  return pkt;
}

// Utilities for packet parsing ------------------------------------------------

inline const ChppEmptyPacket &asEmptyPacket(const std::vector<uint8_t> &pkt) {
  EXPECT_EQ(pkt.size(), sizeof(ChppEmptyPacket));
  return *reinterpret_cast<const ChppEmptyPacket *>(pkt.data());
}

inline const ChppResetPacket &asResetPacket(const std::vector<uint8_t> &pkt) {
  EXPECT_EQ(pkt.size(), sizeof(ChppResetPacket));
  return *reinterpret_cast<const ChppResetPacket *>(pkt.data());
}

inline const ChppPacketPrefix &asChpp(const std::vector<uint8_t> &pkt) {
  EXPECT_GE(pkt.size(), sizeof(ChppEmptyPacket));
  return *reinterpret_cast<const ChppPacketPrefix *>(pkt.data());
}

inline const ChppTransportHeader &getHeader(const std::vector<uint8_t> &pkt) {
  static_assert(CHPP_PREAMBLE_LEN_BYTES == sizeof(uint16_t));
  EXPECT_GE(pkt.size(), sizeof(uint16_t) + sizeof(ChppTransportHeader));
  return *reinterpret_cast<const ChppTransportHeader *>(&pkt[sizeof(uint16_t)]);
}

inline const ChppPacketWithAppHeader &asApp(const std::vector<uint8_t> &pkt) {
  EXPECT_GE(pkt.size(),
            sizeof(ChppPacketWithAppHeader) + sizeof(ChppTransportFooter));
  return *reinterpret_cast<const ChppPacketWithAppHeader *>(pkt.data());
}

// Utilities for debugging -----------------------------------------------------

const char *appErrorCodeToStr(uint8_t error);
const char *appMessageTypeToStr(uint8_t type);
const char *handleToStr(uint8_t handle);
const char *packetAttrToStr(uint8_t attr);
const char *transportErrorToStr(uint8_t error);

//! Tuned for outputting a raw binary buffer (e.g. payload or full packet)
void dumpRaw(std::ostream &os, const void *ptr, size_t len);

void dumpPreamble(std::ostream &os, uint16_t preamble);
void dumpHeader(std::ostream &os, const ChppTransportHeader &hdr);
void dumpConfig(std::ostream &os, const ChppTransportConfiguration &cfg);

template <typename PktType>
void dumpFooter(std::ostream &os, const PktType &pkt) {
  os << "CRC: 0x" << std::hex << pkt.footer.checksum;
  uint32_t computed = computeCrc(pkt);
  if (pkt.footer.checksum != computed) {
    os << " (invalid, expected " << computed << ")";
  } else {
    os << " (ok)";
  }
  os << std::endl;
}

void dumpEmptyPacket(std::ostream &os, const ChppEmptyPacket &pkt);
void dumpResetPacket(std::ostream &os, const ChppResetPacket &pkt);
void dumpPacket(std::ostream &os, const ChppPacketPrefix &pkt);

std::ostream &operator<<(std::ostream &os, const ChppEmptyPacket &pkt);
std::ostream &operator<<(std::ostream &os, const ChppResetPacket &pkt);
std::ostream &operator<<(std::ostream &os, const ChppPacketPrefix &pkt);

// Utilities for gtest packet checking -----------------------------------------

//! Confirms that the supplied packet has a valid preamble, CRC, length, etc.,
//! raising a gtest failure (via EXPECT_*) if not
void checkPacketValidity(std::vector<uint8_t> &received);

// These return true if the packets are the same, false otherwise

bool comparePacketHeader(const ChppTransportHeader &rx,
                         const ChppTransportHeader &expected);

bool comparePacket(const std::vector<uint8_t> &received,
                   const ChppEmptyPacket &expected);
bool comparePacket(const std::vector<uint8_t> &received,
                   const ChppResetPacket &expected);

}  // namespace chpp::test