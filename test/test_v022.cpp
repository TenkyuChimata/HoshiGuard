#include <unity.h>

#include <string.h>

#include "gnss_rx_probe.h"
#include "gnss_self_test.h"
#include "gnss_uart_diag.h"
#include "ubx_parser.h"

namespace {
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
}  // namespace

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

void test_single_pps_is_observed_once() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::ObservedOnce),
      static_cast<int>(evaluatePpsActivity(1U, 1000000U, 1100000U, 0U, 0U)));
}

void test_three_recent_normal_pps_are_stable() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::Stable),
      static_cast<int>(evaluatePpsActivity(3U, 3000000U, 3100000U, 0U, 0U)));
}

void test_one_pps_then_timeout_is_transient() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::TransientOrMissing),
      static_cast<int>(evaluatePpsActivity(1U, 100000U, 2100000U, 0U, 0U)));
}

void test_probe_reset_discards_candidate_local_state() {
  GnssRxProbe probe;
  feedText(probe, "$GNRMC,");
  probe.reset();
  const GnssRxProbeResult result = probe.finish();
  TEST_ASSERT_EQUAL_UINT32(0U, result.stats.receivedBytes);
  TEST_ASSERT_EQUAL_UINT32(0U, result.stats.nmea.incompleteLines);
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

void run_v022_tests() {
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
  RUN_TEST(test_single_pps_is_observed_once);
  RUN_TEST(test_three_recent_normal_pps_are_stable);
  RUN_TEST(test_one_pps_then_timeout_is_transient);
  RUN_TEST(test_probe_reset_discards_candidate_local_state);
  RUN_TEST(test_ubx_stream_reset_discards_partial_but_preserves_totals);
  RUN_TEST(test_ascii_probe_output_escapes_controls);
  RUN_TEST(test_hex_probe_output_is_bounded_and_uppercase);
  RUN_TEST(test_loopback_sequence_contains_binary_prefix_and_mon_ver);
}
