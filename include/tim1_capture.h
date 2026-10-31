#pragma once

#include <stdint.h>

namespace pps_hardware {
// 周波数は RCC と基板の発振器定義による公称値。UTC 精度を表す定数ではない。
static const uint32_t kCounterHz = 72000000UL;
static const uint32_t kCounterPeriod = 65536UL;
// RM0008 の HPRE/PPRE2 を SYSCLK から復号する。古い SystemCoreClock に依存しない。
inline uint32_t tim1InputClock(uint32_t sysclk, uint32_t cfgr) {
  static const uint8_t ahbShift[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
  const uint32_t hclk = sysclk >> ahbShift[(cfgr >> 4U) & 15U];
  const uint32_t ppre2 = (cfgr >> 11U) & 7U;
  return ppre2 < 4U ? hclk : (hclk >> (ppre2 - 3U)) * 2U;
}

// RM0008: CC1IF は CCR1 読み出しで、CC1OF は rc_w0 書き込みで消去する。
static const uint32_t kCc1If = 1U << 1;
static const uint32_t kCc1Of = 1U << 9;
static const uint32_t kValid = 1UL << 16;
static const uint32_t kOvercapture = 1UL << 17;
static const uint32_t kReadRace = 1UL << 18;
static const uint32_t kUnavailable = 1UL << 19;

template <class Registers>
uint32_t readCapture(Registers &io) {
  const uint32_t before = io.status();
  const uint16_t captured = (before & kCc1If) != 0U ? io.capture() : 0U;
  const uint32_t after = io.status();
  uint32_t flags = 0U;
  if (((before | after) & kCc1Of) != 0U) flags |= kOvercapture;
  // ES096 の CCR 読み出し競合では CC1IF/CC1OF が残らない場合がある。
  // EXTI はコールバック前に pending を消去済みなので、再設定は別エッジの証拠になる。
  if ((after & kCc1If) != 0U || io.edgePending()) flags |= kReadRace;
  if (flags != 0U) {
    // 競合時だけ一時停止して再同期する。この間のエッジも EXTI は引き続き数える。
    io.discard();
  } else if ((before & kCc1If) != 0U) {
    flags = kValid;
  }
  // UIF は参照も消去もしない。上位カウントを推測しないため順序依存がない。
  return flags | captured;
}

struct Evidence {
  uint16_t capturedCounter;
  uint16_t intervalModuloTicks;
  bool captureValid;
  bool intervalModuloValid;
  bool overcapture;
  bool readRace;
  bool unavailable;
};

class IntervalEvidence {
 public:
  IntervalEvidence() : previousSequence_(0U), previousCounter_(0U), havePrevious_(false) {}
  Evidence observe(uint32_t sequence, uint32_t sample) {
    Evidence out = {};
    out.capturedCounter = static_cast<uint16_t>(sample);
    out.captureValid = (sample & (kValid | kOvercapture | kReadRace | kUnavailable)) == kValid;
    out.overcapture = (sample & kOvercapture) != 0U;
    out.readRace = (sample & kReadRace) != 0U;
    out.unavailable = (sample & kUnavailable) != 0U;
    out.intervalModuloValid = out.captureValid && havePrevious_ &&
                              sequence - previousSequence_ == 1U;
    if (out.intervalModuloValid)
      out.intervalModuloTicks = static_cast<uint16_t>(out.capturedCounter - previousCounter_);
    previousSequence_ = sequence;
    previousCounter_ = out.capturedCounter;
    havePrevious_ = out.captureValid;
    return out;
  }

 private:
  uint32_t previousSequence_;
  uint16_t previousCounter_;
  bool havePrevious_;
};

// 無効化・競合時にも EXTI の登録と通し番号には触れない。
bool beginTim1();
bool tim1Active();
uint32_t sampleTim1();
void discardTim1();
}  // namespace pps_hardware
