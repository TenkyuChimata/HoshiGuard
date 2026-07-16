#pragma once

#include <stddef.h>
#include <stdint.h>

#include "real/qzss_l1s_mt43_pab53.h"
#include "real/qzss_l1s_mt43_pab9a.h"
#include "real/qzss_l1s_mt43_pabc6.h"
#include "real/qzss_l1s_mt44_pab53.h"
#include "real/qzss_l1s_mt44_pab9a.h"
#include "real/qzss_l1s_mt44_pabc6.h"
#include "real/qzss_l1s_mt47.h"
#include "real/qzss_l1s_mt48.h"
#include "real/qzss_l1s_mt49.h"
#include "real/qzss_l1s_mt50.h"
#include "real/qzss_l1ca_negative.h"
#include "real/non_qzss_negative.h"
#include "real/ubx_checksum_corrupted.h"
#include "real/sfrbx_length_mismatch.h"

struct RealFixture {
  const char *name;
  const uint8_t *data;
  size_t length;
  bool expectedUbxAccept;
  bool expectedL1s;
  uint8_t pab;
  uint8_t messageType;
  uint8_t numWords;
};

static const RealFixture kRealFixtures[] = {
    {"qzss_l1s_mt43_pab53", hoshiguard_fixture::kQzssL1sMt43Pab53, hoshiguard_fixture::kQzssL1sMt43Pab53Length, true, true, 83U, 43U, 9U},
    {"qzss_l1s_mt43_pab9a", hoshiguard_fixture::kQzssL1sMt43Pab9a, hoshiguard_fixture::kQzssL1sMt43Pab9aLength, true, true, 154U, 43U, 9U},
    {"qzss_l1s_mt43_pabc6", hoshiguard_fixture::kQzssL1sMt43Pabc6, hoshiguard_fixture::kQzssL1sMt43Pabc6Length, true, true, 198U, 43U, 9U},
    {"qzss_l1s_mt44_pab53", hoshiguard_fixture::kQzssL1sMt44Pab53, hoshiguard_fixture::kQzssL1sMt44Pab53Length, true, true, 83U, 44U, 9U},
    {"qzss_l1s_mt44_pab9a", hoshiguard_fixture::kQzssL1sMt44Pab9a, hoshiguard_fixture::kQzssL1sMt44Pab9aLength, true, true, 154U, 44U, 9U},
    {"qzss_l1s_mt44_pabc6", hoshiguard_fixture::kQzssL1sMt44Pabc6, hoshiguard_fixture::kQzssL1sMt44Pabc6Length, true, true, 198U, 44U, 9U},
    {"qzss_l1s_mt47", hoshiguard_fixture::kQzssL1sMt47, hoshiguard_fixture::kQzssL1sMt47Length, true, true, 83U, 47U, 9U},
    {"qzss_l1s_mt48", hoshiguard_fixture::kQzssL1sMt48, hoshiguard_fixture::kQzssL1sMt48Length, true, true, 154U, 48U, 9U},
    {"qzss_l1s_mt49", hoshiguard_fixture::kQzssL1sMt49, hoshiguard_fixture::kQzssL1sMt49Length, true, true, 83U, 49U, 9U},
    {"qzss_l1s_mt50", hoshiguard_fixture::kQzssL1sMt50, hoshiguard_fixture::kQzssL1sMt50Length, true, true, 198U, 50U, 9U},
    {"qzss_l1ca_negative", hoshiguard_fixture::kQzssL1caNegative, hoshiguard_fixture::kQzssL1caNegativeLength, true, false, 0U, 0U, 0U},
    {"non_qzss_negative", hoshiguard_fixture::kNonQzssNegative, hoshiguard_fixture::kNonQzssNegativeLength, true, false, 0U, 0U, 0U},
    {"ubx_checksum_corrupted", hoshiguard_fixture::kUbxChecksumCorrupted, hoshiguard_fixture::kUbxChecksumCorruptedLength, false, false, 0U, 0U, 0U},
    {"sfrbx_length_mismatch", hoshiguard_fixture::kSfrbxLengthMismatch, hoshiguard_fixture::kSfrbxLengthMismatchLength, true, false, 0U, 0U, 0U},
};
static const size_t kRealFixtureCount = sizeof(kRealFixtures) / sizeof(kRealFixtures[0]);
