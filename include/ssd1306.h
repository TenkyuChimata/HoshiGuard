#pragma once

#include "oled_i2c.h"

namespace oled {
static const size_t kRows = 8U;
static const size_t kColumns = 21U;
struct Screen {
  char rows[kRows][kColumns + 1U];
  uint8_t inverted;
};

void setRow(Screen &screen, size_t row, const char *text);
uint8_t rasterColumn(const char *text, uint8_t column, bool inverted);

// begin/end は周辺回路の設定・解放のみ。start/poll は非同期で、data は完了まで有効。
struct Bus {
  void *context;
  bool (*begin)(void *);
  bool (*start)(void *, const uint8_t *, size_t, uint32_t);
  OledTransfer (*poll)(void *, uint32_t);
  void (*end)(void *);
};

class Ssd1306 {
 public:
  explicit Ssd1306(Bus bus);
  void begin(uint32_t nowMs);
  void setScreen(const Screen &screen);
  void tick(uint32_t nowMs, uint32_t nowUs, bool allowStart = true);
  bool online() const { return online_; }
  uint32_t failures() const { return failures_; }

 private:
  enum class State : uint8_t { Wait, Init0, Init1, Select, Position, Data, On };
  void fail(uint32_t nowMs);
  void completed();
  Bus bus_;
  Screen desired_;
  Screen sent_;
  char activeRow_[kColumns + 1U];
  uint8_t tx_[17];
  uint32_t waitSinceMs_;
  uint32_t waitMs_;
  uint32_t transferSinceUs_;
  uint32_t failures_;
  State state_;
  uint8_t dirty_;
  uint8_t row_;
  uint8_t column_;
  bool activeInverted_;
  bool pending_;
  bool online_;
};
}
