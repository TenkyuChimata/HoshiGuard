#include <unity.h>

#include <string.h>

#include "dcr_dcx_decoder.h"
#include "qzss_l1s.h"
#include "ubx_parser.h"

namespace {
const uint8_t kDcxSample[32] = {
    0xC6, 0xB0, 0x60, 0x0D, 0xE2, 0xBF, 0x88, 0xE9,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xC0, 0x00, 0x00, 0x11, 0xA1, 0xC8, 0x4E, 0xD4};

void makeSfrbxPayload(uint8_t payload[40]) {
  memset(payload, 0, 40);
  payload[0] = 5;
  payload[1] = 4;
  payload[2] = 1;
  payload[4] = 8;
  payload[6] = 2;
  for (size_t word = 0; word < 8U; ++word) {
    for (size_t byte = 0; byte < 4U; ++byte) {
      payload[8U + word * 4U + byte] =
          kDcxSample[word * 4U + (3U - byte)];
    }
  }
}
}

void test_sfrbx_header_parsing() {
  uint8_t payload[40];
  makeSfrbxPayload(payload);
  SfrbxHeader header = {};
  TEST_ASSERT_TRUE(parseSfrbxHeader(payload, sizeof(payload), header));
  TEST_ASSERT_EQUAL_UINT8(5, header.gnssId);
  TEST_ASSERT_EQUAL_UINT8(1, header.sigId);
  TEST_ASSERT_EQUAL_UINT8(8, header.numWords);
  TEST_ASSERT_FALSE(parseSfrbxHeader(payload, sizeof(payload) - 1U, header));
}

void test_l1s_endian_conversion_and_end_to_end_ubx() {
  uint8_t payload[40];
  makeSfrbxPayload(payload);
  uint8_t ubxData[64];
  const size_t ubxLength = UbxParser::buildPacket(0x02, 0x13, payload,
                                                  sizeof(payload), ubxData,
                                                  sizeof(ubxData));
  UbxParser parser;
  UbxPacketView packet = {};
  bool found = false;
  for (size_t i = 0; i < ubxLength; ++i)
    found |= parser.feed(ubxData[i], packet);
  TEST_ASSERT_TRUE(found);
  QzssL1sFrame frame = {};
  TEST_ASSERT_TRUE(extractQzssL1s(packet.payload, packet.length, frame));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kDcxSample, frame.raw, sizeof(kDcxSample));
  TEST_ASSERT_EQUAL_UINT8(44, frame.messageType);
}

void test_pab_and_message_type() {
  uint8_t payload[40];
  makeSfrbxPayload(payload);
  QzssL1sFrame frame = {};
  TEST_ASSERT_TRUE(extractQzssL1s(payload, sizeof(payload), frame));
  TEST_ASSERT_EQUAL_HEX8(0xC6, frame.pab);
  TEST_ASSERT_TRUE(frame.validPab);
  TEST_ASSERT_EQUAL_UINT8(44, frame.messageType);
}

void test_bit_reader_250_bit_boundary() {
  BitReader reader(kDcxSample, 250U);
  uint32_t value = 0;
  TEST_ASSERT_TRUE(reader.read(218U, 32U, value));
  TEST_ASSERT_FALSE(reader.read(219U, 32U, value));
  TEST_ASSERT_TRUE(reader.read(250U, 0U, value));
  TEST_ASSERT_FALSE(reader.read(250U, 1U, value));
  TEST_ASSERT_FALSE(reader.read(0U, 33U, value));
}

void test_l1s_deduplication_ignores_padding() {
  L1sDeduplicator dedup;
  uint8_t otherPadding[32];
  memcpy(otherPadding, kDcxSample, sizeof(otherPadding));
  otherPadding[31] ^= 0x3FU;
  TEST_ASSERT_FALSE(dedup.checkAndRemember(kDcxSample, 100U, 10000000U));
  TEST_ASSERT_TRUE(dedup.checkAndRemember(otherPadding, 200U, 10000000U));
  TEST_ASSERT_FALSE(dedup.checkAndRemember(kDcxSample, 11000000U, 10000000U));
}

void test_l1s_crc_official_polynomial_sample_vector() {
  // IS-QZSS-L1S-009 の多項式を、提供された完全フレームで独立照合済み。格納 bit 226..249 は 0x87213B。
  TEST_ASSERT_EQUAL_HEX32(0x87213B, qzssL1sCrc24(kDcxSample));
  TEST_ASSERT_EQUAL_HEX32(0x87213B, qzssL1sReceivedCrc(kDcxSample));
  uint8_t corrupt[32];
  memcpy(corrupt, kDcxSample, sizeof(corrupt));
  corrupt[10] ^= 0x01U;
  TEST_ASSERT_NOT_EQUAL(qzssL1sReceivedCrc(corrupt), qzssL1sCrc24(corrupt));
}

void test_dcx_common_fields() {
  QzssL1sFrame frame = {};
  memcpy(frame.raw, kDcxSample, sizeof(frame.raw));
  frame.messageType = 44;
  DcxDecodeResult result = {};
  TEST_ASSERT_TRUE(decodeDcxMt44(frame, result));
  TEST_ASSERT_TRUE(result.supported);
  TEST_ASSERT_EQUAL_UINT8(44, frame.messageType);
}

void run_qzss_l1s_tests() {
  RUN_TEST(test_sfrbx_header_parsing);
  RUN_TEST(test_l1s_endian_conversion_and_end_to_end_ubx);
  RUN_TEST(test_pab_and_message_type);
  RUN_TEST(test_bit_reader_250_bit_boundary);
  RUN_TEST(test_l1s_deduplication_ignores_padding);
  RUN_TEST(test_l1s_crc_official_polynomial_sample_vector);
  RUN_TEST(test_dcx_common_fields);
}
