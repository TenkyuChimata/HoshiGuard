#include "test_support.h"
#include "config.h"
#include "oled_i2c.h"
#include "ssd1306.h"
#include "status_display.h"
#include "bridge_protocol.h"
#include "ubx_dispatcher.h"
#include <vector>

namespace {
struct Registers {
  uint32_t cr1 = 1U, dr = 0U, sr1 = 0U, sr2 = 0U;
  OledI2cRegisters view() { return {cr1, dr, sr1, sr2}; }
};

// 合成接続。転送中の領域の不変性と page/column 境界も監視する。
struct MockBus {
  bool present = true, stuck = false, reject = false, nack = false, beginOk = true;
  unsigned begins = 0U, ends = 0U;
  const uint8_t *pending = 0;
  size_t pendingLength = 0U;
  std::vector<std::vector<uint8_t> > packets;
  uint8_t ram[8][128] = {};
  uint8_t page = 0U, column = 0U;
  size_t dataBytes = 0U;
  bool on = false;
  oled::Bus bus() { return {this, begin, start, poll, end}; }
  static bool begin(void *p) {
    MockBus &m = *static_cast<MockBus *>(p); ++m.begins; m.on = false; return m.beginOk;
  }
  static bool start(void *p, const uint8_t *data, size_t length, uint32_t) {
    MockBus &m = *static_cast<MockBus *>(p);
    if (m.reject) return false;
    TEST_ASSERT_NULL(m.pending);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(17U, length);
    m.pending = data; m.pendingLength = length;
    m.packets.push_back(std::vector<uint8_t>(data, data + length)); return true;
  }
  static OledTransfer poll(void *p, uint32_t) {
    MockBus &m = *static_cast<MockBus *>(p);
    TEST_ASSERT_NOT_NULL(m.pending);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(m.packets.back().data(), m.pending, m.pendingLength);
    if (m.stuck) return OledTransfer::Pending;
    m.pending = 0;
    if (!m.present || m.nack) return OledTransfer::Failed;
    const std::vector<uint8_t> &packet = m.packets.back();
    if (packet[0] == 0x40U) {
      TEST_ASSERT_LESS_THAN_UINT8(8U, m.page);
      for (size_t i = 1U; i < packet.size(); ++i) {
        TEST_ASSERT_LESS_THAN_UINT8(128U, m.column);
        m.ram[m.page][m.column++] = packet[i]; ++m.dataBytes;
      }
    } else {
      TEST_ASSERT_EQUAL_HEX8(0U, packet[0]);
      if (packet.size() == 4U && packet[1] >= 0xB0U && packet[1] <= 0xB7U) {
        m.page = static_cast<uint8_t>(packet[1] - 0xB0U);
        m.column = static_cast<uint8_t>((packet[2] & 0x0FU) | ((packet[3] & 0x0FU) << 4U));
      }
      if (packet.size() == 2U && packet[1] == 0xAFU) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1024U, m.dataBytes); m.on = true;
      }
    }
    return OledTransfer::Complete;
  }
  static void end(void *p) {
    MockBus &m = *static_cast<MockBus *>(p); ++m.ends; m.pending = 0;
  }
};
void run(oled::Ssd1306 &display, uint32_t startUs, size_t ticks = 500U) {
  for (size_t i = 0U; i < ticks; ++i) {
    const uint32_t us = startUs + static_cast<uint32_t>(i) * 10U;
    display.tick(us / 1000U, us);
  }
}
oled::StatusInput ready() {
  oled::StatusInput input = {}; input.gnss = oled::GnssState::Ready;
  input.bridgeBinary = true; input.ppsStable = true; return input;
}

void test_i2c_success_steps_and_last_byte_ack_before_stop() {
  Registers r; OledI2cWriter writer(r.view()); const uint8_t bytes[] = {0x40U, 0xAAU, 0x55U};
  TEST_ASSERT_TRUE(writer.start(bytes, sizeof(bytes), 100U));
  TEST_ASSERT_FALSE(writer.start(bytes, sizeof(bytes), 100U));
  writer.poll(101U); TEST_ASSERT_BITS_HIGH(1U << 8, r.cr1);
  r.sr1 = 1U; writer.poll(102U); TEST_ASSERT_EQUAL_HEX8(0x78U, r.dr);
  r.sr1 = 2U; r.sr2 = 3U; writer.poll(103U);
  for (size_t i = 0U; i < sizeof(bytes); ++i) {
    r.sr1 = 0U; writer.poll(104U); r.sr1 = 128U; writer.poll(105U);
    TEST_ASSERT_EQUAL_HEX8(bytes[i], r.dr);
  }
  r.sr1 = 128U; writer.poll(106U); TEST_ASSERT_BITS_LOW(1U << 9, r.cr1);
  r.sr1 = 4U; writer.poll(107U); TEST_ASSERT_BITS_HIGH(1U << 9, r.cr1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OledTransfer::Pending), static_cast<int>(writer.poll(108U)));
  r.cr1 &= ~(1U << 9); r.sr2 = 0U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OledTransfer::Complete), static_cast<int>(writer.poll(109U)));
}
void test_i2c_errors_release_peripheral() {
  const uint32_t errors[] = {1U << 8, 1U << 9, 1U << 10, 1U << 11, 1U << 14};
  const uint8_t data[] = {0U};
  for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    Registers r; OledI2cWriter writer(r.view()); writer.start(data, sizeof(data), 0U);
    r.sr1 = errors[i];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OledTransfer::Failed), static_cast<int>(writer.poll(1U)));
    TEST_ASSERT_EQUAL_UINT32(0U, r.cr1); TEST_ASSERT_TRUE(writer.start(data, sizeof(data), 2U));
  }
}
void test_i2c_every_phase_has_one_deadline_across_rollover() {
  const uint8_t data[] = {0U};
  for (unsigned phase = 0U; phase < 6U; ++phase) {
    Registers r; OledI2cWriter writer(r.view()); const uint32_t start = UINT32_MAX - 1000U;
    writer.start(data, sizeof(data), start);
    if (phase == 0U) r.sr2 = 2U; else writer.poll(start);
    if (phase > 1U) { r.sr1 = 1U; writer.poll(start + 1U); }
    if (phase > 2U) { r.sr1 = 2U; writer.poll(start + 2U); }
    if (phase > 3U) { r.sr1 = 128U; writer.poll(start + 3U); }
    if (phase > 4U) { r.sr1 = 4U; writer.poll(start + 4U); }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OledTransfer::Pending), static_cast<int>(writer.poll(start + 2999U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OledTransfer::Failed), static_cast<int>(writer.poll(start + 3000U)));
    TEST_ASSERT_EQUAL_UINT32(0U, r.cr1);
  }
}
void test_i2c_rejects_invalid_transfer_lengths() {
  Registers r; OledI2cWriter writer(r.view()); uint8_t data[18] = {};
  TEST_ASSERT_FALSE(writer.start(0, 1U, 0U)); TEST_ASSERT_FALSE(writer.start(data, 0U, 0U));
  TEST_ASSERT_FALSE(writer.start(data, sizeof(data), 0U));
}
void test_ssd1306_initialization_and_ram_contents() {
  MockBus bus; oled::Ssd1306 display(bus.bus()); oled::Screen screen = {};
  oled::setRow(screen, 0U, "HoshiGuard 0.2.7"); oled::setRow(screen, 7U, "! RX LOST");
  screen.inverted = 0x81U; display.begin(0U); display.setScreen(screen);
  display.tick(99U, 99000U); TEST_ASSERT_EQUAL_UINT32(0U, bus.begins); run(display, 100000U);
  TEST_ASSERT_TRUE(display.online()); TEST_ASSERT_TRUE(bus.on);
  TEST_ASSERT_EQUAL_UINT32(1024U, bus.dataBytes);
  const uint8_t init0[] = {0x00,0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x20,0x02,0xA1,0xC8};
  const uint8_t init1[] = {0x00,0xDA,0x12,0x81,0x7F,0xD9,0xF1,0xDB,0x20,0x8D,0x14,0xA4,0xA6,0x2E};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(init0, bus.packets[0].data(), sizeof(init0));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(init1, bus.packets[1].data(), sizeof(init1));
  for (size_t row = 0U; row < 8U; ++row)
    for (uint8_t x = 0U; x < 128U; ++x)
      TEST_ASSERT_EQUAL_HEX8(oled::rasterColumn(screen.rows[row], x, (screen.inverted & (1U << row)) != 0U), bus.ram[row][x]);
}
void test_ssd1306_unchanged_skipped_and_one_row_updated() {
  MockBus bus; oled::Ssd1306 display(bus.bus()); oled::Screen screen = {};
  display.begin(0U); display.setScreen(screen); run(display, 100000U);
  const size_t count = bus.packets.size(); display.setScreen(screen); run(display, 200000U);
  TEST_ASSERT_EQUAL_UINT32(count, bus.packets.size());
  oled::setRow(screen, 4U, "AGE:1s MT44 DCX"); display.setScreen(screen); run(display, 300000U);
  TEST_ASSERT_EQUAL_UINT32(count + 9U, bus.packets.size());
  TEST_ASSERT_EQUAL_HEX8(0xB4U, bus.packets[count][1]); TEST_ASSERT_EQUAL_UINT32(1152U, bus.dataBytes);
  screen.inverted = 1U << 4; display.setScreen(screen); run(display, 400000U);
  TEST_ASSERT_EQUAL_UINT32(count + 18U, bus.packets.size());
}
void test_ssd1306_changes_during_upload_use_stable_row_snapshot() {
  MockBus bus; oled::Ssd1306 display(bus.bus()); oled::Screen screen = {};
  display.begin(0U); display.setScreen(screen); run(display, 100000U);
  oled::setRow(screen, 0U, "OLD VALUE"); display.setScreen(screen); run(display, 200000U, 6U);
  oled::setRow(screen, 0U, "NEW"); display.setScreen(screen); run(display, 200060U);
  for (uint8_t x = 0U; x < 128U; ++x)
    TEST_ASSERT_EQUAL_HEX8(oled::rasterColumn(screen.rows[0], x, false), bus.ram[0][x]);
}
void test_ssd1306_missing_screen_backoff_and_recovery() {
  MockBus bus; bus.present = false; oled::Ssd1306 display(bus.bus()); display.begin(0U);
  run(display, 100000U); TEST_ASSERT_FALSE(display.online());
  TEST_ASSERT_EQUAL_UINT32(1U, display.failures()); TEST_ASSERT_EQUAL_UINT32(1U, bus.ends);
  run(display, 30000000U); TEST_ASSERT_EQUAL_UINT32(1U, bus.begins);
  bus.present = true; run(display, 30101000U);
  TEST_ASSERT_TRUE(display.online()); TEST_ASSERT_EQUAL_UINT32(2U, bus.begins);
}
void test_ssd1306_pending_timeout_and_start_rejection() {
  for (unsigned mode = 0U; mode < 2U; ++mode) {
    MockBus bus; bus.stuck = mode == 0U; bus.reject = mode == 1U;
    oled::Ssd1306 display(bus.bus()); display.begin(0U); run(display, 100000U);
    TEST_ASSERT_EQUAL_UINT32(1U, display.failures()); TEST_ASSERT_EQUAL_UINT32(1U, bus.ends);
    TEST_ASSERT_FALSE(display.online());
  }
}
void test_ssd1306_bus_setup_failure_also_backs_off() {
  MockBus bus; bus.beginOk = false; oled::Ssd1306 display(bus.bus()); display.begin(0U);
  run(display, 100000U); run(display, 200000U);
  TEST_ASSERT_EQUAL_UINT32(1U, bus.begins); TEST_ASSERT_EQUAL_UINT32(1U, bus.ends);
  TEST_ASSERT_EQUAL_UINT32(1U, display.failures()); TEST_ASSERT_TRUE(bus.packets.empty());
}
void test_ssd1306_error_after_online_reinitializes_all_rows() {
  MockBus bus; oled::Ssd1306 display(bus.bus()); oled::Screen screen = {};
  display.begin(0U); run(display, 100000U);
  bus.nack = true; oled::setRow(screen, 3U, "CHANGED"); display.setScreen(screen);
  run(display, 200000U); TEST_ASSERT_FALSE(display.online());
  bus.nack = false; run(display, 30201000U);
  TEST_ASSERT_TRUE(display.online()); TEST_ASSERT_EQUAL_UINT32(2048U, bus.dataBytes);
}
void test_ssd1306_uart_backlog_defers_start_but_not_timeout() {
  MockBus bus; oled::Ssd1306 display(bus.bus()); display.begin(0U);
  display.tick(100U, 100000U, false); TEST_ASSERT_EQUAL_UINT32(0U, bus.begins);
  display.tick(100U, 100000U); display.tick(100U, 100010U);
  TEST_ASSERT_EQUAL_UINT32(1U, bus.packets.size());
  bus.stuck = true; display.tick(103U, 103010U, false); TEST_ASSERT_EQUAL_UINT32(1U, display.failures());
}
void test_ssd1306_wait_and_retry_across_millis_rollover() {
  MockBus bus; bus.present = false; oled::Ssd1306 display(bus.bus());
  const uint32_t start = UINT32_MAX - 50U; display.begin(start);
  display.tick(start + 99U, 0U); TEST_ASSERT_EQUAL_UINT32(0U, bus.begins);
  display.tick(start + 100U, 100U); display.tick(start + 100U, 110U); display.tick(start + 100U, 120U);
  TEST_ASSERT_EQUAL_UINT32(1U, display.failures());
  display.tick(start + 30099U, 130U); TEST_ASSERT_EQUAL_UINT32(1U, bus.begins);
  display.tick(start + 30100U, 140U); TEST_ASSERT_EQUAL_UINT32(2U, bus.begins);
}
void test_text_clipping_unknown_glyphs_and_edges() {
  struct Guarded { uint32_t before; oled::Screen screen; uint32_t after; } v = {};
  v.before = 0xAA5555AAU; v.after = 0x55AAAA55U;
  oled::setRow(v.screen, 7U, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOPQRSTU", v.screen.rows[7]);
  oled::setRow(v.screen, 8U, "INVALID"); oled::setRow(v.screen, 0U, "\xFF");
  TEST_ASSERT_EQUAL_STRING("?", v.screen.rows[0]);
  TEST_ASSERT_EQUAL_UINT32(0xAA5555AAU, v.before); TEST_ASSERT_EQUAL_UINT32(0x55AAAA55U, v.after);
  TEST_ASSERT_EQUAL_HEX8(0U, oled::rasterColumn("A", 125U, false));
  TEST_ASSERT_EQUAL_HEX8(0U, oled::rasterColumn("A", 126U, false));
  TEST_ASSERT_EQUAL_HEX8(0xFFU, oled::rasterColumn("A", 127U, true));
  TEST_ASSERT_EQUAL_HEX8(0U, oled::rasterColumn(0, 0U, false));
  // A の左列は 01111110。自己比較だけで字形を検証しない。
  TEST_ASSERT_EQUAL_HEX8(0x7EU, oled::rasterColumn("A", 0U, false));
  for (uint8_t x = 0U; x < 6U; ++x) {
    TEST_ASSERT_EQUAL_HEX8(oled::rasterColumn("A", x, false), oled::rasterColumn("a", x, false));
    TEST_ASSERT_EQUAL_HEX8(oled::rasterColumn("?", x, false), oled::rasterColumn("@", x, false));
  }
}
void test_status_startup_wait_and_real_failure_are_distinct() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  input.gnss = oled::GnssState::Initializing; status.render(0U, input, screen, HOSHIGUARD_VERSION);
  TEST_ASSERT_EQUAL_STRING("HoshiGuard " HOSHIGUARD_VERSION, screen.rows[0]);
  TEST_ASSERT_EQUAL_STRING("CONFIGURING M10S", screen.rows[7]);
  TEST_ASSERT_EQUAL_STRING("AGE:-- MT:--", screen.rows[4]); TEST_ASSERT_EQUAL_HEX8(1U, screen.inverted);
  input = ready(); status.noteUbx(100U); status.render(100U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("L1S:WAIT N:0", screen.rows[3]);
  TEST_ASSERT_EQUAL_STRING("WAIT L1S", screen.rows[7]); TEST_ASSERT_EQUAL_HEX8(1U, screen.inverted);
  input.gnss = oled::GnssState::Failed; status.render(200U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("! M10S INIT FAILED", screen.rows[7]); TEST_ASSERT_EQUAL_HEX8(0x81U, screen.inverted);
}
void test_status_l1s_counts_valid_frames_and_retains_last_mt() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  status.noteUbx(0U); status.noteL1s(0U, 44U, false); status.render(0U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("L1S:WAIT N:0", screen.rows[3]);
  status.noteL1s(100U, 43U, true); status.noteL1s(200U, 44U, true); status.render(1200U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("L1S:LIVE N:2", screen.rows[3]);
  TEST_ASSERT_EQUAL_STRING("AGE:1s MT44 DCX", screen.rows[4]); status.noteL1s(1300U, 63U, false);
  status.noteUbx(30200U); status.render(30200U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("L1S:WAIT N:2", screen.rows[3]);
  TEST_ASSERT_EQUAL_STRING("WAIT L1S", screen.rows[7]); TEST_ASSERT_EQUAL_HEX8(1U, screen.inverted);
  status.noteL1s(30300U, 0U, true); status.render(30300U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("AGE:0s MT0", screen.rows[4]);
}
void test_status_time_quality_requires_current_evidence() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  input.timeQuality = TimeQuality::UtcPpsConfirmed; status.render(0U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:WAIT TIME:STALE", screen.rows[2]);
  status.noteUbx(0U); status.notePps(0U); status.noteNav(0U); status.noteTimTp(0U);
  status.render(0U, input, screen, "v"); TEST_ASSERT_EQUAL_STRING("PPS:OK TIME:CONF", screen.rows[2]);
  status.noteUbx(2500U); status.render(2500U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:LOST TIME:STALE", screen.rows[2]);
  status.notePps(2500U); status.render(3000U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:OK TIME:STALE", screen.rows[2]);
  status.noteNav(3000U); status.render(3000U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:OK TIME:STALE", screen.rows[2]);
  status.noteTimTp(3000U); input.ppsAbnormal = true; status.render(3000U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:BAD TIME:STALE", screen.rows[2]);
}
void test_status_quality_labels_do_not_invent_lock() {
  const TimeQuality quality[] = {TimeQuality::Invalid, TimeQuality::GnssUtcValid, TimeQuality::PpsObserved,
      TimeQuality::UtcPpsTentative, TimeQuality::LockedUtcLimited, TimeQuality::UtcPpsConfirmed, TimeQuality::Degraded};
  const char *labels[] = {"NONE", "UTC", "PPS", "TENT", "LIMIT", "CONF", "WARN"};
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  status.noteUbx(0U); status.notePps(0U); status.noteNav(0U); status.noteTimTp(0U);
  for (size_t i = 0U; i < sizeof(quality) / sizeof(quality[0]); ++i) {
    input.timeQuality = quality[i]; status.render(0U, input, screen, "v");
    TEST_ASSERT_NOT_NULL(strstr(screen.rows[2], labels[i]));
  }
}
void test_status_single_pps_is_seen_without_confirming_lock() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  status.noteUbx(0U); status.notePps(0U); status.noteNav(0U); status.noteTimTp(0U);
  input.ppsStable = false; input.timeQuality = TimeQuality::PpsObserved;
  status.render(0U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:SEEN TIME:PPS", screen.rows[2]);
  input.timeQuality = TimeQuality::UtcPpsConfirmed; status.render(0U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("PPS:SEEN TIME:STALE", screen.rows[2]);
}
void test_status_bridge_drop_history_and_counter_bounds() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready(); status.noteUbx(0U);
  input.bridgeSent = UINT32_MAX; input.bridgeDropped = UINT32_MAX;
  input.ubxErrors = UINT32_MAX; input.l1sErrors = UINT32_MAX;
  status.render(0U, input, screen, "1234567890123456789012345");
  TEST_ASSERT_EQUAL_STRING("HoshiGuard 1234567890", screen.rows[0]);
  TEST_ASSERT_EQUAL_STRING("BR TX:99999+ D:9999+", screen.rows[5]);
  TEST_ASSERT_EQUAL_STRING("BAD U:9999+ L:9999+", screen.rows[6]);
  TEST_ASSERT_EQUAL_STRING("! BR DROP HISTORY", screen.rows[7]);
  input.bridgeL1sDropped = 1U; status.render(0U, input, screen, 0);
  TEST_ASSERT_EQUAL_STRING("! BR L1S DROP", screen.rows[7]);
  for (size_t row = 0U; row < 8U; ++row) TEST_ASSERT_LESS_OR_EQUAL_UINT32(21U, strlen(screen.rows[row]));
}
void test_status_config_error_pps_loss_and_text_mode() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready(); status.noteUbx(0U);
  input.gnss = oled::GnssState::Degraded; input.initError = 5U; status.render(0U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("M10S:DEG UBX:OK", screen.rows[1]); TEST_ASSERT_EQUAL_STRING("! CONFIG E5", screen.rows[7]);
  input.initError = 0U; input.bridgeBinary = false; status.notePps(0U); status.render(2500U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("! PPS CHECK", screen.rows[7]); TEST_ASSERT_EQUAL_STRING("BR:OFF TEXT OUTPUT", screen.rows[5]);
  status.render(3000U, input, screen, "v"); TEST_ASSERT_EQUAL_STRING("! UBX RX LOST", screen.rows[7]);
}
void test_status_refresh_and_age_rollover() {
  oled::Age age; age.touch(UINT32_MAX - 100U); age.advance(99U);
  TEST_ASSERT_EQUAL_UINT32(200U, age.elapsedMs()); age.advance(UINT32_MAX - 1U); age.advance(1000U);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, age.elapsedMs()); age.advance(2000U);
  TEST_ASSERT_FALSE(age.fresh(30000U)); age.touch(2000U); TEST_ASSERT_TRUE(age.fresh(1U));
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  const uint32_t start = UINT32_MAX - 100U; TEST_ASSERT_TRUE(status.due(start));
  status.noteUbx(start); status.noteL1s(start, 43U, true); status.render(start, input, screen, "v");
  TEST_ASSERT_FALSE(status.due(start + 499U)); TEST_ASSERT_TRUE(status.due(start + 500U));
  status.render(start + 1000U, input, screen, "v"); TEST_ASSERT_EQUAL_STRING("AGE:1s MT43 DCR", screen.rows[4]);
  status.render(start + 30000U, input, screen, "v"); TEST_ASSERT_EQUAL_STRING("L1S:WAIT N:1", screen.rows[3]);
}
void test_status_long_silence_does_not_revive_l1s() {
  oled::StatusDisplay status; oled::Screen screen; oled::StatusInput input = ready();
  status.noteL1s(0U, 44U, true); status.render(0U, input, screen, "v");
  status.render(86400000U, input, screen, "v"); TEST_ASSERT_EQUAL_STRING("AGE:1d MT44 DCX", screen.rows[4]);
  status.render(UINT32_MAX - 10U, input, screen, "v"); status.render(100U, input, screen, "v");
  TEST_ASSERT_EQUAL_STRING("AGE:49d+ MT44 DCX", screen.rows[4]);
  TEST_ASSERT_EQUAL_STRING("L1S:WAIT N:1", screen.rows[3]);
}
void test_real_ubx_and_bridge_bytes_survive_oled_fault_simulation() {
  const RealFixture &real = realFixture("qzss_l1s_mt44_pab53");
  for (unsigned mode = 0U; mode < 3U; ++mode) {
    MockBus bus; bus.present = mode != 1U; bus.stuck = mode == 2U;
    oled::Ssd1306 display(bus.bus()); display.begin(0U); UbxParser parser; UbxFrameDispatcher dispatcher;
    bridge::RecordQueue<7U, bridge::kMaximumEncodedFrame, 4U> queue; PpsIntervalTracker pps;
    size_t received = 0U; std::vector<uint8_t> expected, actual;
    // 実受信 packet を使う合成スケジュール。実機の処理時間の測定ではない。
    for (uint32_t tick = 0U; tick < 4000U; ++tick) {
      const uint32_t us = tick * 1000U;
      if (tick % 1000U == 0U) pps.observe(tick / 1000U + 1U, us);
      if (tick % 10U == 0U) {
        UbxInjectionResult result = {};
        injectUbxBytes(real.data, real.length, parser, dispatcher, result, us, tick / 1000U + 1U);
        TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets); TEST_ASSERT_TRUE(result.lastFrame.validCrc); ++received;
        uint8_t payload[20U + bridge::kMaximumUbxLength] = {};
        bridge::putU32Le(payload, tick / 1000U + 1U);
        payload[4] = result.lastFrame.svId; payload[5] = 5U; payload[6] = 1U;
        payload[7] = real.data[10]; payload[8] = result.lastFrame.pab;
        payload[9] = result.lastFrame.messageType;
        bridge::putU16Le(payload + 10U, bridge::L1sFlagUbxChecksumValid |
            bridge::L1sFlagLengthValid | bridge::L1sFlagPabValid | bridge::L1sFlagCrcValid);
        bridge::putU16Le(payload + 12U, static_cast<uint16_t>(real.length));
        payload[16] = HOSHIGUARD_VERSION_MAJOR; payload[17] = HOSHIGUARD_VERSION_MINOR;
        payload[18] = HOSHIGUARD_VERSION_PATCH; memcpy(payload + 20U, real.data, real.length);
        uint8_t wire[bridge::kMaximumEncodedFrame]; size_t length = 0U;
        const bridge::Header header = {bridge::FrameType::L1sFrame, static_cast<uint16_t>(20U + real.length), 0U, tick / 10U, us};
        TEST_ASSERT_TRUE(bridge::encodeWireFrame(header, payload, wire, sizeof(wire), length));
        expected.insert(expected.end(), wire, wire + length);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bridge::EnqueueResult::Queued),
          static_cast<int>(queue.enqueue(bridge::Priority::L1s, wire, length)));
      }
      if (queue.activateFront()) {
        const size_t amount = queue.frontRemaining() < 12U ? queue.frontRemaining() : 12U;
        actual.insert(actual.end(), queue.frontData(), queue.frontData() + amount); (void)queue.advance(amount);
      }
      display.tick(tick, us);
    }
    TEST_ASSERT_EQUAL_UINT32(400U, received); TEST_ASSERT_EQUAL_UINT32(expected.size(), actual.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
    TEST_ASSERT_EQUAL_UINT32(0U, pps.stats().lost); TEST_ASSERT_EQUAL_UINT32(0U, pps.stats().abnormalInterval);
  }
}
}

void run_oled_tests() {
  RUN_TEST(test_i2c_success_steps_and_last_byte_ack_before_stop);
  RUN_TEST(test_i2c_errors_release_peripheral);
  RUN_TEST(test_i2c_every_phase_has_one_deadline_across_rollover);
  RUN_TEST(test_i2c_rejects_invalid_transfer_lengths);
  RUN_TEST(test_ssd1306_initialization_and_ram_contents);
  RUN_TEST(test_ssd1306_unchanged_skipped_and_one_row_updated);
  RUN_TEST(test_ssd1306_changes_during_upload_use_stable_row_snapshot);
  RUN_TEST(test_ssd1306_missing_screen_backoff_and_recovery);
  RUN_TEST(test_ssd1306_pending_timeout_and_start_rejection);
  RUN_TEST(test_ssd1306_bus_setup_failure_also_backs_off);
  RUN_TEST(test_ssd1306_error_after_online_reinitializes_all_rows);
  RUN_TEST(test_ssd1306_uart_backlog_defers_start_but_not_timeout);
  RUN_TEST(test_ssd1306_wait_and_retry_across_millis_rollover);
  RUN_TEST(test_text_clipping_unknown_glyphs_and_edges);
  RUN_TEST(test_status_startup_wait_and_real_failure_are_distinct);
  RUN_TEST(test_status_l1s_counts_valid_frames_and_retains_last_mt);
  RUN_TEST(test_status_time_quality_requires_current_evidence);
  RUN_TEST(test_status_quality_labels_do_not_invent_lock);
  RUN_TEST(test_status_single_pps_is_seen_without_confirming_lock);
  RUN_TEST(test_status_bridge_drop_history_and_counter_bounds);
  RUN_TEST(test_status_config_error_pps_loss_and_text_mode);
  RUN_TEST(test_status_refresh_and_age_rollover);
  RUN_TEST(test_status_long_silence_does_not_revive_l1s);
  RUN_TEST(test_real_ubx_and_bridge_bytes_survive_oled_fault_simulation);
}
