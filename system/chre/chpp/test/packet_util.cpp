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

#include "packet_util.h"

#include "chpp/app.h"

#include <cstring>

namespace chpp::test {

// Utilities for packet creation -----------------------------------------------

ChppEmptyPacket generateEmptyPacket(uint8_t ackSeq, uint8_t seq,
                                    uint8_t error) {
  // clang-format off
  ChppEmptyPacket pkt = {
    .preamble = kPreamble,
    .header = {
      .flags = CHPP_TRANSPORT_FLAG_FINISHED_DATAGRAM,
      .packetCode = static_cast<uint8_t>(CHPP_ATTR_AND_ERROR_TO_PACKET_CODE(
          CHPP_TRANSPORT_ATTR_NONE, error)),
      .ackSeq = ackSeq,
      .seq = seq,
      .length = 0,
      .reserved = 0,
    },
  };
  // clang-format on
  pkt.footer.checksum = computeCrc(pkt);
  return pkt;
}

ChppResetPacket generateResetPacket(uint8_t ackSeq, uint8_t seq,
                                    uint8_t error) {
  // clang-format off
  ChppResetPacket pkt = {
    .preamble = kPreamble,
    .header = {
      .flags = CHPP_TRANSPORT_FLAG_FINISHED_DATAGRAM,
      .packetCode = static_cast<uint8_t>(CHPP_ATTR_AND_ERROR_TO_PACKET_CODE(
          CHPP_TRANSPORT_ATTR_RESET,
          error
      )),
      .ackSeq = ackSeq,
      .seq = seq,
      .length = sizeof(ChppTransportConfiguration),
      .reserved = 0,
    },
    .config = {
      .version = {
        .major = 1,
        .minor = 0,
        .patch = 0,
      },
      .reserved1 = 0,
      .reserved2 = 0,
      .reserved3 = 0,
    }
  };
  // clang-format on
  pkt.footer.checksum = computeCrc(pkt);
  return pkt;
}

ChppResetPacket generateResetAckPacket(uint8_t ackSeq, uint8_t seq) {
  ChppResetPacket pkt = generateResetPacket(ackSeq, seq);
  pkt.header.packetCode =
      static_cast<uint8_t>(CHPP_ATTR_AND_ERROR_TO_PACKET_CODE(
          CHPP_TRANSPORT_ATTR_RESET_ACK, CHPP_TRANSPORT_ERROR_NONE));
  pkt.footer.checksum = computeCrc(pkt);
  return pkt;
}

ChppEmptyPacket generateAck(const std::vector<uint8_t> &pkt) {
  // An ACK consists of an empty packet with the ackSeq set to the received
  // packet's seq + 1 (since ackSeq indicates the next seq value we expect), and
  // seq set to the received packet's ackSeq - 1 (since we don't increment seq
  // on empty packets and ackSeq indicates the next expected seq)
  const ChppTransportHeader &hdr = getHeader(pkt);
  return generateEmptyPacket(/*acqSeq=*/hdr.seq + 1, /*seq=*/hdr.ackSeq - 1);
}

// Utilities for debugging -----------------------------------------------------

const char *appErrorCodeToStr(uint8_t error) {
  switch (error) {
    case CHPP_APP_ERROR_NONE:
      return "NONE";
    case CHPP_APP_ERROR_INVALID_COMMAND:
      return "INVALID_COMMAND";
    case CHPP_APP_ERROR_INVALID_ARG:
      return "INVALID_ARG";
    case CHPP_APP_ERROR_BUSY:
      return "BUSY";
    case CHPP_APP_ERROR_OOM:
      return "OOM";
    case CHPP_APP_ERROR_UNSUPPORTED:
      return "UNSUPPORTED";
    case CHPP_APP_ERROR_TIMEOUT:
      return "TIMEOUT";
    case CHPP_APP_ERROR_DISABLED:
      return "DISABLED";
    case CHPP_APP_ERROR_RATELIMITED:
      return "RATELIMITED";
    case CHPP_APP_ERROR_BLOCKED:
      return "BLOCKED";
    case CHPP_APP_ERROR_INVALID_LENGTH:
      return "INVALID_LENGTH";
    case CHPP_APP_ERROR_NOT_READY:
      return "NOT_READY";
    case CHPP_APP_ERROR_BEYOND_CHPP:
      return "BEYOND_CHPP";
    case CHPP_APP_ERROR_UNEXPECTED_RESPONSE:
      return "UNEXPECTED_RESPONSE";
    case CHPP_APP_ERROR_CONVERSION_FAILED:
      return "CONVERSION_FAILED";
    case CHPP_APP_ERROR_UNSPECIFIED:
      return "UNSPECIFIED";
    default:
      return "UNKNOWN";
  }
}

const char *appMessageTypeToStr(uint8_t type) {
  switch (type) {
    case CHPP_MESSAGE_TYPE_CLIENT_REQUEST:
      return "CLIENT_REQ";
    case CHPP_MESSAGE_TYPE_SERVICE_RESPONSE:
      return "SERVICE_RESP";
    case CHPP_MESSAGE_TYPE_CLIENT_NOTIFICATION:
      return "CLIENT_NOTIF";
    case CHPP_MESSAGE_TYPE_SERVICE_NOTIFICATION:
      return "SERVICE_NOTIF";
    case CHPP_MESSAGE_TYPE_SERVICE_REQUEST:
      return "SERVICE_REQ";
    case CHPP_MESSAGE_TYPE_CLIENT_RESPONSE:
      return "CLIENT_RESP";
    default:
      return "UNKNOWN";
  }
}

const char *handleToStr(uint8_t handle) {
  switch (handle) {
    case CHPP_HANDLE_NONE:
      return "(NONE)";
    case CHPP_HANDLE_LOOPBACK:
      return "(LOOPBACK)";
    case CHPP_HANDLE_TIMESYNC:
      return "(TIMESYNC)";
    case CHPP_HANDLE_DISCOVERY:
      return "(DISCOVERY)";
    default:
      return "";
  }
}

const char *packetAttrToStr(uint8_t attr) {
  switch (attr) {
    case CHPP_TRANSPORT_ATTR_NONE:
      return "none";
    case CHPP_TRANSPORT_ATTR_RESET:
      return "reset";
    case CHPP_TRANSPORT_ATTR_RESET_ACK:
      return "reset-ack";
    case CHPP_TRANSPORT_ATTR_LOOPBACK_REQUEST:
      return "loopback-req";
    case CHPP_TRANSPORT_ATTR_LOOPBACK_RESPONSE:
      return "loopback-rsp";
    default:
      return "invalid";
  }
}

const char *transportErrorToStr(uint8_t error) {
  switch (error) {
    case CHPP_TRANSPORT_ERROR_NONE:
      return "none";
    case CHPP_TRANSPORT_ERROR_CHECKSUM:
      return "checksum";
    case CHPP_TRANSPORT_ERROR_OOM:
      return "oom";
    case CHPP_TRANSPORT_ERROR_BUSY:
      return "busy";
    case CHPP_TRANSPORT_ERROR_HEADER:
      return "header";
    case CHPP_TRANSPORT_ERROR_ORDER:
      return "order";
    case CHPP_TRANSPORT_ERROR_TIMEOUT:
      return "timeout";
    case CHPP_TRANSPORT_ERROR_MAX_RETRIES:
      return "max-retries";
    case CHPP_TRANSPORT_ERROR_APPLAYER:
      return "app-layer";
    default:
      return "invalid";
  }
}

void dumpRaw(std::ostream &os, const void *ptr, size_t len) {
  const uint8_t *buffer = static_cast<const uint8_t *>(ptr);
  char line[32];
  char lineChars[32];
  size_t offset = 0;
  size_t offsetChars = 0;

  for (size_t i = 1; i <= len; i++) {
    // This ignores potential errors returned by snprintf. This is a relatively
    // simple case and the deliberate decision to ignore them has been made.
    offset += static_cast<size_t>(
        snprintf(&line[offset], sizeof(line) - offset, "%02x ", buffer[i - 1]));
    offsetChars += static_cast<size_t>(
        snprintf(&lineChars[offsetChars], sizeof(lineChars) - offsetChars, "%c",
                 (isprint(buffer[i - 1])) ? buffer[i - 1] : '.'));
    if ((i % 8) == 0) {
      os << "  " << line << "\t" << lineChars << std::endl;
      offset = 0;
      offsetChars = 0;
    } else if ((i % 4) == 0) {
      offset += static_cast<size_t>(
          snprintf(&line[offset], sizeof(line) - offset, " "));
    }
  }

  if (offset > 0) {
    char tabs[8];
    char *pos = tabs;
    while (offset < 28) {
      *pos++ = '\t';
      offset += 8;
    }
    *pos = '\0';
    os << "  " << line << tabs << lineChars << std::endl;
  }
}

void dumpPreamble(std::ostream &os, uint16_t preamble) {
  const char *p = reinterpret_cast<const char *>(&preamble);
  os << std::endl
     << "Preamble: 0x" << std::hex << preamble << " \"" << p[0] << p[1] << "\"";
  if (preamble == kPreamble) {
    os << " (ok)";
  } else {
    os << " (invalid -- expected 0x" << std::hex << kPreamble << ")";
  }
  os << std::endl;
}

void dumpHeader(std::ostream &os, const ChppTransportHeader &hdr) {
  os << "Header {" << std::endl
     << "  flags: 0x" << std::hex << (unsigned)hdr.flags;
  if (hdr.flags & CHPP_TRANSPORT_FLAG_UNFINISHED_DATAGRAM) {
    os << " (unfinished)";
  } else {
    os << " (finished)";
  }
  uint8_t attr = CHPP_TRANSPORT_GET_ATTR(hdr.packetCode);
  uint8_t error = CHPP_TRANSPORT_GET_ERROR(hdr.packetCode);
  os << std::endl
     << "  packetCode: 0x" << std::hex << (unsigned)hdr.packetCode
     << " (attr: " << packetAttrToStr(attr)
     << " | error: " << transportErrorToStr(error) << ")" << std::endl;

  os << "  ackSeq: " << std::dec << (unsigned)hdr.ackSeq << std::endl
     << "  seq: " << std::dec << (unsigned)hdr.seq << std::endl
     << "  length: " << std::dec << hdr.length << std::endl
     << "  reserved: " << std::dec << hdr.reserved << std::endl
     << "}" << std::endl;
}

void dumpConfig(std::ostream &os, const ChppTransportConfiguration &cfg) {
  os << "Config {" << std::endl
     << "  version: " << std::dec << (unsigned)cfg.version.major << "."
     << std::dec << (unsigned)cfg.version.minor << "." << std::dec
     << cfg.version.patch << std::endl
     << "}" << std::endl;
}

void dumpEmptyPacket(std::ostream &os, const ChppEmptyPacket &pkt) {
  dumpPreamble(os, pkt.preamble);
  dumpHeader(os, pkt.header);
  dumpFooter(os, pkt);
}

void dumpResetPacket(std::ostream &os, const ChppResetPacket &pkt) {
  dumpPreamble(os, pkt.preamble);
  dumpHeader(os, pkt.header);
  dumpConfig(os, pkt.config);
  dumpFooter(os, pkt);
}

void dumpPacket(std::ostream &os, const ChppPacketPrefix &pkt) {
  dumpPreamble(os, pkt.preamble);
  dumpHeader(os, pkt.header);
  size_t payloadOffset = 0;
  if (CHPP_TRANSPORT_GET_ATTR(pkt.header.packetCode) ==
          CHPP_TRANSPORT_ATTR_NONE &&
      pkt.header.length >= sizeof(ChppAppHeader)) {
    auto &appHdr = reinterpret_cast<const ChppAppHeader &>(*pkt.payload);
    os << "AppHeader {" << std::endl;
    os << " handle: 0x" << std::hex << (unsigned)appHdr.handle << " "
       << handleToStr(appHdr.handle) << std::endl;
    os << " type: " << std::dec << (unsigned)appHdr.type << " ("
       << appMessageTypeToStr(appHdr.type) << ")" << std::endl;
    os << " transaction: " << std::dec << (unsigned)appHdr.transaction
       << std::endl;
    os << " error: " << std::dec << (unsigned)appHdr.error << " ("
       << appErrorCodeToStr(appHdr.error) << ")" << std::endl;
    os << " command: " << std::dec << (unsigned)appHdr.command << std::endl;
    os << "}" << std::endl;
    payloadOffset = sizeof(ChppAppHeader);
  }
  size_t payloadSize = pkt.header.length - payloadOffset;
  if (payloadSize > 0) {
    os << "Payload (size " << payloadSize << ") {" << std::endl;
    dumpRaw(os, &pkt.payload[payloadOffset], pkt.header.length - payloadOffset);
    os << "}" << std::endl;
  }

  const auto &footer = *reinterpret_cast<const ChppTransportFooter *>(
      &pkt.payload[pkt.header.length]);
  uint32_t crc = chppCrc32(0, reinterpret_cast<const uint8_t *>(&pkt.header),
                           sizeof(pkt.header) + pkt.header.length);
  os << "CRC: 0x" << std::hex << footer.checksum;
  if (footer.checksum != crc) {
    os << " (invalid, expected " << crc << ")";
  } else {
    os << " (ok)";
  }
  os << std::endl;
}

std::ostream &operator<<(std::ostream &os, const ChppEmptyPacket &pkt) {
  dumpEmptyPacket(os, pkt);
  return os;
}

std::ostream &operator<<(std::ostream &os, const ChppResetPacket &pkt) {
  dumpResetPacket(os, pkt);
  return os;
}

std::ostream &operator<<(std::ostream &os, const ChppPacketPrefix &pkt) {
  dumpPacket(os, pkt);
  return os;
}

// Utilities for gtest packet checking -----------------------------------------

void checkPacketValidity(std::vector<uint8_t> &received) {
  const ChppPacketPrefix &pkt = asChpp(received);
  EXPECT_GE(received.size(), sizeof(ChppEmptyPacket));
  EXPECT_EQ(pkt.preamble, kPreamble);

  constexpr size_t kFixedLenPortion =
      sizeof(pkt.preamble) + sizeof(pkt.header) + sizeof(ChppTransportFooter);
  EXPECT_EQ(pkt.header.length, received.size() - kFixedLenPortion);

  EXPECT_EQ(pkt.header.flags & CHPP_TRANSPORT_FLAG_RESERVED, 0);
  EXPECT_EQ(pkt.header.reserved, 0);

  uint8_t error = CHPP_TRANSPORT_GET_ERROR(pkt.header.packetCode);
  EXPECT_TRUE(error <= CHPP_TRANSPORT_SIGNAL_FORCE_RESET ||
              error == CHPP_TRANSPORT_ERROR_APPLAYER);
  uint8_t attrs = CHPP_TRANSPORT_GET_ATTR(pkt.header.packetCode);
  EXPECT_TRUE(attrs <= CHPP_TRANSPORT_ATTR_LOOPBACK_RESPONSE);

  uint32_t crc = chppCrc32(0, reinterpret_cast<const uint8_t *>(&pkt.header),
                           sizeof(pkt.header) + pkt.header.length);
  const auto *footer = reinterpret_cast<const ChppTransportFooter *>(
      &received[sizeof(pkt.preamble) + sizeof(pkt.header) + pkt.header.length]);
  EXPECT_EQ(footer->checksum, crc);
}

bool comparePacketHeader(const ChppTransportHeader &rx,
                         const ChppTransportHeader &expected) {
  EXPECT_EQ(rx.flags, expected.flags);
  EXPECT_EQ(rx.packetCode, expected.packetCode);
  EXPECT_EQ(rx.ackSeq, expected.ackSeq);
  EXPECT_EQ(rx.seq, expected.seq);
  EXPECT_EQ(rx.length, expected.length);
  EXPECT_EQ(rx.reserved, 0u);
  return (memcmp(&rx, &expected, sizeof(rx)) == 0);
}

bool comparePacket(const std::vector<uint8_t> &received,
                   const ChppEmptyPacket &expected) {
  EXPECT_EQ(received.size(), sizeof(expected));
  if (received.size() == sizeof(expected)) {
    const auto *rx = reinterpret_cast<const ChppEmptyPacket *>(received.data());
    EXPECT_EQ(rx->preamble, expected.preamble);
    comparePacketHeader(rx->header, expected.header);
    EXPECT_EQ(rx->footer.checksum, expected.footer.checksum);
  }
  return (received.size() == sizeof(expected) &&
          memcmp(received.data(), &expected, sizeof(expected)) == 0);
}

bool comparePacket(const std::vector<uint8_t> &received,
                   const ChppResetPacket &expected) {
  EXPECT_EQ(received.size(), sizeof(expected));
  if (received.size() == sizeof(expected)) {
    const auto *rx = reinterpret_cast<const ChppResetPacket *>(received.data());
    EXPECT_EQ(rx->preamble, expected.preamble);
    comparePacketHeader(rx->header, expected.header);
    EXPECT_EQ(rx->config.version.major, expected.config.version.major);
    EXPECT_EQ(rx->config.version.minor, expected.config.version.minor);
    EXPECT_EQ(rx->config.version.patch, expected.config.version.patch);
    EXPECT_EQ(rx->footer.checksum, expected.footer.checksum);
  }
  return (received.size() == sizeof(expected) &&
          memcmp(received.data(), &expected, sizeof(expected)) == 0);
}

}  // namespace chpp::test