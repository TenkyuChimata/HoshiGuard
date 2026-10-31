#pragma once

#include <unity.h>
#include <string.h>
#include "fixtures/packets.h"
#include "qzss_l1s.h"
#include "ubx_parser.h"

// この固定ベクトルは合成・仕様検証用。実放送の証拠とは区別する。
static const uint8_t kDcxSample[32] = {
    0xC6, 0xB0, 0x60, 0x0D, 0xE2, 0xBF, 0x88, 0xE9,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xC0, 0x00, 0x00, 0x11, 0xA1, 0xC8, 0x4E, 0xD4};

inline void makeSfrbxPayload(uint8_t payload[40]) {
  memset(payload, 0, 40);
  payload[0] = 5;
  payload[1] = 4;
  payload[2] = 1;
  payload[4] = 8;
  payload[6] = 2;
  for (size_t word = 0; word < 8U; ++word) {
    for (size_t byte = 0; byte < 4U; ++byte) {
      payload[8U + word * 4U + byte] =
          kDcxSample[word * 4U + (3U - byte)];
    }
  }
}

inline const RealFixture &realFixture(const char *name) {
  for (size_t i = 0U; i < kRealFixtureCount; ++i)
    if (strcmp(kRealFixtures[i].name, name) == 0) return kRealFixtures[i];
  TEST_FAIL_MESSAGE("要求した実受信 fixture がありません");
  return kRealFixtures[0];
}

inline size_t makeSfrbxPacket(uint8_t sigId, uint8_t *packet, size_t capacity) {
  uint8_t payload[40];
  makeSfrbxPayload(payload);
  payload[2] = sigId;
  return UbxParser::buildPacket(0x02U, 0x13U, payload, sizeof(payload), packet, capacity);
}
