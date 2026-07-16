#include "gnss_config.h"

#include "config.h"

namespace {
uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeLe32(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

const ConfigKeySpec kItems[] = {
    {0x1031001FUL, 1U, 1U, ConfigRequirement::Required, true,
     "CFG-SIGNAL-GPS_ENA"},
    {0x10310001UL, 1U, 1U, ConfigRequirement::Required, true,
     "CFG-SIGNAL-GPS_L1CA_ENA"},
    {CFG_SIGNAL_QZSS_ENA, 1U, 1U, ConfigRequirement::Required, true,
     "CFG-SIGNAL-QZSS_ENA"},
    {CFG_SIGNAL_QZSS_L1S_ENA, 1U, 1U, ConfigRequirement::Required, true,
     "CFG-SIGNAL-QZSS_L1S_ENA"},
    {CFG_UART1OUTPROT_UBX, 1U, 1U, ConfigRequirement::Required, false,
     "CFG-UART1OUTPROT-UBX"},
    {0x10740002UL, KEEP_NMEA_OUTPUT ? 1U : 0U, 1U,
     ConfigRequirement::Optional, false, "CFG-UART1OUTPROT-NMEA"},
    {CFG_MSGOUT_RXM_SFRBX_UART1, 1U, 1U, ConfigRequirement::Required, false,
     "CFG-MSGOUT-UBX_RXM_SFRBX_UART1"},
    {CFG_MSGOUT_NAV_PVT_UART1, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-MSGOUT-UBX_NAV_PVT_UART1"},
#if CONFIGURE_TIMEPULSE
    {CFG_MSGOUT_TIM_TP_UART1, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-MSGOUT-UBX_TIM_TP_UART1"},
#endif
#if CONFIGURE_TIMEPULSE
    {0x20050023UL, 0U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-PULSE_DEF"},
    {0x20050030UL, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-PULSE_LENGTH_DEF"},
    {0x40050002UL, 1000000U, 4U, ConfigRequirement::Optional, false,
     "CFG-TP-PERIOD_TP1"},
    {0x40050003UL, 1000000U, 4U, ConfigRequirement::Optional, false,
     "CFG-TP-PERIOD_LOCK_TP1"},
    {0x40050004UL, 100000U, 4U, ConfigRequirement::Optional, false,
     "CFG-TP-LEN_TP1"},
    {0x40050005UL, 100000U, 4U, ConfigRequirement::Optional, false,
     "CFG-TP-LEN_LOCK_TP1"},
    {CFG_TP_SYNC_GNSS_TP1, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-SYNC_GNSS_TP1"},
    {0x10050009UL, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-USE_LOCKED_TP1"},
    {CFG_TP_ALIGN_TO_TOW_TP1, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-ALIGN_TO_TOW_TP1"},
    {CFG_TP_POL_TP1, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-POL_TP1"},
    {CFG_TP_TIMEGRID_TP1, 0U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-TIMEGRID_TP1"},
    {CFG_TP_TP1_ENA, 1U, 1U, ConfigRequirement::Optional, false,
     "CFG-TP-TP1_ENA"},
#endif
};
}  // namespace

size_t buildCfgValgetPayload(uint32_t key, uint8_t *output,
                             size_t capacity) {
  if (output == 0 || capacity < 8U) return 0U;
  output[0] = 0U;
  output[1] = 0U;  // RAM layer
  output[2] = 0U;
  output[3] = 0U;  // position
  writeLe32(output + 4, key);
  return 8U;
}

size_t buildCfgValsetPayload(uint32_t key, uint32_t value, uint8_t width,
                             uint8_t layers, uint8_t *output,
                             size_t capacity) {
  if (output == 0 || (width != 1U && width != 2U && width != 4U) ||
      capacity < static_cast<size_t>(8U + width)) {
    return 0U;
  }
  output[0] = 0U;
  output[1] = layers;
  output[2] = 0U;
  output[3] = 0U;
  writeLe32(output + 4, key);
  for (uint8_t i = 0; i < width; ++i)
    output[8U + i] = static_cast<uint8_t>(value >> (8U * i));
  return static_cast<size_t>(8U + width);
}

bool parseCfgValgetResponse(const uint8_t *payload, uint16_t length,
                            uint32_t expectedKey, uint8_t width,
                            uint32_t &value) {
  value = 0U;
  if (payload == 0 || (width != 1U && width != 2U && width != 4U) ||
      length != static_cast<uint16_t>(8U + width) || payload[0] != 1U ||
      payload[1] != 0U || payload[2] != 0U || payload[3] != 0U ||
      readLe32(payload + 4) != expectedKey) {
    return false;
  }
  for (uint8_t i = 0; i < width; ++i)
    value |= static_cast<uint32_t>(payload[8U + i]) << (8U * i);
  return true;
}

ConfigAction decideConfigAction(bool readSucceeded, uint32_t currentValue,
                                uint32_t targetValue, bool onlyOnChange) {
  if (!readSucceeded) return ConfigAction::FallbackApply;
  if (onlyOnChange && currentValue == targetValue) return ConfigAction::Skip;
  return ConfigAction::Apply;
}

bool isConfigFailureFatal(ConfigRequirement requirement) {
  return requirement == ConfigRequirement::Required;
}

bool configEvidenceVerified(const ConfigEvidence &evidence) {
  return evidence.result == ConfigResult::Verified &&
         evidence.hasCurrentValue &&
         evidence.currentValue == evidence.targetValue;
}

const ConfigKeySpec *gnssConfigItems(size_t &count) {
  count = sizeof(kItems) / sizeof(kItems[0]);
  return kItems;
}

const char *configRequirementName(ConfigRequirement requirement) {
  return requirement == ConfigRequirement::Required ? "required" : "optional";
}

const char *configResultName(ConfigResult result) {
  switch (result) {
    case ConfigResult::Pending: return "pending";
    case ConfigResult::Verified: return "verified";
    case ConfigResult::Changed: return "changed";
    case ConfigResult::Unsupported: return "unsupported";
    case ConfigResult::Timeout: return "timeout";
    case ConfigResult::Failed: return "failed";
  }
  return "unknown";
}
