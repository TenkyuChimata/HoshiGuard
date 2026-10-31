#include "ssd1306.h"

#include <string.h>

namespace oled {
namespace {
// 本プロジェクト用に行ビットで定義した 5x7 字形。外部の字形・画像データは含めない。
const char kCharacters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ !-.:+?/>";
const uint8_t kGlyphs[][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {0,0,0,0,0,0,0}, {4,4,4,4,4,0,4},
    {0,0,0,31,0,0,0}, {0,0,0,0,0,0,4},
    {0,4,0,0,4,0,0}, {0,4,4,31,4,4,0},
    {14,17,1,2,4,0,4}, {1,2,2,4,8,8,16},
    {16,8,4,2,4,8,16}};
static_assert(sizeof(kGlyphs) / sizeof(kGlyphs[0]) == sizeof(kCharacters) - 1U,
              "字形数の不一致");
// SSD1306 データシートに従う 128x64、内蔵 charge pump、page addressing。
const uint8_t kInit0[] = {0x00,0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,
                          0x40,0x20,0x02,0xA1,0xC8};
const uint8_t kInit1[] = {0x00,0xDA,0x12,0x81,0x7F,0xD9,0xF1,0xDB,
                          0x20,0x8D,0x14,0xA4,0xA6,0x2E};
}

void setRow(Screen &screen, size_t row, const char *text) {
  if (row >= kRows) return;
  size_t i = 0U;
  if (text != 0) {
    for (; i < kColumns && text[i] != '\0'; ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      screen.rows[row][i] = (c >= 32U && c <= 126U) ? static_cast<char>(c) : '?';
    }
  }
  for (; i <= kColumns; ++i) screen.rows[row][i] = '\0';
}

uint8_t rasterColumn(const char *text, uint8_t column, bool inverted) {
  uint8_t pixels = 0U;
  if (text != 0 && column < 126U) {
    const size_t cell = column / 6U;
    const size_t x = column % 6U;
    size_t end = 0U;
    while (end < cell && text[end] != '\0') ++end;
    char c = text[end];
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const char *glyph = c == '\0' ? 0 : strchr(kCharacters, c);
    if (glyph == 0 && c != '\0') glyph = strchr(kCharacters, '?');
    if (glyph != 0 && x < 5U) {
      const size_t index = static_cast<size_t>(glyph - kCharacters);
      for (size_t y = 0U; y < 7U; ++y)
        if ((kGlyphs[index][y] & (1U << (4U - x))) != 0U)
          pixels = static_cast<uint8_t>(pixels | (1U << y));
    }
  }
  return inverted ? static_cast<uint8_t>(~pixels) : pixels;
}

Ssd1306::Ssd1306(Bus bus)
    : bus_(bus), waitSinceMs_(0U), waitMs_(100U), transferSinceUs_(0U),
      failures_(0U), state_(State::Wait), dirty_(0xFFU), row_(0U), column_(0U),
      activeInverted_(false), pending_(false), online_(false) {
  memset(&desired_, 0, sizeof(desired_));
  memset(&sent_, 0, sizeof(sent_));
  memset(activeRow_, 0, sizeof(activeRow_));
}

void Ssd1306::begin(uint32_t nowMs) {
  waitSinceMs_ = nowMs;
  waitMs_ = 100U;
  state_ = State::Wait;
  pending_ = false;
  online_ = false;
  dirty_ = 0xFFU;
}

void Ssd1306::setScreen(const Screen &screen) {
  for (size_t row = 0U; row < kRows; ++row)
    setRow(desired_, row, screen.rows[row]);
  desired_.inverted = screen.inverted;
}

void Ssd1306::fail(uint32_t nowMs) {
  bus_.end(bus_.context);
  if (failures_ != UINT32_MAX) ++failures_;
  online_ = false;
  pending_ = false;
  dirty_ = 0xFFU;
  waitSinceMs_ = nowMs;
  waitMs_ = 30000U;
  state_ = State::Wait;
}

void Ssd1306::completed() {
  switch (state_) {
    case State::Init0: state_ = State::Init1; break;
    case State::Init1: state_ = State::Select; break;
    case State::Position: column_ = 0U; state_ = State::Data; break;
    case State::Data:
      column_ = static_cast<uint8_t>(column_ + 16U);
      if (column_ == 128U) {
        memcpy(sent_.rows[row_], activeRow_, sizeof(activeRow_));
        const uint8_t bit = static_cast<uint8_t>(1U << row_);
        sent_.inverted = static_cast<uint8_t>((sent_.inverted & ~bit) |
                                            (activeInverted_ ? bit : 0U));
        dirty_ = static_cast<uint8_t>(dirty_ & ~bit);
        row_ = static_cast<uint8_t>((row_ + 1U) % kRows);
        state_ = State::Select;
      }
      break;
    case State::On: online_ = true; state_ = State::Select; break;
    default: break;
  }
}

void Ssd1306::tick(uint32_t nowMs, uint32_t nowUs, bool allowStart) {
  if (pending_) {
    const OledTransfer result = bus_.poll(bus_.context, nowUs);
    if (result == OledTransfer::Failed ||
        nowUs - transferSinceUs_ >= OledI2cWriter::kTimeoutUs) {
      fail(nowMs);
    } else if (result == OledTransfer::Complete) {
      pending_ = false;
      completed();
    }
    return;
  }
  if (!allowStart) return;
  if (state_ == State::Wait) {
    if (nowMs - waitSinceMs_ < waitMs_) return;
    if (!bus_.begin(bus_.context)) { fail(nowMs); return; }
    state_ = State::Init0;
    return;
  }
  if (state_ == State::Select) {
    for (size_t i = 0U; i < kRows; ++i) {
      const uint8_t bit = static_cast<uint8_t>(1U << row_);
      if ((dirty_ & bit) != 0U ||
          ((desired_.inverted ^ sent_.inverted) & bit) != 0U ||
          memcmp(desired_.rows[row_], sent_.rows[row_], sizeof(activeRow_)) != 0) {
        memcpy(activeRow_, desired_.rows[row_], sizeof(activeRow_));
        activeInverted_ = (desired_.inverted & bit) != 0U;
        state_ = State::Position;
        break;
      }
      row_ = static_cast<uint8_t>((row_ + 1U) % kRows);
    }
    if (state_ == State::Select) {
      if (online_) return;
      state_ = State::On;
    }
  }
  size_t length = 0U;
  switch (state_) {
    case State::Init0: length = sizeof(kInit0); memcpy(tx_, kInit0, length); break;
    case State::Init1: length = sizeof(kInit1); memcpy(tx_, kInit1, length); break;
    case State::Position:
      tx_[0] = 0x00U; tx_[1] = static_cast<uint8_t>(0xB0U + row_);
      tx_[2] = 0x00U; tx_[3] = 0x10U; length = 4U;
      break;
    case State::Data:
      tx_[0] = 0x40U;
      for (uint8_t i = 0U; i < 16U; ++i)
        tx_[1U + i] = rasterColumn(activeRow_, static_cast<uint8_t>(column_ + i),
                                   activeInverted_);
      length = 17U;
      break;
    case State::On: tx_[0] = 0x00U; tx_[1] = 0xAFU; length = 2U; break;
    default: return;
  }
  if (!bus_.start(bus_.context, tx_, length, nowUs)) { fail(nowMs); return; }
  pending_ = true;
  transferSinceUs_ = nowUs;
}
}
