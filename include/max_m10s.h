#pragma once

#include <Arduino.h>

#include "config.h"
#include "baud_switch.h"
#include "gnss_config.h"
#include "gnss_rx_probe.h"
#include "gnss_uart_diag.h"
#include "log_arbiter.h"
#include "ubx_parser.h"

enum class GnssInitState : uint8_t {
  Idle,
  DetectingBaud,
  PollingMonVer,
  ReadingConfig,
  ApplyingConfig,
  WaitingForRestart,
  VerifyingBaud,
  RecoveringBaud,
  VerifyingSfrbx,
  Ready,
  Degraded,
  Failed
};

enum GnssErrorCode : uint8_t {
  GNSS_ERR_NONE = 0,
  GNSS_ERR_NO_RESPONSE,
  GNSS_ERR_NO_VALID_UBX,
  GNSS_ERR_MON_VER_TIMEOUT,
  GNSS_ERR_CFG_NAK,
  GNSS_ERR_CFG_TIMEOUT,
  GNSS_ERR_BAUD_VERIFY,
  GNSS_ERR_NO_SFRBX,
  GNSS_ERR_NO_QZSS,
  GNSS_ERR_NO_L1S,
  GNSS_ERR_INVALID_L1S_PAB,
  GNSS_ERR_INVALID_L1S_CRC,
  GNSS_ERR_UNSUPPORTED_MT
  , GNSS_ERR_NO_RX_DATA
  , GNSS_ERR_RX_DATA_NO_VALID_PROTOCOL
  , GNSS_ERR_LIKELY_WRONG_BAUD
  , GNSS_ERR_NMEA_ONLY
  , GNSS_ERR_TX_SHORT_WRITE
  , GNSS_ERR_RX_OK_TX_NO_RESPONSE
  , GNSS_ERR_PROTOCOL_INPUT_DISABLED
  , GNSS_ERR_BAUD_SWITCH_LOST_COMMUNICATION
};

const char *gnssInitStateName(GnssInitState state);
const char *gnssErrorName(GnssErrorCode error);

class MaxM10s {
 public:
  enum Status : uint8_t { DETECTING, CONFIGURING, READY, FAILED };

  struct Stats {
    uint32_t valgetSent;
    uint32_t valgetResponse;
    uint32_t valgetTimeout;
    uint32_t valsetSent;
    uint32_t ackCount;
    uint32_t nakCount;
    uint32_t configSkipped;
    uint32_t configChanged;
    uint32_t timeouts;
    uint32_t failedStages;
    GnssTxStats tx;
  };

  struct BaudProbeRecord {
    uint32_t baud;
    GnssRxProbeResult probe;
    uint16_t monVerWriteResult;
    bool monVerSent;
    bool monVerResponse;
    bool timeout;
  };

  MaxM10s(HardwareSerial &serial, LogArbiter &log);
  void begin(uint32_t nowMs);
  void update(uint32_t nowMs);
  void handlePacket(const UbxPacketView &packet, uint32_t nowMs);
  void observeRxByte(uint8_t byte);
  void noteSfrbx(uint32_t nowMs);

  Status status() const { return status_; }
  GnssInitState initState() const { return initState_; }
  GnssErrorCode lastError() const { return lastError_; }
  uint32_t baudRate() const { return baudRate_; }
  uint32_t lastAttemptedBaud() const { return baudRate_; }
  bool hasDetectedBaud() const { return detectedBaud_ != 0U; }
  uint32_t detectedBaud() const { return detectedBaud_; }
  uint32_t initialBaud() const { return initialBaud_; }
  uint32_t verifiedBaud() const { return verifiedBaud_; }
  uint32_t targetBaud() const { return GNSS_TARGET_BAUD; }
  BaudSwitchState baudSwitchState() const { return baudSwitchState_; }
  BaudSwitchResult baudSwitchResult() const { return baudSwitchResult_; }
  bool hasNmeaInferredBaud() const { return nmeaInferredBaud_ != 0U; }
  uint32_t nmeaInferredBaudValue() const { return nmeaInferredBaud_; }
  GnssRxClassification bestRxClassification() const;
  bool consumeParserResetRequest();
  const Stats &stats() const { return stats_; }
  const char *stageName() const;
  bool monVerReceived() const { return monVerReceived_; }
  const char *softwareVersion() const { return softwareVersion_; }
  const char *hardwareVersion() const { return hardwareVersion_; }
  bool configurationFinished() const { return configurationFinished_; }
  const ConfigEvidence *configEvidence(uint32_t key) const;
  bool timePulseConfigAllVerified() const;
  bool timePulseConfigAnyFailure() const;

  void pollMonVer(uint32_t nowMs = 0U);

  static const uint8_t CLASS_ACK = 0x05;
  static const uint8_t ID_ACK_NAK = 0x00;
  static const uint8_t ID_ACK_ACK = 0x01;
  static const uint8_t CLASS_CFG = 0x06;
  static const uint8_t ID_CFG_VALSET = 0x8A;
  static const uint8_t ID_CFG_VALGET = 0x8B;
  static const uint8_t CLASS_MON = 0x0A;
  static const uint8_t ID_MON_VER = 0x04;
  static const uint8_t CLASS_RXM = 0x02;
  static const uint8_t ID_RXM_SFRBX = 0x13;
  static const uint8_t CLASS_NAV = 0x01;
  static const uint8_t ID_NAV_PVT = 0x07;

 private:
  enum MachineState : uint8_t {
    DETECT_SEND,
    DETECT_WAIT,
    READ_SEND,
    READ_WAIT,
    APPLY_SEND,
    APPLY_ACK_WAIT,
    CONFIG_POST_WAIT,
    VERIFY_SEND,
    VERIFY_WAIT,
    BAUD_SWITCH_SEND,
    BAUD_SWITCH_QUIET_BEFORE,
    BAUD_SWITCH_HOST,
    BAUD_SWITCH_QUIET_AFTER,
    BAUD_SWITCH_POLL,
    BAUD_SWITCH_WAIT,
    BAUD_RECOVER_HOST,
    BAUD_RECOVER_QUIET,
    BAUD_RECOVER_POLL,
    BAUD_RECOVER_WAIT,
    SFRBX_WAIT,
    COMPLETE,
    ERROR_STATE
  } machine_;

  void transition(GnssInitState next, const char *reason, uint32_t nowMs,
                  GnssErrorCode error = GNSS_ERR_NONE);
  void setBaud(uint32_t baud);
  size_t sendPacket(uint8_t cls, uint8_t id,
                    const uint8_t *payload, uint16_t length,
                    uint32_t nowMs = 0U);
  void finishRxProbe(bool timeout);
  void printRxProbe(const BaudProbeRecord &record);
  void sendValget(uint32_t key);
  void sendValset(const ConfigKeySpec &item);
  void beginConfiguration(uint32_t nowMs);
  void advanceConfig(uint32_t nowMs);
  void handleReadFailure(uint32_t nowMs, ConfigResult result);
  void retryApplyOrAdvance(uint32_t nowMs, bool nak);
  void finishVerification(uint32_t nowMs, bool verified,
                          ConfigResult failure, const char *current);
  void beginBaudSwitch(uint32_t nowMs);
  void finishBaudSwitch(uint32_t nowMs, BaudSwitchResult result);
  void startNextBaudRecovery(uint32_t nowMs);
  void logBaudSwitch(const char *stage, uint32_t nowMs);
  size_t sendBaudValset(uint32_t nowMs);
  void printMonVer(const UbxPacketView &packet);
  void printConfigDecision(const ConfigKeySpec &item, const char *current,
                           bool change, ConfigResult result);
  void setEvidence(ConfigResult result, bool hasCurrent, uint32_t current,
                   bool valsetAcknowledged = false);
  const ConfigKeySpec &configItem() const;

  HardwareSerial &serial_;
  LogArbiter &log_;
  Status status_;
  GnssInitState initState_;
  GnssErrorCode lastError_;
  Stats stats_;
  const ConfigKeySpec *items_;
  size_t itemCount_;
  uint32_t baudRate_;
  uint32_t deadlineMs_;
  uint8_t baudCandidate_;
  uint8_t retryCount_;
  size_t configIndex_;
  bool valgetSupported_;
  bool sawSfrbx_;
  bool degraded_;
  bool changeCounted_;
  bool serialStarted_;
  bool monVerReceived_;
  bool configurationFinished_;
  bool parserResetRequested_;
  uint32_t detectedBaud_;
  uint32_t initialBaud_;
  uint32_t verifiedBaud_;
  uint32_t recoveryBaud_;
  uint32_t nmeaInferredBaud_;
  BaudSwitchState baudSwitchState_;
  BaudSwitchResult baudSwitchResult_;
  BaudRecoverySequence baudRecovery_;
  bool baudValsetAck_;
  size_t baudValsetWriteResult_;
#if GNSS_RX_PROBE
  GnssRxProbe rxProbe_;
  BaudProbeRecord baudProbe_[3];
  uint32_t probeDeadlineMs_;
  bool probeActive_;
  bool probeReported_;
#endif
  static const size_t MAX_CONFIG_EVIDENCE = 24U;
  ConfigEvidence evidence_[MAX_CONFIG_EVIDENCE];
  char softwareVersion_[31];
  char hardwareVersion_[11];
};
