#include "ubx_dispatcher.h"

#include <string.h>

#include "config.h"

UbxFrameDispatcher::UbxFrameDispatcher() { memset(&stats_, 0, sizeof(stats_)); }

bool UbxFrameDispatcher::processPacket(const UbxPacketView &packet,
                                       uint64_t receivedMicros,
                                       uint32_t ppsSequence,
                                       UbxDispatchEvent &event) {
  memset(&event, 0, sizeof(event));
  if (packet.messageClass != 0x02U || packet.messageId != 0x13U) return false;
  event.isSfrbx = true;
  ++stats_.sfrbxTotal;

  const SfrbxParseStatus status = parseSfrbxHeaderDetailed(
      packet.payload, packet.length, event.header);
  if (status != SfrbxParseStatus::Ok) {
    if (status == SfrbxParseStatus::InvalidWordCount)
      ++stats_.invalidWordCount;
    else
      ++stats_.invalidSfrbxLength;
    return true;
  }
  event.hasHeader = true;
  if (event.header.gnssId < 9U) ++stats_.sfrbxByGnss[event.header.gnssId];
  if (event.header.gnssId == 0U) ++stats_.gpsSfrbx;
  if (event.header.gnssId != 5U) return true;

  ++stats_.qzssSfrbx;
  if (event.header.sigId == 0U) {
    ++stats_.qzssL1caSfrbx;
    return true;
  }
  if (event.header.sigId != 1U) return true;
  ++stats_.qzssL1sSfrbx;
  if (!isSupportedQzssL1sWordCount(event.header.numWords)) {
    ++stats_.invalidWordCount;
    return true;
  }
  if (!extractQzssL1s(packet.payload, packet.length, event.frame)) return true;

  event.hasL1s = true;
  event.frame.receivedMicros = receivedMicros;
  event.frame.ppsSequence = ppsSequence;
  event.frame.duplicate = deduplicator_.checkAndRemember(
      event.frame.raw, receivedMicros, DEDUP_WINDOW_US);
  stats_.lastSvId = event.frame.svId;
  if (event.frame.svId < 16U) ++stats_.qzssBySv[event.frame.svId];
  if (event.frame.validPab) ++stats_.validPab;
  else ++stats_.invalidPab;
  if (event.frame.validCrc) ++stats_.validCrc;
  else ++stats_.invalidCrc;
  if (event.frame.messageType == 43U) ++stats_.mt43;
  else if (event.frame.messageType == 44U) ++stats_.mt44;
  else ++stats_.unsupportedMt;
  if (event.frame.duplicate) ++stats_.duplicateFrames;
  return true;
}

void injectUbxBytes(const uint8_t *data, size_t length, UbxParser &parser,
                    UbxFrameDispatcher &dispatcher,
                    UbxInjectionResult &result, uint64_t receivedMicros,
                    uint32_t ppsSequence) {
  memset(&result, 0, sizeof(result));
  if (data == 0) return;
  for (size_t i = 0; i < length; ++i) {
    UbxPacketView packet = {};
    if (!parser.feed(data[i], packet)) continue;
    ++result.validPackets;
    UbxDispatchEvent event = {};
    if (!dispatcher.processPacket(packet, receivedMicros, ppsSequence, event))
      continue;
    if (event.isSfrbx) ++result.sfrbxPackets;
    if (event.hasL1s) {
      ++result.l1sFrames;
      result.hasLastFrame = true;
      result.lastFrame = event.frame;
    }
  }
}
