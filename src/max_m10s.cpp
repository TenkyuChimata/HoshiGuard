#include "max_m10s.h"

#include <string.h>

#include "startup_sequence.h"

namespace {
// MAX-M10S の工場出荷時 UART は通常 9600 baud。他の 2 候補は既存 HoshiGuard と利用者設定の互換性を保つ。
const uint32_t kBaudCandidates[] = {9600UL, 38400UL, 115200UL};
const size_t kBaudCandidateCount =
    sizeof(kBaudCandidates) / sizeof(kBaudCandidates[0]);
}

const char *gnssInitStateName(GnssInitState state) {
  switch (state) {
    case GnssInitState::Idle: return "Idle";
    case GnssInitState::DetectingBaud: return "DetectingBaud";
    case GnssInitState::PollingMonVer: return "PollingMonVer";
    case GnssInitState::ReadingConfig: return "ReadingConfig";
    case GnssInitState::ApplyingConfig: return "ApplyingConfig";
    case GnssInitState::WaitingForRestart: return "WaitingForRestart";
    case GnssInitState::VerifyingBaud: return "VerifyingBaud";
    case GnssInitState::RecoveringBaud: return "RecoveringBaud";
    case GnssInitState::VerifyingSfrbx: return "VerifyingSfrbx";
    case GnssInitState::Ready: return "Ready";
    case GnssInitState::Degraded: return "Degraded";
    case GnssInitState::Failed: return "Failed";
  }
  return "Unknown";
}

const char *gnssErrorName(GnssErrorCode error) {
  switch (error) {
    case GNSS_ERR_NONE: return "GNSS_OK";
    case GNSS_ERR_NO_RESPONSE: return "GNSS_ERR_NO_RESPONSE";
    case GNSS_ERR_NO_VALID_UBX: return "GNSS_ERR_NO_VALID_UBX";
    case GNSS_ERR_MON_VER_TIMEOUT: return "GNSS_ERR_MON_VER_TIMEOUT";
    case GNSS_ERR_CFG_NAK: return "GNSS_ERR_CFG_NAK";
    case GNSS_ERR_CFG_TIMEOUT: return "GNSS_ERR_CFG_TIMEOUT";
    case GNSS_ERR_BAUD_VERIFY: return "GNSS_ERR_BAUD_VERIFY";
    case GNSS_ERR_NO_SFRBX: return "GNSS_ERR_NO_SFRBX";
    case GNSS_ERR_NO_QZSS: return "GNSS_ERR_NO_QZSS";
    case GNSS_ERR_NO_L1S: return "GNSS_ERR_NO_L1S";
    case GNSS_ERR_INVALID_L1S_PAB: return "GNSS_ERR_INVALID_L1S_PAB";
    case GNSS_ERR_INVALID_L1S_CRC: return "GNSS_ERR_INVALID_L1S_CRC";
    case GNSS_ERR_UNSUPPORTED_MT: return "GNSS_ERR_UNSUPPORTED_MT";
    case GNSS_ERR_NO_RX_DATA: return "GNSS_ERR_NO_RX_DATA";
    case GNSS_ERR_RX_DATA_NO_VALID_PROTOCOL:
      return "GNSS_ERR_RX_DATA_NO_VALID_PROTOCOL";
    case GNSS_ERR_LIKELY_WRONG_BAUD: return "GNSS_ERR_LIKELY_WRONG_BAUD";
    case GNSS_ERR_NMEA_ONLY: return "GNSS_ERR_NMEA_ONLY";
    case GNSS_ERR_TX_SHORT_WRITE: return "GNSS_ERR_TX_SHORT_WRITE";
    case GNSS_ERR_RX_OK_TX_NO_RESPONSE:
      return "GNSS_ERR_RX_OK_TX_NO_RESPONSE";
    case GNSS_ERR_PROTOCOL_INPUT_DISABLED:
      return "GNSS_ERR_PROTOCOL_INPUT_DISABLED";
    case GNSS_ERR_BAUD_SWITCH_LOST_COMMUNICATION:
      return "GNSS_ERR_BAUD_SWITCH_LOST_COMMUNICATION";
  }
  return "GNSS_ERR_UNKNOWN";
}

MaxM10s::MaxM10s(HardwareSerial &serial, LogArbiter &log)
    : machine_(DETECT_SEND), serial_(serial), log_(log), status_(DETECTING),
      initState_(GnssInitState::Idle), lastError_(GNSS_ERR_NONE), items_(0),
      itemCount_(0), baudRate_(0), deadlineMs_(0), baudCandidate_(0),
      retryCount_(0), configIndex_(0), valgetSupported_(true),
      sawSfrbx_(false), degraded_(false), changeCounted_(false),
      serialStarted_(false), monVerReceived_(false),
      configurationFinished_(false), parserResetRequested_(false),
      detectedBaud_(0U), initialBaud_(0U), verifiedBaud_(0U),
      recoveryBaud_(0U), nmeaInferredBaud_(0U),
      baudSwitchState_(BaudSwitchState::Idle),
      baudSwitchResult_(BaudSwitchResult::Pending), baudValsetAck_(false),
      baudValsetWriteResult_(0U)
#if GNSS_RX_PROBE
      , probeDeadlineMs_(0U), probeActive_(false), probeReported_(false)
#endif
      {
  memset(&stats_, 0, sizeof(stats_));
  memset(evidence_, 0, sizeof(evidence_));
  memset(softwareVersion_, 0, sizeof(softwareVersion_));
  memset(hardwareVersion_, 0, sizeof(hardwareVersion_));
#if GNSS_RX_PROBE
  memset(baudProbe_, 0, sizeof(baudProbe_));
#endif
  items_ = gnssConfigItems(itemCount_);
  if (itemCount_ > MAX_CONFIG_EVIDENCE) itemCount_ = MAX_CONFIG_EVIDENCE;
  for (size_t i = 0; i < itemCount_; ++i) {
    evidence_[i].key = items_[i].key;
    evidence_[i].targetValue = items_[i].targetValue;
    evidence_[i].requirement = items_[i].requirement;
    evidence_[i].result = ConfigResult::Pending;
  }
}

const ConfigEvidence *MaxM10s::configEvidence(uint32_t key) const {
  for (size_t i = 0; i < itemCount_; ++i)
    if (evidence_[i].key == key) return &evidence_[i];
  return 0;
}

bool MaxM10s::timePulseConfigAllVerified() const {
  size_t found = 0U;
  for (size_t i = 0; i < itemCount_; ++i) {
    const bool timing = items_[i].key == CFG_MSGOUT_TIM_TP_UART1 ||
                        strncmp(items_[i].name, "CFG-TP-", 7U) == 0;
    if (!timing) continue;
    ++found;
    if (!configEvidenceVerified(evidence_[i])) return false;
  }
  return found != 0U;
}

bool MaxM10s::timePulseConfigAnyFailure() const {
  for (size_t i = 0; i < itemCount_; ++i) {
    const bool timing = items_[i].key == CFG_MSGOUT_TIM_TP_UART1 ||
                        strncmp(items_[i].name, "CFG-TP-", 7U) == 0;
    if (!timing) continue;
    const ConfigResult result = evidence_[i].result;
    if (result == ConfigResult::Unsupported || result == ConfigResult::Timeout ||
        result == ConfigResult::Failed) return true;
  }
  return false;
}

void MaxM10s::setEvidence(ConfigResult result, bool hasCurrent,
                          uint32_t current, bool valsetAcknowledged) {
  if (configIndex_ >= itemCount_) return;
  ConfigEvidence &evidence = evidence_[configIndex_];
  evidence.result = result;
  evidence.hasCurrentValue = hasCurrent;
  if (hasCurrent) evidence.currentValue = current;
  if (valsetAcknowledged) evidence.valsetAcknowledged = true;
}

const ConfigKeySpec &MaxM10s::configItem() const { return items_[configIndex_]; }

const char *MaxM10s::stageName() const {
  if (configIndex_ < itemCount_ &&
      (initState_ == GnssInitState::ReadingConfig ||
       initState_ == GnssInitState::ApplyingConfig ||
       initState_ == GnssInitState::WaitingForRestart)) {
    return configItem().name;
  }
  return gnssInitStateName(initState_);
}

void MaxM10s::transition(GnssInitState next, const char *reason,
                         uint32_t nowMs, GnssErrorCode error) {
  if (next == initState_ && error == GNSS_ERR_NONE) return;
  log_.setPriority(LogPriority::Critical);
  log_.print(F("GNSS_STATE from=")); log_.print(gnssInitStateName(initState_));
  log_.print(F(" to=")); log_.print(gnssInitStateName(next));
  log_.print(F(" reason=")); log_.print(reason);
  log_.print(F(" time_ms=")); log_.print(nowMs);
  log_.print(F(" retry=")); log_.print(retryCount_);
  if (error != GNSS_ERR_NONE) {
    lastError_ = error;
    log_.print(F(" error=")); log_.print(gnssErrorName(error));
  }
  log_.println();
  initState_ = next;
  switch (next) {
    case GnssInitState::Ready:
    case GnssInitState::Degraded: status_ = READY; break;
    case GnssInitState::Failed: status_ = FAILED; break;
    case GnssInitState::DetectingBaud:
    case GnssInitState::PollingMonVer: status_ = DETECTING; break;
    default: status_ = CONFIGURING; break;
  }
}

void MaxM10s::setBaud(uint32_t baud) {
  // STM32duino Core 2.4.0 では最初の begin() より前の HardwareSerial::end() は危険。
  // serial_t::index が 0（UART1_INDEX）のままなので、未使用 USART2 の終了が USART1 をリセットして診断出力を停止させる。
  if (shouldEndSerialBeforeBegin(serialStarted_)) serial_.end();
  serial_.begin(baud);
  serialStarted_ = true;
  while (serial_.available() > 0) (void)serial_.read();
  baudRate_ = baud;
  parserResetRequested_ = true;
}

void MaxM10s::begin(uint32_t nowMs) {
  status_ = DETECTING;
  lastError_ = GNSS_ERR_NONE;
  baudCandidate_ = 0;
  retryCount_ = 0;
  configIndex_ = 0;
  valgetSupported_ = true;
  sawSfrbx_ = false;
  degraded_ = false;
  changeCounted_ = false;
  monVerReceived_ = false;
  configurationFinished_ = false;
  detectedBaud_ = 0U;
  initialBaud_ = 0U;
  verifiedBaud_ = 0U;
  recoveryBaud_ = 0U;
  nmeaInferredBaud_ = 0U;
  baudSwitchState_ = BaudSwitchState::Idle;
  baudSwitchResult_ = BaudSwitchResult::Pending;
  baudValsetAck_ = false;
  baudValsetWriteResult_ = 0U;
  softwareVersion_[0] = '\0';
  hardwareVersion_[0] = '\0';
  for (size_t i = 0; i < itemCount_; ++i) {
    evidence_[i].currentValue = 0U;
    evidence_[i].result = ConfigResult::Pending;
    evidence_[i].hasCurrentValue = false;
    evidence_[i].valsetAcknowledged = false;
  }
  setBaud(kBaudCandidates[0]);
#if GNSS_RX_PROBE
  rxProbe_.reset();
  memset(&baudProbe_[0], 0, sizeof(baudProbe_[0]));
  probeActive_ = true;
  probeReported_ = false;
  probeDeadlineMs_ = 0U;
#endif
  machine_ = DETECT_SEND;
  deadlineMs_ = nowMs;
  transition(GnssInitState::DetectingBaud, "startup", nowMs);
}

size_t MaxM10s::sendPacket(uint8_t cls, uint8_t id,
                           const uint8_t *payload, uint16_t length,
                           uint32_t nowMs) {
  uint8_t packet[40];
  const size_t size = UbxParser::buildPacket(cls, id, payload, length,
                                             packet, sizeof(packet));
  if (size == 0U) return 0U;
  const size_t written = serial_.write(packet, size);
  recordGnssTx(stats_.tx, size, written);
#if GNSS_RX_PROBE
  if (baudCandidate_ < kBaudCandidateCount && cls == CLASS_MON &&
      id == ID_MON_VER && length == 0U) {
    BaudProbeRecord &record = baudProbe_[baudCandidate_];
    record.baud = baudRate_;
    record.monVerSent = true;
    record.monVerWriteResult = static_cast<uint16_t>(written);
  }
#endif
#if GNSS_TX_ECHO_HEX
  if (cls == CLASS_MON && id == ID_MON_VER && length == 0U) {
    char hex[81];
    formatGnssRxHex(packet, size, hex, sizeof(hex));
    log_.print(F("GNSS_TX time_ms=")); log_.print(nowMs);
    log_.print(F(" baud=")); log_.print(baudRate_);
    log_.print(F(" type=MON_VER_POLL len=")); log_.print(size);
    log_.print(F(" write_result=")); log_.print(written);
    log_.print(F(" data=")); log_.println(hex);
  }
#else
  (void)nowMs;
#endif
  if (written != size) {
    lastError_ = GNSS_ERR_TX_SHORT_WRITE;
    log_.print(F("GNSS_TX_ERROR baud=")); log_.print(baudRate_);
    log_.print(F(" requested=")); log_.print(size);
    log_.print(F(" written=")); log_.println(written);
  }
  return written;
}

void MaxM10s::pollMonVer(uint32_t nowMs) {
  sendPacket(CLASS_MON, ID_MON_VER, 0, 0, nowMs);
}

void MaxM10s::sendValget(uint32_t key) {
  uint8_t payload[8];
  const size_t length = buildCfgValgetPayload(key, payload, sizeof(payload));
  if (length != 0U) {
    sendPacket(CLASS_CFG, ID_CFG_VALGET, payload,
               static_cast<uint16_t>(length));
    ++stats_.valgetSent;
  }
}

void MaxM10s::sendValset(const ConfigKeySpec &item) {
  uint8_t payload[12];
  const uint8_t layers = static_cast<uint8_t>(
      0x01U | (CONFIGURE_GNSS_BBR ? 0x02U : 0U));
  const size_t length = buildCfgValsetPayload(
      item.key, item.targetValue, item.width, layers, payload, sizeof(payload));
  if (length != 0U) {
    sendPacket(CLASS_CFG, ID_CFG_VALSET, payload,
               static_cast<uint16_t>(length));
    ++stats_.valsetSent;
  }
}

void MaxM10s::printConfigDecision(const ConfigKeySpec &item,
                                  const char *current, bool change,
                                  ConfigResult result) {
  log_.setPriority(LogPriority::Config);
  log_.print(F("CFG key=0x")); log_.print(item.key, HEX);
  log_.print(F(" name=")); log_.print(item.name);
  log_.print(F(" class=")); log_.print(configRequirementName(item.requirement));
  log_.print(F(" current=")); log_.print(current);
  log_.print(F(" target=")); log_.print(item.targetValue);
  log_.print(F(" change=")); log_.print(change ? F("yes") : F("no"));
  log_.print(F(" result=")); log_.println(configResultName(result));
}

void MaxM10s::beginConfiguration(uint32_t nowMs) {
  configIndex_ = 0;
  retryCount_ = 0;
  changeCounted_ = false;
#if CONFIG_ONLY_ON_CHANGE
  machine_ = READ_SEND;
  transition(GnssInitState::ReadingConfig, "MON-VER response", nowMs);
#else
  machine_ = APPLY_SEND;
  transition(GnssInitState::ApplyingConfig, "configuration read disabled", nowMs);
#endif
  deadlineMs_ = nowMs;
}

void MaxM10s::logBaudSwitch(const char *stage, uint32_t nowMs) {
  log_.setPriority(LogPriority::Critical);
  log_.print(F("GNSS_BAUD_SWITCH time_ms=")); log_.print(nowMs);
  log_.print(F(" from=")); log_.print(initialBaud_);
  log_.print(F(" to=")); log_.print(GNSS_TARGET_BAUD);
  log_.print(F(" active=")); log_.print(baudRate_);
  log_.print(F(" stage=")); log_.print(stage);
  log_.print(F(" ack=")); log_.print(baudValsetAck_ ? F("yes") : F("no"));
  log_.print(F(" write_result=")); log_.println(baudValsetWriteResult_);
}

size_t MaxM10s::sendBaudValset(uint32_t nowMs) {
  uint8_t packet[24];
  const uint8_t layers = static_cast<uint8_t>(
      0x01U | (CONFIGURE_GNSS_BBR ? 0x02U : 0U));
  const size_t length = buildBaudValsetPacket(
      GNSS_TARGET_BAUD, layers, packet, sizeof(packet));
  if (length == 0U) return 0U;
  const size_t written = serial_.write(packet, length);
  recordGnssTx(stats_.tx, length, written);
  ++stats_.valsetSent;
  baudValsetWriteResult_ = written;
  logBaudSwitch("valset_sent", nowMs);
#if GNSS_TX_ECHO_HEX
  char hex[49];
  formatGnssRxHex(packet, length, hex, sizeof(hex));
  log_.setPriority(LogPriority::Config);
  log_.print(F("GNSS_TX time_ms=")); log_.print(nowMs);
  log_.print(F(" baud=")); log_.print(baudRate_);
  log_.print(F(" type=BAUD_VALSET len=")); log_.print(length);
  log_.print(F(" write_result=")); log_.print(written);
  log_.print(F(" data=")); log_.println(hex);
#endif
  if (written != length) {
    lastError_ = GNSS_ERR_TX_SHORT_WRITE;
    ++stats_.failedStages;
  }
  return written;
}

void MaxM10s::beginBaudSwitch(uint32_t nowMs) {
  configurationFinished_ = true;
  baudValsetAck_ = false;
  baudValsetWriteResult_ = 0U;
#if !GNSS_CHANGE_BAUD
  baudSwitchResult_ = BaudSwitchResult::Disabled;
  baudSwitchState_ = BaudSwitchState::KeptOriginal;
  finishBaudSwitch(nowMs, BaudSwitchResult::Disabled);
#else
  if (initialBaud_ == GNSS_TARGET_BAUD) {
    baudSwitchResult_ = BaudSwitchResult::NotNeeded;
    baudSwitchState_ = BaudSwitchState::Verified;
    finishBaudSwitch(nowMs, BaudSwitchResult::NotNeeded);
    return;
  }
  baudSwitchState_ = BaudSwitchState::SendValset;
  baudSwitchResult_ = BaudSwitchResult::Pending;
  machine_ = BAUD_SWITCH_SEND;
  deadlineMs_ = nowMs;
  transition(GnssInitState::VerifyingBaud,
             "normal configuration complete; baud switch", nowMs);
#endif
}

void MaxM10s::finishBaudSwitch(uint32_t nowMs, BaudSwitchResult result) {
  baudSwitchResult_ = result;
  if (result == BaudSwitchResult::Verified ||
      result == BaudSwitchResult::NotNeeded) {
    baudSwitchState_ = BaudSwitchState::Verified;
  } else if (result == BaudSwitchResult::Disabled ||
             result == BaudSwitchResult::KeptOriginal ||
             result == BaudSwitchResult::RecoveredFallback) {
    baudSwitchState_ = BaudSwitchState::KeptOriginal;
    if (result != BaudSwitchResult::Disabled) degraded_ = true;
  }
  logBaudSwitch(baudSwitchResultName(result), nowMs);
  if (result == BaudSwitchResult::Failed) {
    baudSwitchState_ = BaudSwitchState::Failed;
    verifiedBaud_ = 0U;
    detectedBaud_ = 0U;
    machine_ = ERROR_STATE;
    transition(GnssInitState::Failed, "baud recovery exhausted", nowMs,
               GNSS_ERR_BAUD_SWITCH_LOST_COMMUNICATION);
    return;
  }
  if (sawSfrbx_) {
    machine_ = COMPLETE;
    transition(degraded_ ? GnssInitState::Degraded : GnssInitState::Ready,
               "baud phase and SFRBX complete", nowMs);
  } else {
    machine_ = SFRBX_WAIT;
    deadlineMs_ = nowMs + GNSS_SFRBX_VERIFY_TIMEOUT_MS;
    transition(GnssInitState::VerifyingSfrbx,
               degraded_ ? "baud recovered; verify SFRBX"
                         : "baud verified; verify SFRBX", nowMs);
  }
}

void MaxM10s::startNextBaudRecovery(uint32_t nowMs) {
  if (!baudRecovery_.next(recoveryBaud_)) {
    finishBaudSwitch(nowMs, BaudSwitchResult::Failed);
    return;
  }
  if (baudRecovery_.attempts() == 1U)
    baudSwitchState_ = BaudSwitchState::RecoverProbeTargetBaud;
  else if (baudRecovery_.attempts() == 2U)
    baudSwitchState_ = BaudSwitchState::RecoverProbeOriginalBaud;
  else
    baudSwitchState_ = BaudSwitchState::RecoverProbeCandidate;
  machine_ = BAUD_RECOVER_HOST;
  deadlineMs_ = nowMs;
  transition(GnssInitState::RecoveringBaud, "probe recovery baud", nowMs,
             GNSS_ERR_BAUD_VERIFY);
}

void MaxM10s::advanceConfig(uint32_t nowMs) {
  ++configIndex_;
  retryCount_ = 0;
  changeCounted_ = false;
  if (configIndex_ >= itemCount_) {
    beginBaudSwitch(nowMs);
    return;
  }
#if CONFIG_ONLY_ON_CHANGE
  if (valgetSupported_) {
    machine_ = READ_SEND;
    deadlineMs_ = nowMs;
    transition(GnssInitState::ReadingConfig, "next key", nowMs);
    return;
  }
#endif
  machine_ = APPLY_SEND;
  deadlineMs_ = nowMs;
  transition(GnssInitState::ApplyingConfig, "VALGET unavailable; fallback", nowMs);
}

void MaxM10s::handleReadFailure(uint32_t nowMs, ConfigResult result) {
  const ConfigKeySpec &item = configItem();
  printConfigDecision(item, configResultName(result), true, result);
  setEvidence(result, false, 0U);
  machine_ = APPLY_SEND;
  deadlineMs_ = nowMs;
  transition(GnssInitState::ApplyingConfig, "VALGET fallback to VALSET", nowMs);
}

void MaxM10s::retryApplyOrAdvance(uint32_t nowMs, bool nak) {
  if (nak) ++stats_.nakCount;
  else {
    ++stats_.timeouts;
  }
  if (retryCount_ < UBX_MAX_RETRIES) {
    ++retryCount_;
    machine_ = APPLY_SEND;
    deadlineMs_ = nowMs;
    return;
  }
  ++stats_.failedStages;
  const ConfigKeySpec &item = configItem();
  const bool required = isConfigFailureFatal(item.requirement);
  degraded_ = true;
  if (required) {
    lastError_ = nak ? GNSS_ERR_CFG_NAK : GNSS_ERR_CFG_TIMEOUT;
  }
  setEvidence(required ? ConfigResult::Failed : ConfigResult::Unsupported,
              false, 0U);
  printConfigDecision(item, "unknown", true,
                      required ? ConfigResult::Failed
                               : ConfigResult::Unsupported);
  advanceConfig(nowMs);
}

void MaxM10s::finishVerification(uint32_t nowMs, bool verified,
                                 ConfigResult failure, const char *current) {
  const ConfigKeySpec &item = configItem();
  if (verified) {
    setEvidence(ConfigResult::Verified, true, item.targetValue);
    printConfigDecision(item, current, false, ConfigResult::Verified);
  } else {
    ++stats_.failedStages;
    degraded_ = true;
    if (isConfigFailureFatal(item.requirement)) {
      degraded_ = true;
      lastError_ = failure == ConfigResult::Timeout ? GNSS_ERR_CFG_TIMEOUT
                                                    : GNSS_ERR_CFG_NAK;
    }
    if (configIndex_ < itemCount_ && evidence_[configIndex_].hasCurrentValue)
      evidence_[configIndex_].result = failure;
    else
      setEvidence(failure, false, 0U);
    printConfigDecision(item, current, true, failure);
  }
  advanceConfig(nowMs);
}

void MaxM10s::update(uint32_t nowMs) {
#if GNSS_RX_PROBE
  if (probeActive_ && !probeReported_ && probeDeadlineMs_ != 0U &&
      static_cast<int32_t>(nowMs - probeDeadlineMs_) >= 0)
    finishRxProbe(false);
#endif
  if (static_cast<int32_t>(nowMs - deadlineMs_) < 0) return;
  switch (machine_) {
    case DETECT_SEND:
      pollMonVer(nowMs);
#if GNSS_RX_PROBE
      probeDeadlineMs_ = nowMs + GNSS_RX_PROBE_WINDOW_MS;
#endif
      machine_ = DETECT_WAIT;
      deadlineMs_ = nowMs + UBX_MONVER_TIMEOUT_MS;
      transition(GnssInitState::PollingMonVer, "MON-VER poll", nowMs);
      break;
    case DETECT_WAIT:
      ++stats_.timeouts;
#if GNSS_RX_PROBE
      finishRxProbe(true);
#endif
      if (!advanceBaudCandidate(
              baudCandidate_, static_cast<uint8_t>(kBaudCandidateCount))) {
        machine_ = ERROR_STATE;
        transition(GnssInitState::Failed, "MON-VER exhausted all baud rates",
                   nowMs, GNSS_ERR_MON_VER_TIMEOUT);
      } else {
        setBaud(kBaudCandidates[baudCandidate_]);
#if GNSS_RX_PROBE
        rxProbe_.reset();
        memset(&baudProbe_[baudCandidate_], 0,
               sizeof(baudProbe_[baudCandidate_]));
        probeActive_ = true;
        probeReported_ = false;
        probeDeadlineMs_ = 0U;
#endif
        machine_ = DETECT_SEND;
        deadlineMs_ = nowMs + 50U;
        transition(GnssInitState::DetectingBaud, "next baud candidate", nowMs);
      }
      break;
    case READ_SEND:
      sendValget(configItem().key);
      machine_ = READ_WAIT;
      deadlineMs_ = nowMs + UBX_ACK_TIMEOUT_MS;
      break;
    case READ_WAIT:
      ++stats_.valgetTimeout;
      ++stats_.timeouts;
      handleReadFailure(nowMs, ConfigResult::Timeout);
      break;
    case APPLY_SEND:
      if (!changeCounted_) {
        ++stats_.configChanged;
        changeCounted_ = true;
      }
      sendValset(configItem());
      machine_ = APPLY_ACK_WAIT;
      deadlineMs_ = nowMs + UBX_ACK_TIMEOUT_MS;
      transition(GnssInitState::ApplyingConfig, configItem().name, nowMs);
      break;
    case APPLY_ACK_WAIT:
      retryApplyOrAdvance(nowMs, false);
      break;
    case CONFIG_POST_WAIT:
      machine_ = VERIFY_SEND;
      deadlineMs_ = nowMs;
      transition(GnssInitState::ReadingConfig, "post-restart verification", nowMs);
      break;
    case VERIFY_SEND:
      sendValget(configItem().key);
      machine_ = VERIFY_WAIT;
      deadlineMs_ = nowMs + UBX_ACK_TIMEOUT_MS;
      break;
    case VERIFY_WAIT:
      ++stats_.valgetTimeout;
      ++stats_.timeouts;
      finishVerification(nowMs, false, ConfigResult::Timeout, "timeout");
      break;
    case BAUD_SWITCH_SEND:
      ++stats_.configChanged;
      if (sendBaudValset(nowMs) == 0U) {
        baudRecovery_.begin(initialBaud_, GNSS_TARGET_BAUD);
        startNextBaudRecovery(nowMs);
        break;
      }
      baudSwitchState_ = BaudSwitchState::WaitOldBaudAck;
      machine_ = BAUD_SWITCH_QUIET_BEFORE;
      deadlineMs_ = nowMs + GNSS_BAUD_SWITCH_QUIET_BEFORE_MS;
      break;
    case BAUD_SWITCH_QUIET_BEFORE:
      baudSwitchState_ = BaudSwitchState::SwitchHostUart;
      machine_ = BAUD_SWITCH_HOST;
      deadlineMs_ = nowMs;
      break;
    case BAUD_SWITCH_HOST:
      setBaud(GNSS_TARGET_BAUD);
#if GNSS_RX_PROBE
      rxProbe_.reset();
#endif
      baudSwitchState_ = BaudSwitchState::ResetParsers;
      logBaudSwitch("host_switched", nowMs);
      machine_ = BAUD_SWITCH_QUIET_AFTER;
      deadlineMs_ = nowMs + GNSS_BAUD_SWITCH_QUIET_AFTER_MS;
      break;
    case BAUD_SWITCH_QUIET_AFTER:
      baudSwitchState_ = BaudSwitchState::PollMonVerNewBaud;
      machine_ = BAUD_SWITCH_POLL;
      deadlineMs_ = nowMs;
      break;
    case BAUD_SWITCH_POLL:
      pollMonVer(nowMs);
      baudSwitchState_ = BaudSwitchState::WaitMonVerNewBaud;
      machine_ = BAUD_SWITCH_WAIT;
      deadlineMs_ = nowMs + UBX_MONVER_TIMEOUT_MS;
      transition(GnssInitState::VerifyingBaud, "MON-VER at target baud", nowMs);
      break;
    case BAUD_SWITCH_WAIT:
      ++stats_.timeouts;
      baudRecovery_.begin(initialBaud_, GNSS_TARGET_BAUD);
      startNextBaudRecovery(nowMs);
      break;
    case BAUD_RECOVER_HOST:
      setBaud(recoveryBaud_);
#if GNSS_RX_PROBE
      rxProbe_.reset();
#endif
      logBaudSwitch("recovery_host_switched", nowMs);
      machine_ = BAUD_RECOVER_QUIET;
      deadlineMs_ = nowMs + GNSS_BAUD_SWITCH_QUIET_AFTER_MS;
      break;
    case BAUD_RECOVER_QUIET:
      machine_ = BAUD_RECOVER_POLL;
      deadlineMs_ = nowMs;
      break;
    case BAUD_RECOVER_POLL:
      pollMonVer(nowMs);
      machine_ = BAUD_RECOVER_WAIT;
      deadlineMs_ = nowMs + UBX_MONVER_TIMEOUT_MS;
      break;
    case BAUD_RECOVER_WAIT:
      ++stats_.timeouts;
      startNextBaudRecovery(nowMs);
      break;
    case SFRBX_WAIT:
      machine_ = COMPLETE;
      transition(GnssInitState::Degraded, "SFRBX verification timeout", nowMs,
                 GNSS_ERR_NO_SFRBX);
      break;
    case COMPLETE:
    case ERROR_STATE:
      break;
  }
}

void MaxM10s::printMonVer(const UbxPacketView &packet) {
  const size_t swLimit = packet.length < 30U ? packet.length : 30U;
  size_t sw = 0U;
  while (sw < swLimit && packet.payload[sw] != 0U) {
    softwareVersion_[sw] = static_cast<char>(packet.payload[sw]);
    ++sw;
  }
  softwareVersion_[sw] = '\0';
  if (packet.length >= 40U) {
    size_t hw = 0U;
    while (hw < 10U && packet.payload[30U + hw] != 0U) {
      hardwareVersion_[hw] = static_cast<char>(packet.payload[30U + hw]);
      ++hw;
    }
    hardwareVersion_[hw] = '\0';
  }
  log_.setPriority(LogPriority::Config);
  log_.print(F("MON-VER sw="));
  for (uint8_t i = 0; i < 30U && i < packet.length && packet.payload[i]; ++i)
    log_.write(packet.payload[i]);
  if (packet.length >= 40U) {
    log_.print(F(" hw="));
    for (uint8_t i = 30U; i < 40U && packet.payload[i]; ++i)
      log_.write(packet.payload[i]);
  }
  log_.println();
#if DEBUG_LEVEL >= 3
  for (uint16_t offset = 40U; offset + 30U <= packet.length; offset += 30U) {
    log_.setPriority(LogPriority::Config);
    log_.print(F("MON-VER extension="));
    for (uint8_t i = 0U; i < 30U && packet.payload[offset + i] != 0U; ++i)
      log_.write(packet.payload[offset + i]);
    log_.println();
  }
#endif
}

void MaxM10s::handlePacket(const UbxPacketView &packet, uint32_t nowMs) {
#if GNSS_RX_PROBE
  if (probeActive_) rxProbe_.noteValidUbx();
#endif
  if (machine_ == DETECT_WAIT && detectedBaud_ == 0U) detectedBaud_ = baudRate_;
  if (packet.messageClass == CLASS_MON && packet.messageId == ID_MON_VER) {
    monVerReceived_ = true;
    printMonVer(packet);
    if (machine_ == DETECT_WAIT) {
      initialBaud_ = baudRate_;
      verifiedBaud_ = baudRate_;
      detectedBaud_ = baudRate_;
#if GNSS_RX_PROBE
      if (baudCandidate_ < kBaudCandidateCount)
        baudProbe_[baudCandidate_].monVerResponse = true;
      finishRxProbe(false);
      log_.print(F("GNSS_BAUD_RESPONSE baud=")); log_.print(baudRate_);
      log_.println(F(" type=MON_VER valid=yes"));
#endif
      log_.print(F("MAX-M10S detected baud=")); log_.println(baudRate_);
      beginConfiguration(nowMs);
    } else if (machine_ == BAUD_SWITCH_WAIT) {
      verifiedBaud_ = baudRate_;
      detectedBaud_ = baudRate_;
      finishBaudSwitch(nowMs, BaudSwitchResult::Verified);
    } else if (machine_ == BAUD_RECOVER_WAIT) {
      verifiedBaud_ = baudRate_;
      detectedBaud_ = baudRate_;
      if (baudRate_ == GNSS_TARGET_BAUD)
        finishBaudSwitch(nowMs, BaudSwitchResult::Verified);
      else if (baudRate_ == initialBaud_)
        finishBaudSwitch(nowMs, BaudSwitchResult::KeptOriginal);
      else
        finishBaudSwitch(nowMs, BaudSwitchResult::RecoveredFallback);
    }
    return;
  }

  if (packet.messageClass == CLASS_CFG && packet.messageId == ID_CFG_VALGET &&
      (machine_ == READ_WAIT || machine_ == VERIFY_WAIT)) {
    uint32_t current = 0U;
    if (!parseCfgValgetResponse(packet.payload, packet.length,
                                configItem().key, configItem().width, current)) {
      return;
    }
    ++stats_.valgetResponse;
    char currentText[11];
    ultoa(current, currentText, 10);
    if (machine_ == VERIFY_WAIT) {
      if (current != configItem().targetValue)
        setEvidence(ConfigResult::Failed, true, current);
      finishVerification(nowMs, current == configItem().targetValue,
                         ConfigResult::Failed, currentText);
      return;
    }
    const ConfigAction action = decideConfigAction(
        true, current, configItem().targetValue, CONFIG_ONLY_ON_CHANGE != 0);
    if (action == ConfigAction::Skip) {
      ++stats_.configSkipped;
      setEvidence(ConfigResult::Verified, true, current);
      printConfigDecision(configItem(), currentText, false,
                          ConfigResult::Verified);
      advanceConfig(nowMs);
    } else {
      ++stats_.configChanged;
      changeCounted_ = true;
      setEvidence(ConfigResult::Changed, true, current);
      printConfigDecision(configItem(), currentText, true, ConfigResult::Changed);
      machine_ = APPLY_SEND;
      deadlineMs_ = nowMs;
      transition(GnssInitState::ApplyingConfig, "value differs", nowMs);
    }
    return;
  }

  if (packet.messageClass != CLASS_ACK || packet.length != 2U) return;
  const bool ack = packet.messageId == ID_ACK_ACK;
  const bool nak = packet.messageId == ID_ACK_NAK;
  if ((!ack && !nak) || packet.payload[0] != CLASS_CFG) return;

  if (packet.payload[1] == ID_CFG_VALSET &&
      (machine_ == BAUD_SWITCH_QUIET_BEFORE ||
       machine_ == BAUD_SWITCH_HOST ||
       machine_ == BAUD_SWITCH_QUIET_AFTER)) {
    if (ack) {
      ++stats_.ackCount;
      baudValsetAck_ = true;
      logBaudSwitch("old_baud_ack", nowMs);
    } else {
      ++stats_.nakCount;
      degraded_ = true;
      baudSwitchState_ = BaudSwitchState::KeptOriginal;
      finishBaudSwitch(nowMs, BaudSwitchResult::KeptOriginal);
    }
    return;
  }

  if (packet.payload[1] == ID_CFG_VALGET && nak &&
      (machine_ == READ_WAIT || machine_ == VERIFY_WAIT)) {
    ++stats_.nakCount;
    if (machine_ == VERIFY_WAIT)
      finishVerification(nowMs, false, ConfigResult::Unsupported,
                         "unsupported");
    else
      handleReadFailure(nowMs, ConfigResult::Unsupported);
    return;
  }
  if (packet.payload[1] != ID_CFG_VALSET || machine_ != APPLY_ACK_WAIT) return;

  if (nak) {
    log_.print(F("CFG VALSET NAK key=0x")); log_.print(configItem().key, HEX);
    log_.print(F(" name=")); log_.print(configItem().name);
    log_.print(F(" retry=")); log_.println(retryCount_);
    retryApplyOrAdvance(nowMs, true);
    return;
  }
  ++stats_.ackCount;
  setEvidence(ConfigResult::Changed,
              evidence_[configIndex_].hasCurrentValue,
              evidence_[configIndex_].currentValue, true);
  log_.print(F("CFG VALSET ACK key=0x")); log_.print(configItem().key, HEX);
  log_.print(F(" name=")); log_.println(configItem().name);

  if (configItem().restartDelay) {
    machine_ = CONFIG_POST_WAIT;
    deadlineMs_ = nowMs + GNSS_SIGNAL_RESTART_WAIT_MS;
    transition(GnssInitState::WaitingForRestart, configItem().name, nowMs);
  } else if (valgetSupported_) {
    machine_ = VERIFY_SEND;
    deadlineMs_ = nowMs;
    transition(GnssInitState::ReadingConfig, "VALSET ACK; verify", nowMs);
  } else {
    // VALGET のない旧ファームウェアでは ACK が得られる最も強い証拠。
    printConfigDecision(configItem(), "unreadable", false,
                        ConfigResult::Changed);
    setEvidence(ConfigResult::Changed, false, 0U, true);
    advanceConfig(nowMs);
  }
}

void MaxM10s::observeRxByte(uint8_t byte) {
#if GNSS_RX_PROBE
  if (probeActive_) rxProbe_.feed(byte);
#else
  (void)byte;
#endif
}

bool MaxM10s::consumeParserResetRequest() {
  const bool requested = parserResetRequested_;
  parserResetRequested_ = false;
  return requested;
}

GnssRxClassification MaxM10s::bestRxClassification() const {
#if GNSS_RX_PROBE
  GnssRxClassification best = GnssRxClassification::NoData;
  for (size_t i = 0U; i < kBaudCandidateCount; ++i) {
    const GnssRxClassification value = baudProbe_[i].probe.classification;
    if (value == GnssRxClassification::ValidUbx) return value;
    if (value == GnssRxClassification::LikelyNmea) best = value;
    else if (best == GnssRxClassification::NoData &&
             value != GnssRxClassification::NoData) best = value;
  }
  return best;
#else
  return GnssRxClassification::NoData;
#endif
}

void MaxM10s::finishRxProbe(bool timeout) {
#if GNSS_RX_PROBE
  if (baudCandidate_ >= kBaudCandidateCount) return;
  BaudProbeRecord &record = baudProbe_[baudCandidate_];
  if (!probeActive_) {
    if (timeout && !record.timeout) {
      record.timeout = true;
      log_.print(F("GNSS_BAUD_TIMEOUT baud=")); log_.print(record.baud);
      log_.print(F(" mon_ver_response="));
      log_.println(record.monVerResponse ? 1 : 0);
    }
    return;
  }
  record.baud = baudRate_;
  record.probe = rxProbe_.finish();
  if (timeout) record.timeout = true;
  probeActive_ = false;
  if (nmeaInferredBaud(record.probe) && nmeaInferredBaud_ == 0U)
    nmeaInferredBaud_ = baudRate_;
  printRxProbe(record);
  probeReported_ = true;
#else
  (void)timeout;
#endif
}

void MaxM10s::printRxProbe(const BaudProbeRecord &record) {
#if GNSS_RX_PROBE
  const GnssRxProbeStats &s = record.probe.stats;
  log_.setPriority(LogPriority::Verbose);
  log_.print(F("GNSS_RX_PROBE baud=")); log_.print(record.baud);
  log_.print(F(" bytes=")); log_.print(s.receivedBytes);
  log_.print(F(" printable=")); log_.print(s.printableBytes);
  log_.print(F(" dollar=")); log_.print(s.dollarBytes);
  log_.print(F(" crlf=")); log_.print(s.crlfPairs);
  log_.print(F(" nmea_lines=")); log_.print(s.nmea.candidateLines);
  log_.print(F(" nmea_valid=")); log_.print(s.nmea.validChecksum);
  log_.print(F(" nmea_invalid=")); log_.print(s.nmea.invalidChecksum);
  log_.print(F(" nmea_incomplete=")); log_.print(s.nmea.incompleteLines);
  log_.print(F(" ubx_sync=")); log_.print(s.ubxSyncPairs);
  log_.print(F(" ubx_valid=")); log_.print(s.validUbxPackets);
  log_.print(F(" zeros=")); log_.print(s.zeroBytes);
  log_.print(F(" ff=")); log_.print(s.ffBytes);
  log_.print(F(" classification="));
  log_.print(gnssRxClassificationName(record.probe.classification));
  log_.print(F(" evidence=")); log_.println(gnssRxEvidenceName(record.probe.evidence));
  for (uint8_t i = 0U; i < s.nmea.sentenceIdCount; ++i) {
    log_.setPriority(LogPriority::Verbose);
    log_.print(F("GNSS_NMEA_ID baud=")); log_.print(record.baud);
    log_.print(F(" id=")); log_.println(s.nmea.sentenceIds[i]);
  }
  const uint8_t *sample = rxProbe_.sample();
  const size_t sampleLength = rxProbe_.sampleLength();
  for (size_t offset = 0U; offset < sampleLength; offset += 32U) {
    const size_t count = sampleLength - offset > 32U ? 32U : sampleLength - offset;
    char ascii[65];
    char hex[65];
    formatGnssRxAscii(sample + offset, count, ascii, sizeof(ascii));
    formatGnssRxHex(sample + offset, count, hex, sizeof(hex));
    log_.setPriority(LogPriority::Verbose);
    log_.print(F("GNSS_RX_ASCII baud=")); log_.print(record.baud);
    log_.print(F(" offset=")); log_.print(offset); log_.print(F(" data=\""));
    log_.print(ascii); log_.println('"');
    log_.setPriority(LogPriority::Verbose);
    log_.print(F("GNSS_RX_HEX baud=")); log_.print(record.baud);
    log_.print(F(" offset=")); log_.print(offset); log_.print(F(" data="));
    log_.println(hex);
  }
  log_.setPriority(LogPriority::Verbose);
  log_.print(F("GNSS_BAUD_RESULT baud=")); log_.print(record.baud);
  log_.print(F(" rx=")); log_.print(s.receivedBytes);
  log_.print(F(" ubx=")); log_.print(s.validUbxPackets);
  log_.print(F(" nmea=")); log_.print(s.nmea.validChecksum);
  log_.print(F(" mon_ver_sent=")); log_.print(record.monVerSent ? 1 : 0);
  log_.print(F(" write_result=")); log_.print(record.monVerWriteResult);
  log_.print(F(" mon_ver_response=")); log_.print(record.monVerResponse ? 1 : 0);
  log_.print(F(" timeout=")); log_.println(record.timeout ? 1 : 0);
#else
  (void)record;
#endif
}

void MaxM10s::noteSfrbx(uint32_t nowMs) {
  sawSfrbx_ = true;
  if (machine_ == SFRBX_WAIT) {
    machine_ = COMPLETE;
    transition(degraded_ ? GnssInitState::Degraded : GnssInitState::Ready,
               "valid SFRBX observed", nowMs);
  }
}
