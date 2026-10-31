#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "bridge_protocol.h"
#include "bridge_transport.h"
#include "dcr_dcx_decoder.h"
#include "gnss_self_test.h"
#include "max_m10s.h"
#include "log_arbiter.h"
#include "output_formatter.h"
#include "pps_capture.h"
#include "qzss_l1s.h"
#include "raw_capture.h"
#include "safe_format.h"
#include "startup_metadata.h"
#include "startup_sequence.h"
#include "time_sync.h"
#include "tim_tp.h"
#include "ubx_dispatcher.h"
#include "ubx_parser.h"
#if HOSHIGUARD_OLED
#include "oled_stm32.h"
#include "status_display.h"
#endif

// STM32duino HardwareSerial のコンストラクター引数は (RX pin, TX pin)。
// この基板では Serial2 が PA3/PA2 に生成され、USART1 は明示的に作成する。
HardwareSerial DebugUart(DEBUG_RX_PIN, DEBUG_TX_PIN);
LogArbiter DebugSerial(DebugUart);

namespace {
UbxParser ubx;
MaxM10s gnss(Serial2, DebugSerial);
PpsCapture pps;
PpsUtcAssociator timeSync;
UbxFrameDispatcher dispatcher;
RawCaptureLogger rawCapture(DebugSerial);
#if BRIDGE_BINARY_MODE
BridgeTransport bridgeTransport(DebugUart);
#endif
uint32_t bridgeTextSequence = 0U;
PpsSnapshot ppsState = {};
GnssUtcTime utc = {};
bool selfTestPrinted = false;
bool startUtcPrinted = false;
#if HOSHIGUARD_OLED
oled::Ssd1306 oledScreen(stm32OledBus());
oled::StatusDisplay oledStatus;

void updateOled() {
  const uint32_t nowMs = millis();
  if (oledStatus.due(nowMs)) {
    oled::StatusInput input = {};
    input.gnss = oled::GnssState::Initializing;
    if (gnss.initState() == GnssInitState::Ready) input.gnss = oled::GnssState::Ready;
    else if (gnss.initState() == GnssInitState::Degraded) input.gnss = oled::GnssState::Degraded;
    else if (gnss.initState() == GnssInitState::Failed) input.gnss = oled::GnssState::Failed;
    // SFRBX 未観測は通常の衛星待ちと区別できないため、受信故障とは表示しない。
    input.initError = gnss.lastError() == GNSS_ERR_NO_SFRBX ? 0U :
                         static_cast<uint8_t>(gnss.lastError());
    input.timeQuality = timeSync.quality();
    input.ppsAbnormal = ppsState.recentBad != 0U;
    input.ppsStable = ppsState.consecutiveGood >= 3U;
    input.ubxErrors = ubx.stats().checksumErrors;
    const SatelliteFrameStats &satellites = dispatcher.stats();
    input.l1sErrors = satellites.invalidPab + satellites.invalidCrc;
#if BRIDGE_BINARY_MODE
    input.bridgeBinary = true;
    input.bridgeSent = bridgeTransport.stats().framesSent;
    input.bridgeDropped = bridgeTransport.stats().framesDropped;
    input.bridgeL1sDropped = bridgeTransport.stats().l1sDropped;
#endif
    oled::Screen screen = {};
    oledStatus.render(nowMs, input, screen, HOSHIGUARD_VERSION);
    oledScreen.setScreen(screen);
  }
  // PPS、USART2 受信、Bridge の pump 後にのみ実行する。受信待ちがあれば新規転送を延期する。
  oledScreen.tick(nowMs, micros(), Serial2.available() == 0);
}
#endif

#if GNSS_UART_LOOPBACK_TEST
uint8_t loopbackExpected[12] = {};
uint8_t loopbackReceived[12] = {};
size_t loopbackExpectedLength = 0U;
size_t loopbackReceivedLength = 0U;
uint32_t loopbackDeadlineMs = 0U;
bool loopbackFinished = false;

void finishLoopback(bool timeout) {
  if (loopbackFinished) return;
  loopbackFinished = true;
  char expectedHex[25];
  char receivedHex[25];
  formatGnssRxHex(loopbackExpected, loopbackExpectedLength,
                  expectedHex, sizeof(expectedHex));
  formatGnssRxHex(loopbackReceived, loopbackReceivedLength,
                  receivedHex, sizeof(receivedHex));
  const bool pass = !timeout &&
      loopbackReceivedLength == loopbackExpectedLength &&
      memcmp(loopbackExpected, loopbackReceived, loopbackExpectedLength) == 0;
  DebugSerial.print(F("LOOPBACK ")); DebugSerial.println(pass ? F("PASS") : F("FAIL"));
  DebugSerial.print(F("expected=")); DebugSerial.println(expectedHex);
  DebugSerial.print(F("received=")); DebugSerial.println(receivedHex);
  DebugSerial.print(F("timeout=")); DebugSerial.println(timeout ? 1 : 0);
}

void updateLoopback(uint32_t nowMs) {
  if (loopbackFinished) return;
  while (Serial2.available() > 0 &&
         loopbackReceivedLength < sizeof(loopbackReceived)) {
    const int byte = Serial2.read();
    if (byte < 0) break;
    loopbackReceived[loopbackReceivedLength++] = static_cast<uint8_t>(byte);
  }
  if (loopbackReceivedLength == loopbackExpectedLength) finishLoopback(false);
  else if (static_cast<int32_t>(nowMs - loopbackDeadlineMs) >= 0)
    finishLoopback(true);
}
#endif

uint64_t extendedMicros() {
  static uint32_t previous = 0;
  static uint64_t high = 0;
  const uint32_t current = micros();
  if (current < previous) high += (1ULL << 32);
  previous = current;
  return high | current;
}

void handleNavPvt(const UbxPacketView &packet, uint32_t receivedMicros) {
  NavPvtRecord record = {};
  if (!timeSync.observeNavPvt(packet.payload, packet.length,
                              receivedMicros, record)) return;
#if HOSHIGUARD_OLED
  if (record.utcValid) oledStatus.noteNav(millis());
#endif
  utc.year = record.year;
  utc.month = record.month;
  utc.day = record.day;
  utc.hour = record.hour;
  utc.minute = record.minute;
  utc.second = record.second;
  utc.iTowMs = record.iTowMs;
  utc.nano = record.nano;
  utc.receivedMicros = record.receivedMicros;
  utc.valid = record.utcValid;
  utc.associatedWithPps = record.ppsAssociated;
  utc.ppsSequence = record.nearestPpsSequence;
  utc.quality = record.quality;
  if (record.utcValid && !startUtcPrinted) {
    startUtcPrinted = true;
    DebugSerial.print(F("META test_start_utc="));
    char value[24];
    const SafeFormatResult formatted = safeSnprintf(
        value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        record.year, record.month, record.day, record.hour, record.minute,
        record.second);
    safePrintln(DebugSerial, value, formatted);
  }
#if OUTPUT_NAV_PVT_COORDINATES && !CAPTURE_PRIVACY_MODE
  if (packet.length == 92U) {
    const int32_t lon = static_cast<int32_t>(
        static_cast<uint32_t>(packet.payload[24]) |
        (static_cast<uint32_t>(packet.payload[25]) << 8) |
        (static_cast<uint32_t>(packet.payload[26]) << 16) |
        (static_cast<uint32_t>(packet.payload[27]) << 24));
    const int32_t lat = static_cast<int32_t>(
        static_cast<uint32_t>(packet.payload[28]) |
        (static_cast<uint32_t>(packet.payload[29]) << 8) |
        (static_cast<uint32_t>(packet.payload[30]) << 16) |
        (static_cast<uint32_t>(packet.payload[31]) << 24));
    DebugSerial.print(F("NAV_COORD lon_1e7=")); DebugSerial.print(lon);
    DebugSerial.print(F(" lat_1e7=")); DebugSerial.println(lat);
  }
#endif
}

void printSfrbxHeader(const SfrbxHeader &h) {
#if DEBUG_LEVEL >= 2
  DebugSerial.print(F("SFRBX sv=")); DebugSerial.print(h.svId);
  DebugSerial.print(F(" gnssId=")); DebugSerial.print(h.gnssId);
  DebugSerial.print(F(" sigId=")); DebugSerial.print(h.sigId);
  DebugSerial.print(F(" numWords=")); DebugSerial.print(h.numWords);
  DebugSerial.print(F(" freqId=")); DebugSerial.print(h.freqId);
  DebugSerial.print(F(" version=")); DebugSerial.println(h.version);
#else
  (void)h;
#endif
}

void handleSfrbx(const UbxPacketView &packet, const UbxDispatchEvent &event,
                  uint64_t receivedMicros) {
  if (!event.hasHeader) {
#if DEBUG_LEVEL >= 1
    DebugSerial.println(F("SFRBX rejected: inconsistent header/length"));
#endif
    return;
  }
  printSfrbxHeader(event.header);
#if L1S_COMPACT_CAPTURE
  if (event.hasL1s) {
    rawCapture.enqueueQzssL1sUbx(receivedMicros, event.frame.ppsSequence,
                                 event.header, event.frame, packet);
  }
#else
  rawCapture.enqueueSfrbxHeader(receivedMicros, event.header);
  rawCapture.enqueueSfrbx(receivedMicros, event.header, packet.payload,
                          packet.length);
  if (!event.hasL1s) return;
  rawCapture.enqueueL1s(event.frame);
#endif
  if (!event.hasL1s) return;
  const QzssL1sFrame &frame = event.frame;
#if HOSHIGUARD_OLED
  oledStatus.noteL1s(millis(), frame.messageType, frame.validPab && frame.validCrc);
#endif

#if BRIDGE_BINARY_MODE || BRIDGE_TEXT_MODE
  uint8_t fullUbx[bridge::kMaximumUbxLength];
  const size_t fullUbxLength = UbxParser::buildPacket(
      packet.messageClass, packet.messageId, packet.payload, packet.length,
      fullUbx, sizeof(fullUbx));
  if (fullUbxLength != 0U) {
    const PpsActivity ppsActivity = evaluatePpsActivity(
        pps.healthStats(), ppsState.edgeMicros, micros());
    const TimTpObservation &timing = timeSync.lastTimTp();
    uint16_t flags = bridge::L1sFlagUbxChecksumValid |
                     bridge::L1sFlagLengthValid;
    if (frame.validPab) flags |= bridge::L1sFlagPabValid;
    if (frame.validCrc) flags |= bridge::L1sFlagCrcValid;
    if (frame.duplicate) flags |= bridge::L1sFlagDuplicateSatelliteCopy;
    if (timing.matched && timing.matchedPpsSequence == frame.ppsSequence)
      flags |= bridge::L1sFlagPpsMatched;
    if (utc.valid) flags |= bridge::L1sFlagUtcAvailable;
    if (timing.message.tpLocked) flags |= bridge::L1sFlagTpLocked;
    if (timeSync.quality() == TimeQuality::LockedUtcLimited)
      flags |= bridge::L1sFlagTimeLimited;
    if (timeSync.quality() == TimeQuality::UtcPpsConfirmed)
      flags |= bridge::L1sFlagTimeConfirmed;
#if BRIDGE_BINARY_MODE
    if (bridgeTransport.stats().framesDropped != 0U)
      flags |= bridge::L1sFlagQueuePressure;
    const BridgeL1sMetadata metadata = {
        frame.ppsSequence, frame.svId, event.header.gnssId,
        event.header.sigId, event.header.numWords, frame.pab,
        frame.messageType, flags, static_cast<uint8_t>(timeSync.quality()),
        static_cast<uint8_t>(ppsActivity)};
    if (frame.validPab && frame.validCrc) {
      (void)bridgeTransport.enqueueL1s(receivedMicros, metadata, fullUbx,
                                       static_cast<uint16_t>(fullUbxLength));
    } else {
      const uint16_t code = frame.validPab ? GNSS_ERR_INVALID_L1S_CRC
                                           : GNSS_ERR_INVALID_L1S_PAB;
      (void)bridgeTransport.enqueueError(
          receivedMicros, code,
          static_cast<uint32_t>(frame.pab),
          static_cast<uint32_t>(frame.messageType));
    }
#else
    static char line[700];
    char timestampValue[21];
    (void)formatUint64Decimal(receivedMicros, timestampValue,
                              sizeof(timestampValue));
    const SafeFormatResult prefix = safeSnprintf(
        line, sizeof(line),
        "BRIDGE_L1S seq=%lu timestamp_us=%s pps_seq=%lu svId=%u "
        "gnssId=%u sigId=%u numWords=%u pab=%02X mt=%u "
        "ubx_checksum=ok l1s_crc=%s flags=%04X len=%u data=",
        static_cast<unsigned long>(bridgeTextSequence++),
        timestampValue,
        static_cast<unsigned long>(frame.ppsSequence), frame.svId,
        event.header.gnssId, event.header.sigId, event.header.numWords,
        frame.pab, frame.messageType, frame.validCrc ? "ok" : "bad", flags,
        static_cast<unsigned>(fullUbxLength));
    size_t length = prefix.length;
    if (prefix.status == SafeFormatStatus::Complete &&
        length + fullUbxLength * 2U + 1U <= sizeof(line)) {
      static const char hex[] = "0123456789ABCDEF";
      for (size_t i = 0U; i < fullUbxLength; ++i) {
        line[length++] = hex[fullUbx[i] >> 4U];
        line[length++] = hex[fullUbx[i] & 0x0FU];
      }
      line[length++] = '\n';
      (void)DebugSerial.enqueueRecord(LogPriority::QzssL1s, line, length);
    }
#endif
  }
#endif

#if !RAW_CAPTURE_MODE && !BRIDGE_BINARY_MODE && !BRIDGE_TEXT_MODE
#if JSON_OUTPUT
  printL1sFrame(DebugSerial, frame, utc);
#else
  if (frame.duplicate) {
    DebugSerial.print(F("L1S duplicate sv=")); DebugSerial.print(frame.svId);
    DebugSerial.print(F(" MT=")); DebugSerial.print(frame.messageType);
    DebugSerial.print(F(" CRC="));
    DebugSerial.println(frame.validCrc ? F("ok") : F("bad"));
  } else {
    printL1sFrame(DebugSerial, frame, utc);
  }
#endif

  if (!frame.validPab || !frame.validCrc || frame.duplicate) return;
  if (frame.messageType == 43U) {
    DcrDecodeResult result;
    if (decodeDcrMt43(frame, result)) printDcrResult(DebugSerial, result);
  } else if (frame.messageType == 44U) {
    DcxDecodeResult result;
    if (decodeDcxMt44(frame, result)) printDcxResult(DebugSerial, result);
  } else {
#if DEBUG_LEVEL >= 1
    DebugSerial.print(F("unsupported MT="));
    DebugSerial.println(frame.messageType);
#endif
  }
#endif
}

void handlePacket(const UbxPacketView &packet, uint32_t nowMs) {
#if HOSHIGUARD_OLED
  oledStatus.noteUbx(millis());
#endif
  gnss.handlePacket(packet, nowMs);
  const uint64_t receivedUs = extendedMicros();
  rawCapture.enqueueUbx(receivedUs, packet);
  UbxDispatchEvent event = {};
  if (dispatcher.processPacket(packet, receivedUs, ppsState.sequence, event)) {
    if (event.hasHeader) gnss.noteSfrbx(nowMs);
    handleSfrbx(packet, event, receivedUs);
  } else if (packet.messageClass == MaxM10s::CLASS_NAV &&
             packet.messageId == MaxM10s::ID_NAV_PVT) {
    handleNavPvt(packet, static_cast<uint32_t>(receivedUs));
  } else if (packet.messageClass == UBX_CLASS_TIM &&
             packet.messageId == UBX_ID_TIM_TP) {
    UbxTimTp timing = {};
    if (parseUbxTimTp(packet.payload, packet.length, timing)) {
#if HOSHIGUARD_OLED
      oledStatus.noteTimTp(millis());
#endif
      const PpsEvent *previous = timeSync.newestPps();
      const uint32_t previousSequence = previous == 0 ? 0U : previous->sequence;
      timeSync.observeTimTp(timing, static_cast<uint32_t>(receivedUs));
      rawCapture.enqueueTimTp(static_cast<uint32_t>(receivedUs),
                              previousSequence, timing);
    }
  }
}

GnssErrorCode currentReceptionDiagnosis() {
  const UbxParserStats &u = ubx.stats();
  const SatelliteFrameStats &s = dispatcher.stats();
  const MaxM10s::Stats &m = gnss.stats();
  if (m.tx.shortWrites != 0U) return GNSS_ERR_TX_SHORT_WRITE;
  if (u.receivedBytes == 0U) return GNSS_ERR_NO_RX_DATA;
  if (u.validPackets == 0U) {
    const GnssRxClassification classification = gnss.bestRxClassification();
    if (classification == GnssRxClassification::LikelyNmea)
      return GNSS_ERR_RX_OK_TX_NO_RESPONSE;
    if (classification == GnssRxClassification::LikelyWrongBaud)
      return GNSS_ERR_LIKELY_WRONG_BAUD;
    return GNSS_ERR_RX_DATA_NO_VALID_PROTOCOL;
  }
  if (s.sfrbxTotal == 0U) return GNSS_ERR_NO_SFRBX;
  if (s.qzssSfrbx == 0U) return GNSS_ERR_NO_QZSS;
  if (s.qzssL1sSfrbx == 0U) return GNSS_ERR_NO_L1S;
  if (s.validPab == 0U && s.invalidPab != 0U)
    return GNSS_ERR_INVALID_L1S_PAB;
  if (s.validCrc == 0U && s.invalidCrc != 0U)
    return GNSS_ERR_INVALID_L1S_CRC;
  if (s.mt43 == 0U && s.mt44 == 0U && s.unsupportedMt != 0U)
    return GNSS_ERR_UNSUPPORTED_MT;
  return GNSS_ERR_NONE;
}

void printSelfTest() {
  const UbxParserStats &u = ubx.stats();
  const SatelliteFrameStats &s = dispatcher.stats();
  const TimeSyncStats &t = timeSync.stats();
  GnssSelfTestEvidence evidence = {};
  evidence.uartBytesObserved = u.receivedBytes != 0U;
  evidence.txAttempted = gnss.stats().tx.packets != 0U;
  evidence.validUbxObserved = u.validPackets != 0U;
  evidence.monVerObserved = gnss.monVerReceived();
  evidence.sfrbxObserved = s.sfrbxTotal != 0U;
  evidence.navPvtObserved = t.navPvtCount != 0U;
  evidence.ppsObserved = ppsState.sequence != 0U;
  evidence.ppsActivity = evaluatePpsActivity(
      pps.healthStats(), ppsState.edgeMicros, micros());
  evidence.optionalConfigFailure = gnss.initState() == GnssInitState::Degraded;
  evidence.timePulseAllVerified = gnss.timePulseConfigAllVerified();
  evidence.timePulseAnyFailure = gnss.timePulseConfigAnyFailure();
  evidence.qzss = gnss.configEvidence(CFG_SIGNAL_QZSS_ENA);
  evidence.qzssL1s = gnss.configEvidence(CFG_SIGNAL_QZSS_L1S_ENA);
  evidence.ubxOutput = gnss.configEvidence(CFG_UART1OUTPROT_UBX);
  evidence.sfrbxOutput = gnss.configEvidence(CFG_MSGOUT_RXM_SFRBX_UART1);
  evidence.navPvtOutput = gnss.configEvidence(CFG_MSGOUT_NAV_PVT_UART1);
  evidence.timePulseEnable = gnss.configEvidence(CFG_TP_TP1_ENA);
  evidence.timePulseGrid = gnss.configEvidence(CFG_TP_TIMEGRID_TP1);
  const GnssSelfTestReport report = evaluateGnssSelfTest(evidence);
  DebugSerial.println(F("=== HoshiGuard GNSS Self Test ==="));
  DebugSerial.print(F("GNSS RX activity: ")); DebugSerial.println(evidence.uartBytesObserved ? F("YES") : F("NO"));
  DebugSerial.print(F("GNSS TX attempted: ")); DebugSerial.println(evidence.txAttempted ? F("YES") : F("NO"));
  DebugSerial.print(F("Bidirectional UBX communication: "));
  DebugSerial.println(bidirectionalUbxStatusName(report.bidirectionalUbx));
  DebugSerial.print(F("Initial baud: "));
  if (gnss.initialBaud() != 0U) DebugSerial.println(gnss.initialBaud());
  else DebugSerial.println(F("UNKNOWN"));
  DebugSerial.print(F("Target baud: ")); DebugSerial.println(gnss.targetBaud());
  DebugSerial.print(F("Verified baud: "));
  if (gnss.verifiedBaud() != 0U) DebugSerial.println(gnss.verifiedBaud());
  else DebugSerial.println(F("UNKNOWN"));
  DebugSerial.print(F("Baud switch: "));
  DebugSerial.println(baudSwitchResultName(gnss.baudSwitchResult()));
  if (gnss.hasDetectedBaud()) {
    DebugSerial.print(F("Detected baud: ")); DebugSerial.println(gnss.detectedBaud());
    DebugSerial.println(F("Baud evidence: valid UBX"));
  } else {
    DebugSerial.println(F("Detected baud: UNKNOWN"));
    if (gnss.hasNmeaInferredBaud()) {
      DebugSerial.print(F("NMEA-only inferred baud: "));
      DebugSerial.println(gnss.nmeaInferredBaudValue());
    }
    DebugSerial.print(F("Last attempted baud: "));
    DebugSerial.println(gnss.lastAttemptedBaud());
  }
  DebugSerial.print(F("MON-VER: ")); DebugSerial.println(selfTestStatusName(report.monVer));
  DebugSerial.print(F("Module software: ")); DebugSerial.println(gnss.softwareVersion());
  DebugSerial.print(F("Module hardware: ")); DebugSerial.println(gnss.hardwareVersion());
  DebugSerial.print(F("QZSS enabled: ")); DebugSerial.println(selfTestStatusName(report.qzssEnabled));
  DebugSerial.print(F("QZSS L1S enabled: ")); DebugSerial.println(selfTestStatusName(report.qzssL1sEnabled));
  DebugSerial.print(F("UBX output: ")); DebugSerial.println(selfTestStatusName(report.ubxOutput));
  DebugSerial.print(F("SFRBX output: ")); DebugSerial.println(selfTestStatusName(report.sfrbxOutput));
  DebugSerial.print(F("NAV-PVT output: ")); DebugSerial.println(selfTestStatusName(report.navPvtOutput));
  DebugSerial.print(F("TIMEPULSE config: ")); DebugSerial.println(selfTestStatusName(report.timePulseConfig));
  DebugSerial.print(F("PPS activity: "));
  DebugSerial.println(ppsActivityName(report.ppsActivity));
  DebugSerial.print(F("Overall state: ")); DebugSerial.println(selfTestStatusName(report.overall));
  DebugSerial.println(F("================================"));
}

void printAdvice(GnssErrorCode diagnosis) {
  static GnssErrorCode printed = GNSS_ERR_NONE;
  if (diagnosis == GNSS_ERR_NONE || diagnosis == printed) return;
  printed = diagnosis;
  DebugSerial.setPriority(LogPriority::Critical);
  DebugSerial.print(F("ADVICE code=")); DebugSerial.print(gnssErrorName(diagnosis));
  DebugSerial.print(F(" action="));
  switch (diagnosis) {
    case GNSS_ERR_NO_RX_DATA: DebugSerial.println(F("check module power, common ground, TX/RX crossing and PA3 input")); break;
    case GNSS_ERR_RX_DATA_NO_VALID_PROTOCOL: DebugSerial.println(F("capture GNSS_RX_HEX; stream is neither checksum-valid UBX nor clear NMEA")); break;
    case GNSS_ERR_LIKELY_WRONG_BAUD: DebugSerial.println(F("try candidate baud rates and inspect module output with 3.3V USB-TTL")); break;
    case GNSS_ERR_NMEA_ONLY:
    case GNSS_ERR_RX_OK_TX_NO_RESPONSE: DebugSerial.println(F("MAX-M10S TX to PA3 appears active; verify PA2 to module RX, UBX input enable, port selection, level and ground")); break;
    case GNSS_ERR_TX_SHORT_WRITE: DebugSerial.println(F("USART2 write accepted fewer bytes than requested; inspect TX driver/state")); break;
    case GNSS_ERR_PROTOCOL_INPUT_DISABLED: DebugSerial.println(F("enable UBX input protocol on the module UART")); break;
    case GNSS_ERR_BAUD_SWITCH_LOST_COMMUNICATION: DebugSerial.println(F("baud switch recovery exhausted; inspect original/target probes and power-cycle the module")); break;
    case GNSS_ERR_NO_RESPONSE: DebugSerial.println(F("check crossed RX/TX, common ground, power and baud")); break;
    case GNSS_ERR_NO_VALID_UBX: DebugSerial.println(F("check NMEA-only output and UART parameters")); break;
    case GNSS_ERR_NO_SFRBX: DebugSerial.println(F("verify CFG-MSGOUT-UBX_RXM_SFRBX_UART1")); break;
    case GNSS_ERR_NO_QZSS: DebugSerial.println(F("improve sky view and check QZSS visibility")); break;
    case GNSS_ERR_NO_L1S: DebugSerial.println(F("verify QZSS L1S enable and sigId=1")); break;
    case GNSS_ERR_INVALID_L1S_PAB: DebugSerial.println(F("inspect word conversion and bit alignment")); break;
    case GNSS_ERR_INVALID_L1S_CRC: DebugSerial.println(F("inspect padding, CRC range and byte order")); break;
    case GNSS_ERR_UNSUPPORTED_MT: DebugSerial.println(F("valid L1S received; MT decoder is unsupported")); break;
    default: DebugSerial.println(F("inspect initialization log")); break;
  }
}

void printDiagnostics() {
  const UbxParserStats &u = ubx.stats();
  const MaxM10s::Stats &m = gnss.stats();
  const SatelliteFrameStats &s = dispatcher.stats();
  const TimeSyncStats &t = timeSync.stats();
  const RawCaptureStats &r = rawCapture.stats();
  const LogArbiterStats &l = DebugSerial.stats();
#if BRIDGE_BINARY_MODE
  const GnssErrorCode bridgeDiagnosis = currentReceptionDiagnosis();
  const BridgeStatusPayload status = {
      millis(), gnss.baudRate(), static_cast<uint8_t>(gnss.initState()),
      static_cast<uint8_t>(evaluatePpsActivity(
          pps.healthStats(), ppsState.edgeMicros, micros())),
      static_cast<uint8_t>(timeSync.quality()),
      static_cast<uint8_t>(bridgeDiagnosis), u.validPackets, u.checksumErrors,
      s.sfrbxTotal, s.qzssL1sSfrbx,
      bridgeTransport.stats().l1sGenerated, ppsState.sequence,
      ppsState.lostCount, ppsState.anomalyCount};
  (void)bridgeTransport.enqueueStatus(extendedMicros(), status);
  static GnssErrorCode previousError = GNSS_ERR_NONE;
  if (bridgeDiagnosis != GNSS_ERR_NONE && bridgeDiagnosis != previousError) {
    (void)bridgeTransport.enqueueError(
        extendedMicros(), static_cast<uint16_t>(bridgeDiagnosis), u.receivedBytes,
        u.validPackets);
  }
  previousError = bridgeDiagnosis;
  return;
#endif
  DebugSerial.setPriority(LogPriority::Periodic);
  DebugSerial.print(F("STATUS state=")); DebugSerial.print(gnssInitStateName(gnss.initState()));
  DebugSerial.print(F(" uart=")); DebugSerial.print(u.receivedBytes);
  DebugSerial.print(F(" ubx=")); DebugSerial.print(u.validPackets);
  DebugSerial.print(F(" sfrbx=")); DebugSerial.print(s.sfrbxTotal);
  DebugSerial.print(F(" qzss_sfrbx=")); DebugSerial.print(s.qzssSfrbx);
  DebugSerial.print(F(" l1ca=")); DebugSerial.print(s.qzssL1caSfrbx);
  DebugSerial.print(F(" l1s=")); DebugSerial.print(s.qzssL1sSfrbx);
  DebugSerial.print(F(" pab_ok=")); DebugSerial.print(s.validPab);
  DebugSerial.print(F(" crc_ok=")); DebugSerial.print(s.validCrc);
  DebugSerial.print(F(" mt43=")); DebugSerial.print(s.mt43);
  DebugSerial.print(F(" mt44=")); DebugSerial.print(s.mt44);
  DebugSerial.print(F(" pps=")); DebugSerial.print(ppsState.sequence);
  DebugSerial.print(F(" timtp=")); DebugSerial.print(t.timTpCount);
  DebugSerial.print(F(" time=")); DebugSerial.print(timeQualityName(timeSync.quality()));
  DebugSerial.print(F(" reason=")); DebugSerial.print(timeConfirmReasonName(timeSync.confirmReason()));
  DebugSerial.print(F(" drops=")); DebugSerial.print(r.droppedRecords);
  DebugSerial.print(F(" raw_bps=")); DebugSerial.print(r.bytesPerSecond);
  DebugSerial.print(F(" log_queued=")); DebugSerial.print(l.recordsQueued);
  DebugSerial.print(F(" log_dropped=")); DebugSerial.print(l.recordsDropped);
  DebugSerial.print(F(" log_max_pending=")); DebugSerial.print(l.maxPendingBytes);
  DebugSerial.print(F(" log_interleave_prevented="));
  DebugSerial.print(l.interleavePrevented);
  DebugSerial.print(F(" cfg_get=")); DebugSerial.print(m.valgetResponse);
  DebugSerial.print(F(" cfg_set=")); DebugSerial.print(m.valsetSent);
  DebugSerial.print(F(" tx_packets=")); DebugSerial.print(m.tx.packets);
  DebugSerial.print(F(" tx_req=")); DebugSerial.print(m.tx.bytesRequested);
  DebugSerial.print(F(" tx_written=")); DebugSerial.print(m.tx.bytesWritten);
  DebugSerial.print(F(" tx_short=")); DebugSerial.print(m.tx.shortWrites);
  DebugSerial.print(F(" rx_class="));
  DebugSerial.print(gnssRxClassificationName(gnss.bestRxClassification()));
  DebugSerial.print(F(" pps_activity="));
  DebugSerial.print(ppsActivityName(evaluatePpsActivity(
      pps.healthStats(), ppsState.edgeMicros, micros())));
  DebugSerial.print(F(" pps_lost_total=")); DebugSerial.print(ppsState.lostCount);
  DebugSerial.print(F(" pps_abnormal_total=")); DebugSerial.print(ppsState.anomalyCount);
  DebugSerial.print(F(" pps_consecutive_good=")); DebugSerial.print(ppsState.consecutiveGood);
  DebugSerial.print(F(" l1s_generated=")); DebugSerial.print(l.l1sRecordsGenerated);
  DebugSerial.print(F(" l1s_queued=")); DebugSerial.print(l.l1sRecordsQueued);
  DebugSerial.print(F(" l1s_sent=")); DebugSerial.print(l.l1sRecordsSent);
  DebugSerial.print(F(" l1s_dropped=")); DebugSerial.print(l.l1sRecordsDropped);
  DebugSerial.print(F(" other_dropped=")); DebugSerial.print(l.otherDropped);
  DebugSerial.print(F(" critical_dropped=")); DebugSerial.print(l.criticalDropped);
  DebugSerial.print(F(" uart_output_bytes=")); DebugSerial.print(l.totalOutputBytes);
  DebugSerial.print(F(" queue_high_watermark=")); DebugSerial.print(l.queueHighWatermark);
  const SafeFormatStats &format = safeFormatStats();
  DebugSerial.print(F(" format_truncated=")); DebugSerial.print(format.truncated);
  DebugSerial.print(F(" format_errors=")); DebugSerial.print(format.errors);
  DebugSerial.print(F(" log_format_truncated="));
  DebugSerial.print(l.formatTruncated);
  DebugSerial.print(F(" log_format_errors="));
  DebugSerial.print(l.formatErrors);
  const GnssErrorCode diagnosis = currentReceptionDiagnosis();
  DebugSerial.print(F(" diagnosis=")); DebugSerial.println(gnssErrorName(diagnosis));
  printAdvice(diagnosis);
}

bool emitStartupMetadata(void *) {
  static char line[224];
  const StartupMetadata metadata = {
      HOSHIGUARD_VERSION, __DATE__, __TIME__, HOSHIGUARD_GIT_COMMIT,
      CONFIGURE_GNSS_BBR ? "RAM+BBR" : "RAM", CAPTURE_ANTENNA,
      CAPTURE_REGION, CAPTURE_SKY_VIEW,
      OUTPUT_NAV_PVT_COORDINATES != 0 && CAPTURE_PRIVACY_MODE == 0};
  const SafeFormatResult formatted =
      formatStartupMetadata(metadata, line, sizeof(line));
  safePrintln(DebugSerial, line, formatted);
  const SafeFormatResult privacy = formatPrivacyMetadata(
      CAPTURE_PRIVACY_MODE != 0, CAPTURE_INCLUDE_NAV_PVT_RAW != 0,
      line, sizeof(line));
  safePrintln(DebugSerial, line, privacy);
  return formatted.status == SafeFormatStatus::Complete &&
         privacy.status == SafeFormatStatus::Complete;
}

void beginStartupPps(void *) { pps.begin(); }

void beginStartupGnss(void *) {
  rawCapture.begin(millis());
  gnss.begin(millis());
}
}

void setup() {
  DebugSerial.begin(DEBUG_BAUD);
#if BRIDGE_BINARY_MODE
  // MaxM10s の診断用 Print 依存は維持するが、本番バイナリモードでは USART1 への全テキスト出力を抑止する。
  // 物理ストリームは Bridge 専用とし、0x00 によるフレーム境界を保つ。
  DebugSerial.setSuppressed(true);
  bridgeTransport.begin();
#endif
  DebugSerial.setPriority(LogPriority::Critical);
  DebugSerial.println();
  DebugSerial.print(F("HoshiGuard v")); DebugSerial.print(HOSHIGUARD_VERSION);
  DebugSerial.println(F(" MAX-M10S QZSS L1S receiver"));
  DebugSerial.println(F("UART: USART1 PA9/PA10 115200; GNSS USART2 PA2/PA3"));
#if GNSS_UART_LOOPBACK_TEST
  DebugSerial.println(F("GNSS UART LOOPBACK MODE: disconnect MAX-M10S and short PA2 to PA3"));
  Serial2.begin(GNSS_TARGET_BAUD);
  loopbackExpectedLength = buildGnssLoopbackSequence(
      loopbackExpected, sizeof(loopbackExpected));
  const size_t written = Serial2.write(loopbackExpected, loopbackExpectedLength);
  DebugSerial.print(F("LOOPBACK_TX baud=")); DebugSerial.print(GNSS_TARGET_BAUD);
  DebugSerial.print(F(" requested=")); DebugSerial.print(loopbackExpectedLength);
  DebugSerial.print(F(" written=")); DebugSerial.println(written);
  loopbackDeadlineMs = millis() + 500U;
  if (written != loopbackExpectedLength) finishLoopback(false);
  return;
#endif
  const StartupHooks hooks = {0, emitStartupMetadata, beginStartupPps,
                              beginStartupGnss};
  (void)runStartupSequence(hooks);
#if HOSHIGUARD_OLED
  oledScreen.begin(millis());
#endif
#if BRIDGE_BINARY_MODE
  (void)bridgeTransport.enqueueHello(extendedMicros(), 0U);
#endif
}

void loop() {
  const uint32_t nowMs = millis();
#if GNSS_UART_LOOPBACK_TEST
  updateLoopback(nowMs);
  DebugSerial.pump();
  return;
#endif
  if (gnss.consumeParserResetRequest()) ubx.resetStreamState();
  (void)extendedMicros();  // 32 bit の周回が起きる前に micros() を拡張する。
  if (pps.update(ppsState)) {
#if HOSHIGUARD_OLED
    oledStatus.notePps(millis());
#endif
    const uint32_t generation = timeSync.timTpMatchGeneration();
    timeSync.observePps(ppsState.sequence, ppsState.edgeMicros);
    rawCapture.enqueuePps(ppsState.edgeMicros, ppsState.sequence,
                          ppsState.intervalMicros, ppsState.lostCount,
                          ppsState.anomalyCount, ppsState.consecutiveGood);
    if (timeSync.timTpMatchGeneration() != generation) {
      const TimTpObservation &match = timeSync.lastTimTp();
      rawCapture.enqueueTimTpMatch(
          ppsState.edgeMicros, ppsState.sequence, match.matched,
          match.ambiguous, timeQualityName(timeSync.quality()),
          timeConfirmReasonName(timeSync.confirmReason()));
    }
    // 新しいエッジへの暫定 UTC 割り当てにも、新しい NAV-PVT epoch が必要。
    utc.associatedWithPps = false;
    utc.ppsSequence = 0U;
    utc.quality = TimeQuality::PpsObserved;
#if BRIDGE_BINARY_MODE
    const TimTpObservation &timing = timeSync.lastTimTp();
    (void)bridgeTransport.enqueueTimeStatus(
        extendedMicros(), ppsState.sequence,
        static_cast<uint8_t>(evaluatePpsActivity(
            pps.healthStats(), ppsState.edgeMicros, micros())),
        static_cast<uint8_t>(timeSync.quality()),
        static_cast<uint8_t>(timeSync.confirmReason()), timing.message,
        timing.matchedPpsSequence, timing.matched, timing.ambiguous,
        timeSync.timeGridKnown());
#endif
#if DEBUG_LEVEL >= 3
    DebugSerial.print(F("PPS seq=")); DebugSerial.print(ppsState.sequence);
    DebugSerial.print(F(" interval_us=")); DebugSerial.println(ppsState.intervalMicros);
#endif
  }
  while (Serial2.available() > 0) {
    UbxPacketView packet;
    const int incoming = Serial2.read();
    if (incoming < 0) break;
    const uint8_t byte = static_cast<uint8_t>(incoming);
    gnss.observeRxByte(byte);
    if (ubx.feed(byte, packet)) {
      handlePacket(packet, nowMs);
    }
  }
  gnss.update(nowMs);
  const ConfigEvidence *grid = gnss.configEvidence(CFG_TP_TIMEGRID_TP1);
  const ConfigEvidence *enabled = gnss.configEvidence(CFG_TP_TP1_ENA);
  const ConfigEvidence *sync = gnss.configEvidence(CFG_TP_SYNC_GNSS_TP1);
  const ConfigEvidence *align = gnss.configEvidence(CFG_TP_ALIGN_TO_TOW_TP1);
  const ConfigEvidence *polarity = gnss.configEvidence(CFG_TP_POL_TP1);
  timeSync.setTimeGridKnown(grid != 0 && enabled != 0 && sync != 0 &&
                            align != 0 && polarity != 0 &&
                            configEvidenceVerified(*grid) &&
                            configEvidenceVerified(*enabled) &&
                            configEvidenceVerified(*sync) &&
                            configEvidenceVerified(*align) &&
                            configEvidenceVerified(*polarity));
  if (!selfTestPrinted &&
      (gnss.initState() == GnssInitState::Ready ||
       gnss.initState() == GnssInitState::Degraded ||
       gnss.initState() == GnssInitState::Failed)) {
    selfTestPrinted = true;
    printSelfTest();
  }
  rawCapture.pump(nowMs);
  static uint32_t nextDiagnostics = DIAGNOSTIC_INTERVAL_MS;
  if (static_cast<int32_t>(nowMs - nextDiagnostics) >= 0) {
    if (!rawCapture.busy()) {
      nextDiagnostics = nowMs + DIAGNOSTIC_INTERVAL_MS;
      printDiagnostics();
    }
  }
#if BRIDGE_BINARY_MODE
  static uint32_t nextHelloMs = 60000UL;
  if (static_cast<int32_t>(nowMs - nextHelloMs) >= 0) {
    nextHelloMs = nowMs + 60000UL;
    (void)bridgeTransport.enqueueHello(extendedMicros(), gnss.baudRate());
  }
  bridgeTransport.pump();
#else
  DebugSerial.pump();
#endif
#if HOSHIGUARD_OLED
  updateOled();
#endif
}
