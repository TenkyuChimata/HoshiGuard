#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ConfigRequirement : uint8_t { Required, Optional };

enum class ConfigResult : uint8_t {
  Pending,
  Verified,
  Changed,
  Unsupported,
  Timeout,
  Failed
};

enum class ConfigAction : uint8_t { Skip, Apply, FallbackApply };

struct ConfigKeySpec {
  uint32_t key;
  uint32_t targetValue;
  uint8_t width;
  ConfigRequirement requirement;
  bool restartDelay;
  const char *name;
};

struct ConfigEvidence {
  uint32_t key;
  uint32_t targetValue;
  uint32_t currentValue;
  ConfigRequirement requirement;
  ConfigResult result;
  bool hasCurrentValue;
  bool valsetAcknowledged;
};

static const uint32_t CFG_UART1_BAUDRATE = 0x40520001UL;
static const uint32_t CFG_SIGNAL_QZSS_ENA = 0x10310024UL;
static const uint32_t CFG_SIGNAL_QZSS_L1S_ENA = 0x10310014UL;
static const uint32_t CFG_UART1OUTPROT_UBX = 0x10740001UL;
static const uint32_t CFG_MSGOUT_RXM_SFRBX_UART1 = 0x20910232UL;
static const uint32_t CFG_MSGOUT_NAV_PVT_UART1 = 0x20910007UL;
static const uint32_t CFG_MSGOUT_TIM_TP_UART1 = 0x2091017EUL;
static const uint32_t CFG_TP_TIMEGRID_TP1 = 0x2005000CUL;
static const uint32_t CFG_TP_TP1_ENA = 0x10050007UL;
static const uint32_t CFG_TP_SYNC_GNSS_TP1 = 0x10050008UL;
static const uint32_t CFG_TP_ALIGN_TO_TOW_TP1 = 0x1005000AUL;
static const uint32_t CFG_TP_POL_TP1 = 0x1005000BUL;

bool configEvidenceVerified(const ConfigEvidence &evidence);

// UBX-CFG-VALGET poll: version 0, RAM layer, position 0, one U4 key.
size_t buildCfgValgetPayload(uint32_t key, uint8_t *output,
                             size_t capacity);

// UBX-CFG-VALSET version 0, transactionless, one key/value pair.
size_t buildCfgValsetPayload(uint32_t key, uint32_t value, uint8_t width,
                             uint8_t layers, uint8_t *output,
                             size_t capacity);

// Parses one-key UBX-CFG-VALGET response (response version 1). The response
// cfgData begins at byte 4 and contains a little-endian U4 key immediately
// followed by its variable-width little-endian value.
bool parseCfgValgetResponse(const uint8_t *payload, uint16_t length,
                            uint32_t expectedKey, uint8_t width,
                            uint32_t &value);

ConfigAction decideConfigAction(bool readSucceeded, uint32_t currentValue,
                                uint32_t targetValue, bool onlyOnChange);
bool isConfigFailureFatal(ConfigRequirement requirement);

const ConfigKeySpec *gnssConfigItems(size_t &count);
const char *configRequirementName(ConfigRequirement requirement);
const char *configResultName(ConfigResult result);
