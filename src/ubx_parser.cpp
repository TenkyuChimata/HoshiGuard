#include "ubx_parser.h"

#include <string.h>

UbxParser::UbxParser() : nmeaActive_(false) {
  memset(&stats_, 0, sizeof(stats_));
  reset();
}

void UbxParser::reset() {
  state_ = SYNC1;
  messageClass_ = 0;
  messageId_ = 0;
  payloadLength_ = 0;
  payloadIndex_ = 0;
  ckA_ = 0;
  ckB_ = 0;
  receivedCkA_ = 0;
}

void UbxParser::resetStreamState() {
  reset();
  nmeaActive_ = false;
}

void UbxParser::checksumByte(uint8_t byte) {
  ckA_ = static_cast<uint8_t>(ckA_ + byte);
  ckB_ = static_cast<uint8_t>(ckB_ + ckA_);
}

void UbxParser::resyncFrom(uint8_t byte) {
  ++stats_.resyncs;
  state_ = (byte == 0xB5U) ? SYNC2 : SYNC1;
  payloadIndex_ = 0;
  payloadLength_ = 0;
  ckA_ = 0;
  ckB_ = 0;
}

bool UbxParser::feed(uint8_t byte, UbxPacketView &packet) {
  ++stats_.receivedBytes;

  // 認識できる NMEA 文のみを数え、任意のバイナリノイズを NMEA としない。この集計は UBX の再同期に影響しない。
  if (!nmeaActive_ && state_ == SYNC1 && byte == '$') nmeaActive_ = true;
  if (nmeaActive_) {
    ++stats_.nmeaBytesSkipped;
    if (byte == '\n') nmeaActive_ = false;
  }

  switch (state_) {
    case SYNC1:
      if (byte == 0xB5U) state_ = SYNC2;
      break;
    case SYNC2:
      if (byte == 0x62U) {
        state_ = CLASS;
        ckA_ = ckB_ = 0;
      } else if (byte != 0xB5U) {
        resyncFrom(byte);
      }
      break;
    case CLASS:
      messageClass_ = byte;
      checksumByte(byte);
      state_ = ID;
      break;
    case ID:
      messageId_ = byte;
      checksumByte(byte);
      state_ = LENGTH_LOW;
      break;
    case LENGTH_LOW:
      payloadLength_ = byte;
      checksumByte(byte);
      state_ = LENGTH_HIGH;
      break;
    case LENGTH_HIGH:
      payloadLength_ = static_cast<uint16_t>(
          payloadLength_ |
          static_cast<uint16_t>(static_cast<uint16_t>(byte) << 8));
      checksumByte(byte);
      payloadIndex_ = 0;
      if (payloadLength_ > UBX_MAX_PAYLOAD) {
        ++stats_.oversizedPackets;
        resyncFrom(byte);
      } else {
        state_ = payloadLength_ == 0U ? CHECKSUM_A : PAYLOAD;
      }
      break;
    case PAYLOAD:
      payload_[payloadIndex_++] = byte;
      checksumByte(byte);
      if (payloadIndex_ == payloadLength_) state_ = CHECKSUM_A;
      break;
    case CHECKSUM_A:
      receivedCkA_ = byte;
      state_ = CHECKSUM_B;
      break;
    case CHECKSUM_B: {
      const bool valid = (receivedCkA_ == ckA_) && (byte == ckB_);
      if (valid) {
        packet.messageClass = messageClass_;
        packet.messageId = messageId_;
        packet.length = payloadLength_;
        packet.payload = payload_;
        ++stats_.validPackets;
        reset();
        return true;
      }
      ++stats_.checksumErrors;
      resyncFrom(byte);
      break;
    }
  }
  return false;
}

void UbxParser::checksum(const uint8_t *data, size_t length,
                         uint8_t &ckA, uint8_t &ckB) {
  ckA = 0;
  ckB = 0;
  for (size_t i = 0; i < length; ++i) {
    ckA = static_cast<uint8_t>(ckA + data[i]);
    ckB = static_cast<uint8_t>(ckB + ckA);
  }
}

size_t UbxParser::buildPacket(uint8_t messageClass, uint8_t messageId,
                              const uint8_t *payload, uint16_t payloadLength,
                              uint8_t *output, size_t outputCapacity) {
  const size_t total = static_cast<size_t>(payloadLength) + 8U;
  if (output == 0 || outputCapacity < total ||
      (payloadLength != 0U && payload == 0)) {
    return 0;
  }
  output[0] = 0xB5;
  output[1] = 0x62;
  output[2] = messageClass;
  output[3] = messageId;
  output[4] = static_cast<uint8_t>(payloadLength);
  output[5] = static_cast<uint8_t>(payloadLength >> 8);
  if (payloadLength != 0U) memcpy(output + 6, payload, payloadLength);
  checksum(output + 2, static_cast<size_t>(payloadLength) + 4U,
           output[6U + payloadLength], output[7U + payloadLength]);
  return total;
}
