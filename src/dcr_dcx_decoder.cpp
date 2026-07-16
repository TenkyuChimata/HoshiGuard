#include "dcr_dcx_decoder.h"

#include <string.h>

const char *dcrCategoryName(uint8_t category) {
  switch (category) {
    case 1: return "earthquake_early_warning";
    case 2: return "hypocenter";
    case 3: return "seismic_intensity";
    case 4: return "nankai_trough_earthquake";
    case 5: return "tsunami";
    case 6: return "northwest_pacific_tsunami";
    case 8: return "volcano";
    case 9: return "ash_fall";
    case 10: return "weather";
    case 11: return "flood";
    case 12: return "typhoon";
    case 14: return "marine";
    default: return "unsupported_category";
  }
}

bool decodeDcrMt43(const QzssL1sFrame &frame, DcrDecodeResult &result) {
  memset(&result, 0, sizeof(result));
  if (frame.messageType != 43U) return false;
  BitReader bits(frame.raw, QZSS_L1S_VALID_BITS);
  uint32_t value = 0;
#define READ_DCR(offset, width, member) \
  do { \
    if (!bits.read((offset), (width), value)) return false; \
    result.member = static_cast<uint8_t>(value); \
  } while (0)
  READ_DCR(14U, 3U, reportClassification);
  READ_DCR(17U, 4U, disasterCategory);
  READ_DCR(21U, 4U, reportMonth);
  READ_DCR(25U, 5U, reportDay);
  READ_DCR(30U, 5U, reportHour);
  READ_DCR(35U, 6U, reportMinute);
  READ_DCR(41U, 2U, informationType);
  READ_DCR(214U, 6U, version);
#undef READ_DCR
  result.categoryName = dcrCategoryName(result.disasterCategory);
  result.issuerName = "Japan Meteorological Agency (JMA)";
  result.supported = result.disasterCategory == 1U ||
                     result.disasterCategory == 2U ||
                     result.disasterCategory == 3U ||
                     result.disasterCategory == 4U ||
                     result.disasterCategory == 5U ||
                     result.disasterCategory == 6U ||
                     (result.disasterCategory >= 8U &&
                      result.disasterCategory <= 12U) ||
                     result.disasterCategory == 14U;
  result.fieldsValid = result.reportMonth >= 1U && result.reportMonth <= 12U &&
                       result.reportDay >= 1U && result.reportDay <= 31U &&
                       result.reportHour <= 23U && result.reportMinute <= 59U;
  return true;
}

bool decodeDcxMt44(const QzssL1sFrame &frame, DcxDecodeResult &result) {
  memset(&result, 0, sizeof(result));
  if (frame.messageType != 44U) return false;
  BitReader bits(frame.raw, QZSS_L1S_VALID_BITS);
  uint32_t value = 0;
#define READ_DCX(offset, width, member, type) \
  do { \
    if (!bits.read((offset), (width), value)) return false; \
    result.member = static_cast<type>(value); \
  } while (0)
  READ_DCX(14U, 1U, satelliteDesignationType, uint8_t);
  READ_DCX(15U, 9U, satelliteDesignationMask, uint16_t);
  READ_DCX(24U, 2U, alertMessageType, uint8_t);
  READ_DCX(26U, 9U, countryRegion, uint16_t);
  READ_DCX(35U, 5U, provider, uint8_t);
  READ_DCX(40U, 7U, hazardCategoryType, uint8_t);
  READ_DCX(47U, 2U, severity, uint8_t);
  READ_DCX(49U, 1U, onsetWeek, uint8_t);
  READ_DCX(50U, 14U, onsetMinuteOfWeek, uint16_t);
  READ_DCX(64U, 2U, duration, uint8_t);
  READ_DCX(66U, 1U, libraryType, uint8_t);
  READ_DCX(67U, 3U, libraryVersion, uint8_t);
  READ_DCX(70U, 10U, instructionCode, uint16_t);
  READ_DCX(129U, 2U, specificSettingsType, uint8_t);
  READ_DCX(214U, 6U, version, uint8_t);
#undef READ_DCX
  static const char *const names[4] = {"test", "alert", "update", "all_clear"};
  result.alertMessageName = names[result.alertMessageType & 3U];
  result.supported = true;  // All CAMF common fields above are version-004 fields.
  return true;
}
