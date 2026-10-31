#pragma once

#include <stddef.h>
#include <stdint.h>

enum class OledTransfer : uint8_t { Pending, Complete, Failed };

// STM32F1 の I2C レジスター。参照を注入し、同じ送信状態機械をホストでも検証する。
struct OledI2cRegisters {
  volatile uint32_t &cr1;
  volatile uint32_t &dr;
  volatile uint32_t &sr1;
  volatile uint32_t &sr2;
};

class OledI2cWriter {
 public:
  static const uint32_t kTimeoutUs = 3000U;
  explicit OledI2cWriter(OledI2cRegisters registers);
  bool start(const uint8_t *data, size_t length, uint32_t nowUs);
  OledTransfer poll(uint32_t nowUs);
  void stop();

 private:
  enum class State : uint8_t { Idle, BusFree, Start, Address, Data, Drain, Stop };
  OledI2cRegisters registers_;
  const uint8_t *data_;
  size_t length_;
  size_t offset_;
  uint32_t startedUs_;
  State state_;
};
