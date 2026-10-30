#include <unity.h>

#include <string.h>

#include "ubx_parser.h"

namespace {
size_t makePacket(uint8_t cls, uint8_t id, const uint8_t *payload,
                  uint16_t length, uint8_t *packet, size_t capacity) {
  return UbxParser::buildPacket(cls, id, payload, length, packet, capacity);
}
}

void test_ubx_correct_packet() {
  const uint8_t payload[] = {0x01, 0x02, 0xA5};
  uint8_t data[32];
  const size_t length = makePacket(0x05, 0x01, payload, sizeof(payload),
                                   data, sizeof(data));
  UbxParser parser;
  UbxPacketView packet = {};
  bool found = false;
  for (size_t i = 0; i < length; ++i) found |= parser.feed(data[i], packet);
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_HEX8(0x05, packet.messageClass);
  TEST_ASSERT_EQUAL_HEX8(0x01, packet.messageId);
  TEST_ASSERT_EQUAL_UINT16(3, packet.length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, packet.payload, sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(1, parser.stats().validPackets);
}

void test_ubx_bad_checksums() {
  const uint8_t payload[] = {0x06, 0x8A};
  uint8_t data[32];
  const size_t length = makePacket(0x05, 0x01, payload, sizeof(payload),
                                   data, sizeof(data));
  for (uint8_t which = 0; which < 2U; ++which) {
    UbxParser parser;
    UbxPacketView packet = {};
    data[length - 2U + which] ^= 0x01U;
    bool found = false;
    for (size_t i = 0; i < length; ++i) found |= parser.feed(data[i], packet);
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_UINT32(1, parser.stats().checksumErrors);
    data[length - 2U + which] ^= 0x01U;
  }
}

void test_ubx_split_packet() {
  uint8_t data[16];
  const size_t length = makePacket(0x0A, 0x04, 0, 0, data, sizeof(data));
  UbxParser parser;
  UbxPacketView packet = {};
  for (size_t i = 0; i < 3; ++i)
    TEST_ASSERT_FALSE(parser.feed(data[i], packet));
  bool found = false;
  for (size_t i = 3; i < length; ++i) found |= parser.feed(data[i], packet);
  TEST_ASSERT_TRUE(found);
}

void test_ubx_noise_and_resync() {
  uint8_t data[16];
  const size_t length = makePacket(0x0A, 0x04, 0, 0, data, sizeof(data));
  const uint8_t noise[] = {0x24, 0x47, 0xB5, 0x00, 0x7F, 0xB5};
  UbxParser parser;
  UbxPacketView packet = {};
  for (size_t i = 0; i < sizeof(noise); ++i) parser.feed(noise[i], packet);
  bool found = false;
  // ノイズ末尾が B5 のため、packet の 2 番目の同期 byte から開始する。
  for (size_t i = 1; i < length; ++i) found |= parser.feed(data[i], packet);
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_GREATER_THAN_UINT32(0, parser.stats().resyncs);
}

void test_ubx_continuous_packets() {
  uint8_t first[16], second[16];
  const size_t firstLength = makePacket(0x0A, 0x04, 0, 0,
                                        first, sizeof(first));
  const size_t secondLength = makePacket(0x01, 0x07, 0, 0,
                                         second, sizeof(second));
  UbxParser parser;
  UbxPacketView packet = {};
  uint8_t count = 0;
  for (size_t i = 0; i < firstLength; ++i)
    if (parser.feed(first[i], packet)) ++count;
  for (size_t i = 0; i < secondLength; ++i)
    if (parser.feed(second[i], packet)) ++count;
  TEST_ASSERT_EQUAL_UINT8(2, count);
}

void test_ubx_oversized_payload() {
  const uint16_t oversized = UBX_MAX_PAYLOAD + 1U;
  const uint8_t header[] = {0xB5, 0x62, 0x02, 0x13,
                            static_cast<uint8_t>(oversized),
                            static_cast<uint8_t>(oversized >> 8)};
  UbxParser parser;
  UbxPacketView packet = {};
  for (size_t i = 0; i < sizeof(header); ++i) parser.feed(header[i], packet);
  TEST_ASSERT_EQUAL_UINT32(1, parser.stats().oversizedPackets);
}

void run_ubx_parser_tests() {
  RUN_TEST(test_ubx_correct_packet);
  RUN_TEST(test_ubx_bad_checksums);
  RUN_TEST(test_ubx_split_packet);
  RUN_TEST(test_ubx_noise_and_resync);
  RUN_TEST(test_ubx_continuous_packets);
  RUN_TEST(test_ubx_oversized_payload);
}

