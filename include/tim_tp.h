#pragma once

#include <stdint.h>

static const uint8_t UBX_CLASS_TIM = 0x0DU;
static const uint8_t UBX_ID_TIM_TP = 0x01U;
static const uint16_t UBX_TIM_TP_PAYLOAD_LENGTH = 16U;

struct UbxTimTp {
  uint32_t towMs;
  uint32_t towSubMs;  // 単位は 2^-32 ms。浮動小数点を避けるため原値を保持する。
  int32_t qErrPs;     // 公式の単位は ps。
  uint16_t week;
  uint8_t flags;
  uint8_t refInfo;
  bool timeBaseUtc;
  bool utcAvailable;
  uint8_t raim;
  bool qErrValid;
  bool tpLocked;
  uint8_t timeRefGnss;
  uint8_t utcStandard;
};

bool parseUbxTimTp(const uint8_t *payload, uint16_t length, UbxTimTp &result);

