#include "test_support.h"
#include "baud_switch.h"
#include "gnss_self_test.h"
#include "gnss_rx_probe.h"
#include "gnss_uart_diag.h"
#include "startup_sequence.h"

namespace {
ConfigEvidence verifiedEvidence(uint32_t key, uint32_t value,
                                ConfigRequirement requirement) {
  ConfigEvidence evidence = {};
  evidence.key = key;
  evidence.targetValue = value;
  evidence.currentValue = value;
  evidence.requirement = requirement;
  evidence.result = ConfigResult::Verified;
  evidence.hasCurrentValue = true;
  return evidence;
}
void feedText(GnssRxProbe &probe, const char *text) {
  for (size_t i = 0U; text[i] != '\0'; ++i)
    probe.feed(static_cast<uint8_t>(text[i]));
}

void buildNmea(const char *body, char *output, size_t capacity) {
  uint8_t checksum = 0U;
  for (size_t i = 0U; body[i] != '\0'; ++i)
    checksum = static_cast<uint8_t>(checksum ^ static_cast<uint8_t>(body[i]));
  static const char hex[] = "0123456789ABCDEF";
  size_t p = 0U;
  if (capacity == 0U) return;
  output[p++] = '$';
  for (size_t i = 0U; body[i] != '\0' && p + 6U < capacity; ++i)
    output[p++] = body[i];
  output[p++] = '*';
  output[p++] = hex[checksum >> 4];
  output[p++] = hex[checksum & 0x0FU];
  output[p++] = '\r';
  output[p++] = '\n';
  output[p] = '\0';
}
}

void test_valid_nmea_gga_is_likely_nmea() {
  GnssRxProbe probe;
  feedText(probe, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
  const GnssRxProbeResult result = probe.finish();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GnssRxClassification::LikelyNmea),
                        static_cast<int>(result.classification));
  TEST_ASSERT_EQUAL_UINT32(1U, result.stats.nmea.validChecksum);
  TEST_ASSERT_EQUAL_STRING("GPGGA", result.stats.nmea.sentenceIds[0]);
}

void test_empty_fix_valid_nmea_is_still_likely_nmea() {
  char line[80];
  buildNmea("GNRMC,,V,,,,,,,,,,N", line, sizeof(line));
  GnssRxProbe probe;
  feedText(probe, line);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GnssRxClassification::LikelyNmea),
                        static_cast<int>(probe.finish().classification));
}

void test_bad_nmea_checksum_is_counted() {
  GnssRxProbe probe;
  feedText(probe, "$GNGGA,,,,,,0,,,,,,,,*00\r\n");
  const GnssRxProbeResult result = probe.finish();
  TEST_ASSERT_EQUAL_UINT32(0U, result.stats.nmea.validChecksum);
  TEST_ASSERT_EQUAL_UINT32(1U, result.stats.nmea.invalidChecksum);
}

void test_incomplete_nmea_is_counted_on_finish() {
  GnssRxProbe probe;
  feedText(probe, "$GNRMC,,V");
  TEST_ASSERT_EQUAL_UINT32(1U, probe.finish().stats.nmea.incompleteLines);
}

void test_valid_ubx_has_classification_priority() {
  GnssRxProbe probe;
  char line[80];
  buildNmea("GNRMC,,V,,,,,,,,,,N", line, sizeof(line));
  feedText(probe, line);
  probe.noteValidUbx();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GnssRxClassification::ValidUbx),
                        static_cast<int>(probe.finish().classification));
}

void test_high_bit_stream_is_wrong_baud_or_unknown_binary() {
  GnssRxProbe probe;
  for (uint8_t i = 0U; i < 64U; ++i)
    probe.feed(static_cast<uint8_t>(0x80U + ((i * 37U) & 0x7FU)));
  const GnssRxClassification c = probe.finish().classification;
  TEST_ASSERT_TRUE(c == GnssRxClassification::LikelyWrongBaud ||
                   c == GnssRxClassification::UnknownBinary);
}

void test_no_data_classification() {
  GnssRxProbe probe;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GnssRxClassification::NoData),
                        static_cast<int>(probe.finish().classification));
}

void test_mon_ver_poll_bytes_are_exact() {
  static const uint8_t expected[] =
      {0xB5U, 0x62U, 0x0AU, 0x04U, 0x00U, 0x00U, 0x0EU, 0x34U};
  uint8_t packet[8];
  TEST_ASSERT_EQUAL_UINT32(sizeof(packet), buildMonVerPoll(packet, sizeof(packet)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, packet, sizeof(expected));
}

void test_short_uart_write_is_recorded() {
  GnssTxStats stats = {};
  recordGnssTx(stats, 8U, 7U);
  TEST_ASSERT_EQUAL_UINT32(1U, stats.packets);
  TEST_ASSERT_EQUAL_UINT32(8U, stats.bytesRequested);
  TEST_ASSERT_EQUAL_UINT32(7U, stats.bytesWritten);
  TEST_ASSERT_EQUAL_UINT32(1U, stats.shortWrites);
}

void test_rx_only_is_not_bidirectional_pass() {
  const GnssLinkEvidence evidence = {true, true, false, false};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BidirectionalUbxStatus::Fail),
      static_cast<int>(evaluateBidirectionalUbx(evidence)));
}

void test_no_protocol_evidence_means_unknown_baud() {
  const BaudEvidence result = selectBaudEvidence(false, 115200U, false, 9600U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudEvidenceKind::Unknown),
                        static_cast<int>(result.kind));
  TEST_ASSERT_EQUAL_UINT32(0U, result.baud);
}

void test_nmea_baud_is_explicitly_nmea_only() {
  const BaudEvidence result = selectBaudEvidence(false, 0U, true, 9600U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudEvidenceKind::NmeaOnly),
                        static_cast<int>(result.kind));
  TEST_ASSERT_EQUAL_UINT32(9600U, result.baud);
}

void test_probe_reset_discards_candidate_local_state() {
  GnssRxProbe probe;
  feedText(probe, "$GNRMC,");
  probe.reset();
  const GnssRxProbeResult result = probe.finish();
  TEST_ASSERT_EQUAL_UINT32(0U, result.stats.receivedBytes);
  TEST_ASSERT_EQUAL_UINT32(0U, result.stats.nmea.incompleteLines);
}

void test_ascii_probe_output_escapes_controls() {
  const uint8_t input[] = {'A', '\r', '\n', '\t', 0x01U, 'Z'};
  char output[32];
  formatGnssRxAscii(input, sizeof(input), output, sizeof(output));
  TEST_ASSERT_EQUAL_STRING("A\\r\\n\\t.Z", output);
  for (size_t i = 0U; output[i] != '\0'; ++i)
    TEST_ASSERT_TRUE(static_cast<uint8_t>(output[i]) >= 0x20U);
}

void test_hex_probe_output_is_bounded_and_uppercase() {
  const uint8_t input[] = {0x00U, 0xA5U, 0xFFU};
  char output[6];
  const size_t length = formatGnssRxHex(input, sizeof(input), output,
                                        sizeof(output));
  TEST_ASSERT_EQUAL_UINT32(4U, length);
  TEST_ASSERT_EQUAL_STRING("00A5", output);
}

void test_loopback_sequence_contains_binary_prefix_and_mon_ver() {
  uint8_t output[12];
  TEST_ASSERT_EQUAL_UINT32(12U, buildGnssLoopbackSequence(output, sizeof(output)));
  TEST_ASSERT_EQUAL_HEX8(0x00U, output[0]);
  TEST_ASSERT_EQUAL_HEX8(0x55U, output[1]);
  TEST_ASSERT_EQUAL_HEX8(0xAAU, output[2]);
  TEST_ASSERT_EQUAL_HEX8(0xFFU, output[3]);
  TEST_ASSERT_EQUAL_HEX8(0xB5U, output[4]);
  TEST_ASSERT_EQUAL_HEX8(0x34U, output[11]);
}

void test_first_serial_begin_never_calls_end() {
  TEST_ASSERT_FALSE(shouldEndSerialBeforeBegin(false));
  TEST_ASSERT_TRUE(shouldEndSerialBeforeBegin(true));
}

void test_unconnected_gnss_exhausts_finite_baud_candidates() {
  uint8_t candidate = 0U;
  TEST_ASSERT_TRUE(advanceBaudCandidate(candidate, 3U));
  TEST_ASSERT_TRUE(advanceBaudCandidate(candidate, 3U));
  TEST_ASSERT_FALSE(advanceBaudCandidate(candidate, 3U));
  TEST_ASSERT_EQUAL_UINT8(3U, candidate);
}

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

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigAction::Apply),
      static_cast<int>(decideConfigAction(true, 0U, 1U, true)));

  }

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigAction::FallbackApply),
      static_cast<int>(decideConfigAction(false, 0U, 1U, true)));

  }
}

void test_optional_config_nak_is_not_fatal() {
  TEST_ASSERT_FALSE(isConfigFailureFatal(ConfigRequirement::Optional));
  TEST_ASSERT_TRUE(isConfigFailureFatal(ConfigRequirement::Required));
}

void test_self_test_pass_requires_verified_evidence() {
  ConfigEvidence qzss = verifiedEvidence(CFG_SIGNAL_QZSS_ENA, 1U,
                                          ConfigRequirement::Required);
  ConfigEvidence l1s = verifiedEvidence(CFG_SIGNAL_QZSS_L1S_ENA, 1U,
                                         ConfigRequirement::Required);
  ConfigEvidence ubx = verifiedEvidence(CFG_UART1OUTPROT_UBX, 1U,
                                         ConfigRequirement::Required);
  ConfigEvidence sfrbx = verifiedEvidence(CFG_MSGOUT_RXM_SFRBX_UART1, 1U,
                                           ConfigRequirement::Required);
  ConfigEvidence nav = verifiedEvidence(CFG_MSGOUT_NAV_PVT_UART1, 1U,
                                         ConfigRequirement::Optional);
  ConfigEvidence tp = verifiedEvidence(CFG_TP_TP1_ENA, 1U,
                                        ConfigRequirement::Optional);
  ConfigEvidence grid = verifiedEvidence(CFG_TP_TIMEGRID_TP1, 0U,
                                          ConfigRequirement::Optional);
  GnssSelfTestEvidence evidence = {};
  evidence.uartBytesObserved = evidence.validUbxObserved = true;
  evidence.monVerObserved = evidence.sfrbxObserved = true;
  evidence.navPvtObserved = evidence.ppsObserved = true;
  evidence.timePulseAllVerified = true;
  evidence.qzss = &qzss; evidence.qzssL1s = &l1s; evidence.ubxOutput = &ubx;
  evidence.sfrbxOutput = &sfrbx; evidence.navPvtOutput = &nav;
  evidence.timePulseEnable = &tp; evidence.timePulseGrid = &grid;
  const GnssSelfTestReport report = evaluateGnssSelfTest(evidence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SelfTestStatus::Pass),
                        static_cast<int>(report.overall));
}

void test_valset_ack_without_matching_valget_is_not_verified() {
  ConfigEvidence evidence = {};
  evidence.targetValue = 1U;
  evidence.currentValue = 0U;
  evidence.hasCurrentValue = true;
  evidence.valsetAcknowledged = true;
  evidence.result = ConfigResult::Changed;
  TEST_ASSERT_FALSE(configEvidenceVerified(evidence));
  evidence.result = ConfigResult::Verified;
  TEST_ASSERT_FALSE(configEvidenceVerified(evidence));
}

void test_baud_is_not_a_normal_config_key() {
  TEST_ASSERT_FALSE(normalConfigContainsBaudKey());
}

void test_baud_waits_for_all_normal_config_keys() {
  TEST_ASSERT_FALSE(baudSwitchMayStart(9U, 10U));
  TEST_ASSERT_TRUE(baudSwitchMayStart(10U, 10U));
}

void test_disabled_baud_switch_does_not_request_change() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::Disabled),
      static_cast<int>(initialBaudSwitchResult(false, 38400U, 115200U)));

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::NotNeeded),
      static_cast<int>(initialBaudSwitchResult(true, 38400U, 38400U)));

  }
}

void test_baud_valset_packet_is_little_endian_and_checksum_valid() {
  uint8_t packet[24] = {};
  const size_t length = buildBaudValsetPacket(115200U, 1U, packet,
                                               sizeof(packet));
  const uint8_t expectedPrefix[] = {0xB5, 0x62, 0x06, 0x8A, 0x0C, 0x00,
                                    0x00, 0x01, 0x00, 0x00,
                                    0x01, 0x00, 0x52, 0x40,
                                    0x00, 0xC2, 0x01, 0x00};
  TEST_ASSERT_EQUAL_UINT32(20U, length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedPrefix, packet, sizeof(expectedPrefix));
  uint8_t a = 0U, b = 0U;
  UbxParser::checksum(packet + 2U, length - 4U, a, b);
  TEST_ASSERT_EQUAL_HEX8(a, packet[length - 2U]);
  TEST_ASSERT_EQUAL_HEX8(b, packet[length - 1U]);
}

void test_valset_ack_alone_never_verifies_baud() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::Pending),
      static_cast<int>(evaluateBaudSwitchEvidence(
          true, 38400U, 115200U, true, false, false)));

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::Verified),
      static_cast<int>(evaluateBaudSwitchEvidence(
          true, 38400U, 115200U, false, true, false)));

  }

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::KeptOriginal),
      static_cast<int>(evaluateBaudSwitchEvidence(
          true, 38400U, 115200U, true, false, true)));

  }
}

void test_recovery_sequence_is_finite_and_ordered() {
  BaudRecoverySequence sequence;
  sequence.begin(38400U, 115200U);
  const uint32_t expected[] = {115200U, 38400U, 9600U, 38400U, 115200U};
  for (size_t i = 0U; i < 5U; ++i) {
    uint32_t value = 0U;
    TEST_ASSERT_TRUE(sequence.next(value));
    TEST_ASSERT_EQUAL_UINT32(expected[i], value);
  }
  uint32_t value = 0U;
  TEST_ASSERT_FALSE(sequence.next(value));
  TEST_ASSERT_TRUE(sequence.exhausted());
}

void run_gnss_tests() {
  RUN_TEST(test_valid_nmea_gga_is_likely_nmea);
  RUN_TEST(test_empty_fix_valid_nmea_is_still_likely_nmea);
  RUN_TEST(test_bad_nmea_checksum_is_counted);
  RUN_TEST(test_incomplete_nmea_is_counted_on_finish);
  RUN_TEST(test_valid_ubx_has_classification_priority);
  RUN_TEST(test_high_bit_stream_is_wrong_baud_or_unknown_binary);
  RUN_TEST(test_no_data_classification);
  RUN_TEST(test_mon_ver_poll_bytes_are_exact);
  RUN_TEST(test_short_uart_write_is_recorded);
  RUN_TEST(test_rx_only_is_not_bidirectional_pass);
  RUN_TEST(test_no_protocol_evidence_means_unknown_baud);
  RUN_TEST(test_nmea_baud_is_explicitly_nmea_only);
  RUN_TEST(test_probe_reset_discards_candidate_local_state);
  RUN_TEST(test_ascii_probe_output_escapes_controls);
  RUN_TEST(test_hex_probe_output_is_bounded_and_uppercase);
  RUN_TEST(test_loopback_sequence_contains_binary_prefix_and_mon_ver);
  RUN_TEST(test_first_serial_begin_never_calls_end);
  RUN_TEST(test_unconnected_gnss_exhausts_finite_baud_candidates);
  RUN_TEST(test_cfg_valget_request_encoding);
  RUN_TEST(test_cfg_valget_response_parsing);
  RUN_TEST(test_config_match_skips_valset);
  RUN_TEST(test_optional_config_nak_is_not_fatal);
  RUN_TEST(test_self_test_pass_requires_verified_evidence);
  RUN_TEST(test_valset_ack_without_matching_valget_is_not_verified);
  RUN_TEST(test_baud_is_not_a_normal_config_key);
  RUN_TEST(test_baud_waits_for_all_normal_config_keys);
  RUN_TEST(test_disabled_baud_switch_does_not_request_change);
  RUN_TEST(test_baud_valset_packet_is_little_endian_and_checksum_valid);
  RUN_TEST(test_valset_ack_alone_never_verifies_baud);
  RUN_TEST(test_recovery_sequence_is_finite_and_ordered);
}
