#include <unity.h>

#include <string.h>

#include "gnss_config.h"
#include "raw_capture.h"
#include "time_sync.h"
#include "ubx_dispatcher.h"

namespace {
const uint8_t kDcxSampleV020[32] = {
    0xC6, 0xB0, 0x60, 0x0D, 0xE2, 0xBF, 0x88, 0xE9,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xC0, 0x00, 0x00, 0x11, 0xA1, 0xC8, 0x4E, 0xD4};

void makeSfrbx(uint8_t sigId, uint8_t payload[40]) {
  memset(payload, 0, 40U);
  payload[0] = 5U;
  payload[1] = 4U;
  payload[2] = sigId;
  payload[4] = 8U;
  payload[6] = 2U;
  for (size_t word = 0; word < 8U; ++word)
    for (size_t byte = 0; byte < 4U; ++byte)
      payload[8U + word * 4U + byte] =
          kDcxSampleV020[word * 4U + 3U - byte];
}

size_t makeSfrbxPacket(uint8_t sigId, uint8_t *packet, size_t capacity) {
  uint8_t payload[40];
  makeSfrbx(sigId, payload);
  return UbxParser::buildPacket(0x02U, 0x13U, payload, sizeof(payload),
                                packet, capacity);
}

void writeLe32(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}
}  // namespace

void test_cfg_valget_request_encoding() {
  uint8_t payload[8] = {};
  TEST_ASSERT_EQUAL_UINT32(8U, buildCfgValgetPayload(
      0x10310014UL, payload, sizeof(payload)));
  const uint8_t expected[] = {0, 0, 0, 0, 0x14, 0x00, 0x31, 0x10};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, payload, sizeof(expected));
}

void test_cfg_valget_response_parsing() {
  const uint8_t response[] = {1, 0, 0, 0, 0x01, 0x00, 0x52, 0x40,
                              0x00, 0xC2, 0x01, 0x00};
  uint32_t value = 0;
  TEST_ASSERT_TRUE(parseCfgValgetResponse(response, sizeof(response),
                                          CFG_UART1_BAUDRATE, 4U, value));
  TEST_ASSERT_EQUAL_UINT32(115200U, value);
  TEST_ASSERT_FALSE(parseCfgValgetResponse(response, sizeof(response) - 1U,
                                           CFG_UART1_BAUDRATE, 4U, value));
}

void test_config_match_skips_valset() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigAction::Skip),
      static_cast<int>(decideConfigAction(true, 1U, 1U, true)));
}

void test_config_mismatch_applies_valset() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigAction::Apply),
      static_cast<int>(decideConfigAction(true, 0U, 1U, true)));
}

void test_config_timeout_falls_back_to_valset() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigAction::FallbackApply),
      static_cast<int>(decideConfigAction(false, 0U, 1U, true)));
}

void test_optional_config_nak_is_not_fatal() {
  TEST_ASSERT_FALSE(isConfigFailureFatal(ConfigRequirement::Optional));
  TEST_ASSERT_TRUE(isConfigFailureFatal(ConfigRequirement::Required));
}

void test_complete_ubx_raw_serialization() {
  const uint8_t payload[] = {0x06, 0x8A};
  UbxPacketView view = {0x05, 0x01, sizeof(payload), payload};
  char line[128];
  const size_t length = formatUbxRawRecord(12345678U, view, line, sizeof(line));
  TEST_ASSERT_GREATER_THAN_UINT32(0U, length);
  TEST_ASSERT_NOT_NULL(strstr(line, "UBX_RAW timestamp_us=12345678 len=10 data=B56205010200"));
  TEST_ASSERT_EQUAL_CHAR('\n', line[length - 1U]);
}

void test_restore_ubx_packet_from_log_hex() {
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  UbxPacketView view = {0x0A, 0x04, sizeof(payload), payload};
  char line[128];
  TEST_ASSERT_NOT_EQUAL(0U, formatUbxRawRecord(99U, view, line, sizeof(line)));
  uint8_t recovered[32];
  size_t recoveredLength = 0U;
  uint64_t timestamp = 0U;
  TEST_ASSERT_TRUE(parseUbxRawRecord(line, recovered, sizeof(recovered),
                                     recoveredLength, timestamp));
  TEST_ASSERT_EQUAL_UINT64(99U, timestamp);
  TEST_ASSERT_EQUAL_UINT32(11U, recoveredLength);
}

void test_complete_ubx_offline_injection_uses_uart_path() {
  uint8_t packet[64];
  const size_t length = makeSfrbxPacket(1U, packet, sizeof(packet));
  UbxParser parser;
  UbxFrameDispatcher dispatcher;
  UbxInjectionResult result = {};
  injectUbxBytes(packet, length, parser, dispatcher, result, 456U, 7U);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, result.sfrbxPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, result.l1sFrames);
  TEST_ASSERT_TRUE(result.hasLastFrame);
  TEST_ASSERT_EQUAL_UINT8(44U, result.lastFrame.messageType);
  TEST_ASSERT_EQUAL_UINT32(7U, result.lastFrame.ppsSequence);
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

void test_pps_micros_wrap_is_safe() {
  PpsIntervalTracker tracker;
  const uint32_t first = 0xFFF00000UL;
  tracker.observe(1U, first);
  tracker.observe(2U, first + 1000000UL);
  TEST_ASSERT_EQUAL_UINT32(1000000U, tracker.stats().lastIntervalMicros);
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.stats().abnormalInterval);
}

void test_pps_loss_and_abnormal_period() {
  PpsIntervalTracker tracker;
  tracker.observe(1U, 100U);
  tracker.observe(3U, 2100100U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().abnormalInterval);
}

void test_nav_pvt_pps_association_is_tentative() {
  uint8_t payload[92] = {};
  writeLe32(payload, 123000U);
  payload[4] = 0xEA; payload[5] = 0x07;  // 2026
  payload[6] = 7; payload[7] = 14;
  payload[8] = 12; payload[9] = 34; payload[10] = 56;
  payload[11] = 0x07;
  PpsUtcAssociator associator;
  associator.observePps(42U, 0xFFFFFF00UL);
  NavPvtRecord record = {};
  TEST_ASSERT_TRUE(associator.observeNavPvt(payload, sizeof(payload),
                                            0x00010000UL, record));
  TEST_ASSERT_TRUE(record.utcValid);
  TEST_ASSERT_TRUE(record.ppsAssociated);
  TEST_ASSERT_EQUAL_UINT32(42U, record.nearestPpsSequence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::UtcPpsTentative),
                        static_cast<int>(record.quality));
}

void test_log_buffer_boundary_rejected_without_overwrite() {
  QzssL1sFrame frame = {};
  memcpy(frame.raw, kDcxSampleV020, sizeof(frame.raw));
  char output[80];
  memset(output, 0x5A, sizeof(output));
  TEST_ASSERT_EQUAL_UINT32(0U, formatL1sRecord(frame, output, 32U));
  TEST_ASSERT_EQUAL_HEX8(0x5A, static_cast<uint8_t>(output[79]));
}

void test_oversized_raw_record_never_overflows() {
  uint8_t payload[UBX_MAX_PAYLOAD];
  memset(payload, 0xA5, sizeof(payload));
  UbxPacketView view = {0xFF, 0xEE, sizeof(payload), payload};
  char output[RAW_CAPTURE_MAX_RECORD + 1U];
  output[RAW_CAPTURE_MAX_RECORD] = 'Q';
  const size_t length = formatUbxRawRecord(0xFFFFFFFFFFFFFFFFULL, view,
                                           output, RAW_CAPTURE_MAX_RECORD);
  TEST_ASSERT_GREATER_THAN_UINT32(0U, length);
  TEST_ASSERT_LESS_THAN_UINT32(RAW_CAPTURE_MAX_RECORD, length);
  TEST_ASSERT_EQUAL_CHAR('Q', output[RAW_CAPTURE_MAX_RECORD]);
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

void run_v020_tests() {
  RUN_TEST(test_cfg_valget_request_encoding);
  RUN_TEST(test_cfg_valget_response_parsing);
  RUN_TEST(test_config_match_skips_valset);
  RUN_TEST(test_config_mismatch_applies_valset);
  RUN_TEST(test_config_timeout_falls_back_to_valset);
  RUN_TEST(test_optional_config_nak_is_not_fatal);
  RUN_TEST(test_complete_ubx_raw_serialization);
  RUN_TEST(test_restore_ubx_packet_from_log_hex);
  RUN_TEST(test_complete_ubx_offline_injection_uses_uart_path);
  RUN_TEST(test_qzss_l1ca_and_l1s_statistics_are_distinct);
  RUN_TEST(test_pps_micros_wrap_is_safe);
  RUN_TEST(test_pps_loss_and_abnormal_period);
  RUN_TEST(test_nav_pvt_pps_association_is_tentative);
  RUN_TEST(test_log_buffer_boundary_rejected_without_overwrite);
  RUN_TEST(test_oversized_raw_record_never_overflows);
  RUN_TEST(test_nmea_bytes_are_counted_separately);
}
