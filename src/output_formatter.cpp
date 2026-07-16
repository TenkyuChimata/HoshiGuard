#include "output_formatter.h"

#include <stdio.h>
#include <string.h>

#include "safe_format.h"

namespace {
void rawHex(const uint8_t *raw, size_t length, char *buffer, size_t capacity) {
  static const char hex[] = "0123456789ABCDEF";
  if (raw == 0 || buffer == 0 || capacity == 0U) return;
  size_t position = 0;
  for (size_t i = 0; i < length && position + 2U < capacity; ++i) {
    buffer[position++] = hex[raw[i] >> 4];
    buffer[position++] = hex[raw[i] & 0x0F];
  }
  buffer[position] = '\0';
}
}

void formatUtc(const GnssUtcTime &utc, char *buffer, size_t capacity) {
  if (!utc.valid) {
    (void)safeSnprintf(buffer, capacity, "invalid");
    return;
  }
  (void)safeSnprintf(buffer, capacity, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                     utc.year, utc.month, utc.day, utc.hour, utc.minute,
                     utc.second);
}

void printL1sFrame(Print &output, const QzssL1sFrame &frame,
                   const GnssUtcTime &utc) {
  char raw[65];
  char timestamp[24];
  rawHex(frame.raw, sizeof(frame.raw), raw, sizeof(raw));
  formatUtc(utc, timestamp, sizeof(timestamp));
  const char *name = frame.messageType == 43U ? "DC Report" :
                     frame.messageType == 44U ? "DCX" : "unsupported";
  char received[21];
  if (formatUint64Decimal(frame.receivedMicros, received, sizeof(received)) == 0U)
    received[0] = '0', received[1] = '\0';
#if JSON_OUTPUT
  char line[400];
  const SafeFormatResult formatted = safeSnprintf(line, sizeof(line),
           "{\"type\":\"qzss_l1s\",\"sv_id\":%u,\"message_type\":%u,"
           "\"message_name\":\"%s\",\"pab\":%u,\"valid_pab\":%s,"
           "\"valid_crc\":%s,\"duplicate\":%s,\"pps_sequence\":%lu,"
           "\"time_pps_link\":%s,\"time_quality\":\"%s\","
           "\"received_micros\":%s,"
           "\"utc\":\"%s\",\"raw\":\"%s\"}",
           frame.svId, frame.messageType, name, frame.pab,
           frame.validPab ? "true" : "false",
           frame.validCrc ? "true" : "false",
           frame.duplicate ? "true" : "false",
           static_cast<unsigned long>(frame.ppsSequence),
           utc.associatedWithPps ? "true" : "false",
           timeQualityName(utc.quality),
           received, timestamp, raw);
  safePrintln(output, line, formatted);
#else
  output.print(F("L1S sv=")); output.print(frame.svId);
  output.print(F(" PAB=0x")); if (frame.pab < 16U) output.print('0');
  output.print(frame.pab, HEX);
  output.print(F(" MT=")); output.print(frame.messageType);
  output.print(' '); output.print(name);
  output.print(F(" PAB_OK=")); output.print(frame.validPab ? F("yes") : F("no"));
  output.print(F(" CRC=")); output.print(frame.validCrc ? F("ok") : F("bad"));
  output.print(F(" duplicate=")); output.print(frame.duplicate ? F("yes") : F("no"));
  output.print(F(" PPS=")); output.print(frame.ppsSequence);
  output.print(F(" UTC=")); output.print(timestamp);
  output.print(F(" time_pps_link="));
  output.print(utc.associatedWithPps ? F("yes") : F("no"));
  output.print(F(" time_quality=")); output.print(timeQualityName(utc.quality));
  output.print(F(" received_us="));
  const SafeFormatResult formatted = {
      strlen(received), SafeFormatStatus::Complete};
  safePrintln(output, received, formatted);
#if RAW_HEX_OUTPUT
  output.print(F("RAW250+pad=")); output.println(raw);
#endif
#endif
}

void printDcrResult(Print &output, const DcrDecodeResult &r) {
#if !JSON_OUTPUT
  output.print(F("DCR class=")); output.print(r.reportClassification);
  output.print(F(" issuer=")); output.print(r.issuerName);
  output.print(F(" category=")); output.print(r.disasterCategory);
  output.print(' '); output.print(r.categoryName);
  output.print(F(" report_utc="));
  if (r.fieldsValid) {
    char value[20];
    (void)safeSnprintf(value, sizeof(value), "%02u-%02uT%02u:%02uZ",
                       r.reportMonth, r.reportDay, r.reportHour,
                       r.reportMinute);
    output.print(value);
  } else {
    output.print(F("invalid"));
  }
  output.print(F(" info_type=")); output.print(r.informationType);
  output.print(F(" version=")); output.println(r.version);
#else
  char line[320];
  const SafeFormatResult formatted = safeSnprintf(line, sizeof(line),
           "{\"type\":\"dc_report\",\"issuer\":\"JMA\","
           "\"report_classification\":%u,\"disaster_category\":%u,"
           "\"category_name\":\"%s\",\"report_month\":%u,"
           "\"report_day\":%u,\"report_hour\":%u,\"report_minute\":%u,"
           "\"report_fields_valid\":%s,\"information_type\":%u,"
           "\"version\":%u,\"category_recognized\":%s}",
           r.reportClassification, r.disasterCategory, r.categoryName,
           r.reportMonth, r.reportDay, r.reportHour, r.reportMinute,
           r.fieldsValid ? "true" : "false", r.informationType, r.version,
           r.supported ? "true" : "false");
  safePrintln(output, line, formatted);
#endif
}

void printDcxResult(Print &output, const DcxDecodeResult &r) {
#if !JSON_OUTPUT
  output.print(F("DCX alert=")); output.print(r.alertMessageName);
  output.print(F(" country=")); output.print(r.countryRegion);
  output.print(F(" provider=")); output.print(r.provider);
  output.print(F(" hazard=")); output.print(r.hazardCategoryType);
  output.print(F(" severity=")); output.print(r.severity);
  output.print(F(" onset_week=")); output.print(r.onsetWeek);
  output.print(F(" onset_minute=")); output.print(r.onsetMinuteOfWeek);
  output.print(F(" instruction=")); output.print(r.instructionCode);
  output.print(F(" version=")); output.println(r.version);
#else
  char line[360];
  const SafeFormatResult formatted = safeSnprintf(line, sizeof(line),
           "{\"type\":\"dcx\",\"alert_message_type\":%u,"
           "\"alert_message_name\":\"%s\",\"sd_type\":%u,"
           "\"sd_mask\":%u,\"country_region\":%u,\"provider\":%u,"
           "\"hazard_category_type\":%u,\"severity\":%u,"
           "\"onset_week\":%u,\"onset_minute_of_week\":%u,"
           "\"duration\":%u,\"library_type\":%u,\"library_version\":%u,"
           "\"instruction_code\":%u,\"specific_settings_type\":%u,"
           "\"version\":%u}",
           r.alertMessageType, r.alertMessageName,
           r.satelliteDesignationType, r.satelliteDesignationMask,
           r.countryRegion, r.provider, r.hazardCategoryType, r.severity,
           r.onsetWeek, r.onsetMinuteOfWeek, r.duration, r.libraryType,
           r.libraryVersion, r.instructionCode, r.specificSettingsType,
           r.version);
  safePrintln(output, line, formatted);
#endif
}
