#include "tim_tp.h"

#include <string.h>

namespace {
uint16_t readLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

bool parseUbxTimTp(const uint8_t *payload, uint16_t length, UbxTimTp &result) {
  memset(&result, 0, sizeof(result));
  if (payload == 0 || length != UBX_TIM_TP_PAYLOAD_LENGTH) return false;
  result.towMs = readLe32(payload);
  result.towSubMs = readLe32(payload + 4U);
  result.qErrPs = static_cast<int32_t>(readLe32(payload + 8U));
  result.week = readLe16(payload + 12U);
  result.flags = payload[14];
  result.refInfo = payload[15];
  result.timeBaseUtc = (result.flags & 0x01U) != 0U;
  result.utcAvailable = (result.flags & 0x02U) != 0U;
  result.raim = static_cast<uint8_t>((result.flags >> 2U) & 0x03U);
  result.qErrValid = (result.flags & 0x10U) == 0U;
  result.tpLocked = (result.flags & 0x20U) == 0U;
  result.timeRefGnss = static_cast<uint8_t>(result.refInfo & 0x0FU);
  result.utcStandard = static_cast<uint8_t>(result.refInfo >> 4U);
  return true;
}
