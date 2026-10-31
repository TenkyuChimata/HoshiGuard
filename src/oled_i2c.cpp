#include "oled_i2c.h"

namespace {
// RM0008 の I2C マスター送信シーケンス。割り込みと DMA は使用しない。
const uint32_t kStart = 1U << 8;
const uint32_t kStop = 1U << 9;
const uint32_t kSb = 1U << 0;
const uint32_t kAddr = 1U << 1;
const uint32_t kBtf = 1U << 2;
const uint32_t kTxe = 1U << 7;
const uint32_t kBusy = 1U << 1;
const uint32_t kErrors = (1U << 8) | (1U << 9) | (1U << 10) |
                         (1U << 11) | (1U << 14);
}

OledI2cWriter::OledI2cWriter(OledI2cRegisters registers)
    : registers_(registers), data_(0), length_(0U), offset_(0U),
      startedUs_(0U), state_(State::Idle) {}

bool OledI2cWriter::start(const uint8_t *data, size_t length, uint32_t nowUs) {
  if (state_ != State::Idle || data == 0 || length == 0U || length > 17U)
    return false;
  data_ = data;
  length_ = length;
  offset_ = 0U;
  startedUs_ = nowUs;
  state_ = State::BusFree;
  return true;
}

void OledI2cWriter::stop() {
  // NACK、BUSY 固着、途中切断時も周辺回路をリセットして SCL/SDA を解放する。
  registers_.cr1 = 1U << 15;
  registers_.cr1 = 0U;
  state_ = State::Idle;
}

OledTransfer OledI2cWriter::poll(uint32_t nowUs) {
  if (state_ == State::Idle) return OledTransfer::Complete;
  const uint32_t sr1 = registers_.sr1;
  if ((sr1 & kErrors) != 0U || nowUs - startedUs_ >= kTimeoutUs) {
    stop();
    return OledTransfer::Failed;
  }
  // 1 回につき 1 段階、最大 1 byte。待機ループも割り込み禁止区間も設けない。
  switch (state_) {
    case State::BusFree:
      if ((registers_.sr2 & kBusy) == 0U) {
        registers_.cr1 |= kStart;
        state_ = State::Start;
      }
      break;
    case State::Start:
      if ((sr1 & kSb) != 0U) {
        registers_.dr = 0x3CU << 1;
        state_ = State::Address;
      }
      break;
    case State::Address:
      if ((sr1 & kAddr) != 0U) {
        const uint32_t sr2 = registers_.sr2;  // SR1 → SR2 の順に読み、ADDR を解除する。
        (void)sr2;
        state_ = State::Data;
      }
      break;
    case State::Data:
      if ((sr1 & kTxe) != 0U) {
        registers_.dr = data_[offset_++];
        if (offset_ == length_) state_ = State::Drain;
      }
      break;
    case State::Drain:
      if ((sr1 & kBtf) != 0U) {
        registers_.cr1 |= kStop;
        state_ = State::Stop;
      }
      break;
    case State::Stop:
      if ((registers_.cr1 & kStop) == 0U &&
          (registers_.sr2 & kBusy) == 0U) {
        state_ = State::Idle;
        return OledTransfer::Complete;
      }
      break;
    case State::Idle: break;
  }
  return OledTransfer::Pending;
}
