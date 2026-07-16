#pragma once

#include <stdint.h>

#include "qzss_l1s.h"

struct DcrDecodeResult {
  bool supported;
  bool fieldsValid;
  uint8_t reportClassification;
  uint8_t disasterCategory;
  uint8_t reportMonth;
  uint8_t reportDay;
  uint8_t reportHour;
  uint8_t reportMinute;
  uint8_t informationType;
  uint8_t version;
  const char *categoryName;
  const char *issuerName;
};

struct DcxDecodeResult {
  bool supported;
  uint8_t satelliteDesignationType;
  uint16_t satelliteDesignationMask;
  uint8_t alertMessageType;
  uint16_t countryRegion;
  uint8_t provider;
  uint8_t hazardCategoryType;
  uint8_t severity;
  uint8_t onsetWeek;
  uint16_t onsetMinuteOfWeek;
  uint8_t duration;
  uint8_t libraryType;
  uint8_t libraryVersion;
  uint16_t instructionCode;
  uint8_t specificSettingsType;
  uint8_t version;
  const char *alertMessageName;
};

bool decodeDcrMt43(const QzssL1sFrame &frame, DcrDecodeResult &result);
bool decodeDcxMt44(const QzssL1sFrame &frame, DcxDecodeResult &result);
const char *dcrCategoryName(uint8_t category);
