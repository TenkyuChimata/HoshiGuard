#pragma once

#include <Arduino.h>

#include "dcr_dcx_decoder.h"
#include "qzss_l1s.h"
#include "time_sync.h"

struct GnssUtcTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint32_t iTowMs;
  int32_t nano;
  uint32_t receivedMicros;
  bool valid;
  bool associatedWithPps;
  uint32_t ppsSequence;
  TimeQuality quality;
};

void formatUtc(const GnssUtcTime &utc, char *buffer, size_t capacity);
void printL1sFrame(Print &output, const QzssL1sFrame &frame,
                   const GnssUtcTime &utc);
void printDcrResult(Print &output, const DcrDecodeResult &result);
void printDcxResult(Print &output, const DcxDecodeResult &result);
