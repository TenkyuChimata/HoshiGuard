#include "test_support.h"
#include "dcr_dcx_decoder.h"
#include "ubx_dispatcher.h"

namespace {
void assertRealL1s(const char *name, uint8_t pab, uint8_t mt) {
  const RealFixture &value = realFixture(name);
  UbxParser parser;
  UbxFrameDispatcher dispatcher;
  UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result,
                 1234567ULL, 9U);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, result.sfrbxPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, result.l1sFrames);
  TEST_ASSERT_TRUE(result.hasLastFrame);
  TEST_ASSERT_EQUAL_UINT32(9U, result.lastFrame.ppsSequence);
  TEST_ASSERT_EQUAL_UINT64(1234567ULL, result.lastFrame.receivedMicros);
  TEST_ASSERT_TRUE(result.lastFrame.validPab);
  TEST_ASSERT_TRUE(result.lastFrame.validCrc);
  TEST_ASSERT_EQUAL_UINT8(9U, result.lastFrame.numWords);
  TEST_ASSERT_TRUE(result.lastFrame.hasExtraWord);
  TEST_ASSERT_EQUAL_HEX8(pab, result.lastFrame.pab);
  TEST_ASSERT_EQUAL_UINT8(mt, result.lastFrame.messageType);
  TEST_ASSERT_EQUAL_HEX32(qzssL1sReceivedCrc(result.lastFrame.raw),
                          qzssL1sCrc24(result.lastFrame.raw));
}

void checksumPacket(uint8_t *packet, size_t length) {
  uint8_t a = 0U, b = 0U;
  UbxParser::checksum(packet + 2U, length - 4U, a, b);
  packet[length - 2U] = a;
  packet[length - 1U] = b;
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

void test_ubx_stream_reset_discards_partial_but_preserves_totals() {
  UbxParser parser;
  UbxPacketView packet = {};
  parser.feed(0xB5U, packet);
  parser.feed(0x62U, packet);
  parser.feed(0x01U, packet);
  parser.resetStreamState();
  uint8_t valid[8];
  TEST_ASSERT_EQUAL_UINT32(8U,
      UbxParser::buildPacket(0x0AU, 0x04U, 0, 0U, valid, sizeof(valid)));
  bool emitted = false;
  for (size_t i = 0U; i < sizeof(valid); ++i)
    emitted = parser.feed(valid[i], packet) || emitted;
  TEST_ASSERT_TRUE(emitted);
  TEST_ASSERT_EQUAL_UINT32(11U, parser.stats().receivedBytes);
  TEST_ASSERT_EQUAL_UINT32(1U, parser.stats().validPackets);
}

void test_qzss_l1ca_and_l1s_statistics_are_distinct() {
  uint8_t l1ca[64], l1s[64];
  const size_t l1caLength = makeSfrbxPacket(0U, l1ca, sizeof(l1ca));
  const size_t l1sLength = makeSfrbxPacket(1U, l1s, sizeof(l1s));
  UbxParser parser;
  UbxFrameDispatcher dispatcher;
  UbxInjectionResult result = {};
  injectUbxBytes(l1ca, l1caLength, parser, dispatcher, result);
  injectUbxBytes(l1s, l1sLength, parser, dispatcher, result, 1U, 0U);
  TEST_ASSERT_EQUAL_UINT32(2U, dispatcher.stats().qzssSfrbx);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().qzssL1caSfrbx);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().qzssL1sSfrbx);
}

void test_nmea_bytes_are_counted_separately() {
  const char sentence[] = "$GNTXT,01,01,01,test*00\r\n";
  UbxParser parser;
  UbxPacketView packet = {};
  for (size_t i = 0; i < sizeof(sentence) - 1U; ++i)
    parser.feed(static_cast<uint8_t>(sentence[i]), packet);
  TEST_ASSERT_EQUAL_UINT32(sizeof(sentence) - 1U,
                           parser.stats().nmeaBytesSkipped);
}

void test_real_qzss_l1ca_does_not_enter_l1s() {
  const RealFixture &value = realFixture("qzss_l1ca_negative");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().qzssL1caSfrbx);
}

void test_synthetic_non_qzss_does_not_enter_l1s() {
  const RealFixture &value = realFixture("non_qzss_negative");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(0U, dispatcher.stats().qzssSfrbx);
}

void test_synthetic_checksum_corruption_is_rejected_by_ubx_parser() {
  const RealFixture &value = realFixture("ubx_checksum_corrupted");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(0U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, parser.stats().checksumErrors);
}

void test_synthetic_sfrbx_length_mismatch_is_safely_rejected() {
  const RealFixture &value = realFixture("sfrbx_length_mismatch");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().invalidSfrbxLength);
}

void test_legacy_eight_word_path_has_no_extra_word() {
  const RealFixture &source = realFixture("qzss_l1s_mt43_pab53");
  uint8_t payload[40]; memcpy(payload, source.data + 6U, sizeof(payload));
  payload[4] = 8U;
  QzssL1sFrame frame = {};
  TEST_ASSERT_TRUE(extractQzssL1s(payload, sizeof(payload), frame));
  TEST_ASSERT_EQUAL_UINT8(8U, frame.numWords);
  TEST_ASSERT_FALSE(frame.hasExtraWord);
}

void test_seven_word_qzss_l1s_is_rejected_as_word_count() {
  uint8_t payload[36] = {};
  payload[0] = 5U; payload[2] = 1U; payload[4] = 7U; payload[6] = 2U;
  uint8_t packet[48];
  const size_t length = UbxParser::buildPacket(0x02U, 0x13U, payload,
      sizeof(payload), packet, sizeof(packet));
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(packet, length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().invalidWordCount);
}

void test_six_padding_bits_do_not_change_crc_or_received_crc() {
  const RealFixture &source = realFixture("qzss_l1s_mt44_pab53");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(source.data, source.length, parser, dispatcher, result);
  uint8_t changed[QZSS_L1S_RAW_BYTES];
  memcpy(changed, result.lastFrame.raw, sizeof(changed));
  changed[31] ^= 0x3FU;
  TEST_ASSERT_EQUAL_HEX32(qzssL1sCrc24(result.lastFrame.raw),
                          qzssL1sCrc24(changed));
  TEST_ASSERT_EQUAL_HEX32(qzssL1sReceivedCrc(result.lastFrame.raw),
                          qzssL1sReceivedCrc(changed));
}

void test_real_ninth_word_is_retained_but_not_in_250_bits() {
  const RealFixture &value = realFixture("qzss_l1s_mt43_pab53");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_TRUE(result.lastFrame.hasExtraWord);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(value.data + 46U,
                                result.lastFrame.extraWordUbx, 4U);
  BitReader reader(result.lastFrame.raw, QZSS_L1S_VALID_BITS);
  uint32_t bit = 0U;
  TEST_ASSERT_FALSE(reader.read(250U, 1U, bit));
}

void test_ten_word_qzss_sig1_is_rejected() {
  const RealFixture &source = realFixture("qzss_l1ca_negative");
  uint8_t packet[64];
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(packet), source.length);
  memcpy(packet, source.data, source.length);
  packet[8] = 1U;  // 実受信の 10 word payload を保持したまま sigId=1 とする。
  checksumPacket(packet, source.length);
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(packet, source.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().invalidWordCount);
}

void test_same_real_content_from_different_sv_is_deduplicated() {
  const RealFixture &source = realFixture("qzss_l1s_mt44_pab53");
  uint8_t packet[64]; memcpy(packet, source.data, source.length);
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(source.data, source.length, parser, dispatcher, result, 100U);
  packet[7] = static_cast<uint8_t>(packet[7] == 7U ? 2U : 7U);
  checksumPacket(packet, source.length);
  injectUbxBytes(packet, source.length, parser, dispatcher, result, 200U);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().duplicateFrames);
}

void test_ubx_correct_packet() {
  const uint8_t payload[] = {0x01, 0x02, 0xA5};
  uint8_t data[32];
  const size_t length = UbxParser::buildPacket(0x05, 0x01, payload, sizeof(payload),
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
  const size_t length = UbxParser::buildPacket(0x05, 0x01, payload, sizeof(payload),
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
  const size_t length = UbxParser::buildPacket(0x0A, 0x04, 0, 0, data, sizeof(data));
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
  const size_t length = UbxParser::buildPacket(0x0A, 0x04, 0, 0, data, sizeof(data));
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
  const size_t firstLength = UbxParser::buildPacket(0x0A, 0x04, 0, 0,
                                        first, sizeof(first));
  const size_t secondLength = UbxParser::buildPacket(0x01, 0x07, 0, 0,
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

void test_real_l1s_all_observed_mt_and_pab() {
  // 期待値を fixture のフラグから導出せず、元の全ケースを明示する。
  const struct { const char *name; uint8_t pab; uint8_t mt; } cases[] = {
      {"qzss_l1s_mt43_pab53", 0x53U, 43U},
      {"qzss_l1s_mt43_pab9a", 0x9AU, 43U},
      {"qzss_l1s_mt43_pabc6", 0xC6U, 43U},
      {"qzss_l1s_mt44_pab53", 0x53U, 44U},
      {"qzss_l1s_mt44_pab9a", 0x9AU, 44U},
      {"qzss_l1s_mt44_pabc6", 0xC6U, 44U}};
  for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
    assertRealL1s(cases[i].name, cases[i].pab, cases[i].mt);
  const char *slas[] = {"qzss_l1s_mt47", "qzss_l1s_mt48",
                        "qzss_l1s_mt49", "qzss_l1s_mt50"};
  for (size_t i = 0U; i < sizeof(slas) / sizeof(slas[0]); ++i)
    assertRealL1s(slas[i], realFixture(slas[i]).pab,
                   static_cast<uint8_t>(47U + i));
}

void run_transport_tests() {
  RUN_TEST(test_sfrbx_header_parsing);
  RUN_TEST(test_l1s_endian_conversion_and_end_to_end_ubx);
  RUN_TEST(test_pab_and_message_type);
  RUN_TEST(test_bit_reader_250_bit_boundary);
  RUN_TEST(test_l1s_deduplication_ignores_padding);
  RUN_TEST(test_l1s_crc_official_polynomial_sample_vector);
  RUN_TEST(test_dcx_common_fields);
  RUN_TEST(test_ubx_stream_reset_discards_partial_but_preserves_totals);
  RUN_TEST(test_qzss_l1ca_and_l1s_statistics_are_distinct);
  RUN_TEST(test_nmea_bytes_are_counted_separately);
  RUN_TEST(test_real_qzss_l1ca_does_not_enter_l1s);
  RUN_TEST(test_synthetic_non_qzss_does_not_enter_l1s);
  RUN_TEST(test_synthetic_checksum_corruption_is_rejected_by_ubx_parser);
  RUN_TEST(test_synthetic_sfrbx_length_mismatch_is_safely_rejected);
  RUN_TEST(test_legacy_eight_word_path_has_no_extra_word);
  RUN_TEST(test_seven_word_qzss_l1s_is_rejected_as_word_count);
  RUN_TEST(test_six_padding_bits_do_not_change_crc_or_received_crc);
  RUN_TEST(test_real_ninth_word_is_retained_but_not_in_250_bits);
  RUN_TEST(test_ten_word_qzss_sig1_is_rejected);
  RUN_TEST(test_same_real_content_from_different_sv_is_deduplicated);
  RUN_TEST(test_ubx_correct_packet);
  RUN_TEST(test_ubx_bad_checksums);
  RUN_TEST(test_ubx_split_packet);
  RUN_TEST(test_ubx_noise_and_resync);
  RUN_TEST(test_ubx_continuous_packets);
  RUN_TEST(test_ubx_oversized_payload);
  RUN_TEST(test_real_l1s_all_observed_mt_and_pab);
}
