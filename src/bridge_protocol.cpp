#include "bridge_protocol.h"

namespace bridge {

void putU16Le(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}
void putU32Le(uint8_t *out, uint32_t value) {
  for (uint8_t i = 0U; i < 4U; ++i)
    out[i] = static_cast<uint8_t>(value >> (8U * i));
}
void putU64Le(uint8_t *out, uint64_t value) {
  for (uint8_t i = 0U; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(value >> (8U * i));
}
uint16_t getU16Le(const uint8_t *in) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                               (static_cast<uint16_t>(in[1]) << 8));
}
uint32_t getU32Le(const uint8_t *in) {
  uint32_t value = 0U;
  for (uint8_t i = 0U; i < 4U; ++i)
    value |= static_cast<uint32_t>(in[i]) << (8U * i);
  return value;
}
uint64_t getU64Le(const uint8_t *in) {
  uint64_t value = 0U;
  for (uint8_t i = 0U; i < 8U; ++i)
    value |= static_cast<uint64_t>(in[i]) << (8U * i);
  return value;
}

uint32_t crc32IsoHdlc(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit)
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
  }
  return crc ^ 0xFFFFFFFFUL;
}

bool cobsEncode(const uint8_t *input, size_t inputLength, uint8_t *output,
                size_t outputCapacity, size_t &outputLength) {
  outputLength = 0U;
  if ((input == 0 && inputLength != 0U) || output == 0 || outputCapacity == 0U)
    return false;
  size_t readIndex = 0U;
  size_t codeIndex = 0U;
  size_t writeIndex = 1U;
  uint8_t code = 1U;
  if (writeIndex > outputCapacity) return false;
  while (readIndex < inputLength) {
    if (input[readIndex] == 0U) {
      if (codeIndex >= outputCapacity) return false;
      output[codeIndex] = code;
      code = 1U;
      codeIndex = writeIndex++;
      if (writeIndex > outputCapacity) return false;
      ++readIndex;
    } else {
      if (writeIndex >= outputCapacity) return false;
      output[writeIndex++] = input[readIndex++];
      ++code;
      if (code == 0xFFU) {
        if (codeIndex >= outputCapacity) return false;
        output[codeIndex] = code;
        code = 1U;
        codeIndex = writeIndex++;
        if (writeIndex > outputCapacity) return false;
      }
    }
  }
  if (codeIndex >= outputCapacity) return false;
  output[codeIndex] = code;
  outputLength = writeIndex;
  return true;
}

bool cobsDecode(const uint8_t *input, size_t inputLength, uint8_t *output,
                size_t outputCapacity, size_t &outputLength) {
  outputLength = 0U;
  if (input == 0 || output == 0 || inputLength == 0U) return false;
  size_t readIndex = 0U;
  size_t writeIndex = 0U;
  while (readIndex < inputLength) {
    const uint8_t code = input[readIndex++];
    if (code == 0U) return false;
    const size_t copyLength = static_cast<size_t>(code - 1U);
    if (copyLength > inputLength - readIndex || copyLength > outputCapacity - writeIndex)
      return false;
    for (size_t i = 0U; i < copyLength; ++i) {
      const uint8_t byte = input[readIndex++];
      if (byte == 0U) return false;
      output[writeIndex++] = byte;
    }
    if (code != 0xFFU && readIndex < inputLength) {
      if (writeIndex >= outputCapacity) return false;
      output[writeIndex++] = 0U;
    }
  }
  outputLength = writeIndex;
  return true;
}

bool encodeLogicalFrame(const Header &header, const uint8_t *payload,
                        uint8_t *output, size_t outputCapacity,
                        size_t &outputLength) {
  outputLength = 0U;
  const size_t total = kHeaderLength + header.payloadLength + kCrcLength;
  if (total > outputCapacity || total > kMaximumLogicalFrame ||
      (header.payloadLength != 0U && payload == 0)) return false;
  output[0] = kMagic0;
  output[1] = kMagic1;
  output[2] = kProtocolMajor;
  output[3] = kProtocolMinor;
  output[4] = static_cast<uint8_t>(header.frameType);
  output[5] = 0U;
  putU16Le(output + 6U, kHeaderLength);
  putU16Le(output + 8U, header.payloadLength);
  putU16Le(output + 10U, header.flags);
  putU32Le(output + 12U, header.sequence);
  putU64Le(output + 16U, header.timestampUs);
  if (header.payloadLength != 0U)
    memcpy(output + kHeaderLength, payload, header.payloadLength);
  putU32Le(output + total - kCrcLength,
           crc32IsoHdlc(output, total - kCrcLength));
  outputLength = total;
  return true;
}

bool encodeWireFrame(const Header &header, const uint8_t *payload,
                     uint8_t *output, size_t outputCapacity,
                     size_t &outputLength) {
  outputLength = 0U;
  uint8_t logical[kMaximumLogicalFrame];
  size_t logicalLength = 0U;
  if (!encodeLogicalFrame(header, payload, logical, sizeof(logical), logicalLength) ||
      outputCapacity < 2U) return false;
  size_t encodedLength = 0U;
  if (!cobsEncode(logical, logicalLength, output, outputCapacity - 1U,
                  encodedLength)) return false;
  output[encodedLength++] = 0U;
  outputLength = encodedLength;
  return true;
}

bool decodeLogicalFrame(const uint8_t *input, size_t inputLength,
                        DecodedFrame &frame, uint8_t supportedMajor) {
  if (input == 0 || inputLength < kHeaderLength + kCrcLength ||
      inputLength > kMaximumLogicalFrame) return false;
  if (input[0] != kMagic0 || input[1] != kMagic1 ||
      input[2] != supportedMajor || getU16Le(input + 6U) != kHeaderLength)
    return false;
  const uint16_t payloadLength = getU16Le(input + 8U);
  if (inputLength != kHeaderLength + payloadLength + kCrcLength) return false;
  frame.receivedCrc = getU32Le(input + inputLength - kCrcLength);
  frame.calculatedCrc = crc32IsoHdlc(input, inputLength - kCrcLength);
  if (frame.receivedCrc != frame.calculatedCrc) return false;
  frame.header.frameType = static_cast<FrameType>(input[4]);
  frame.header.payloadLength = payloadLength;
  frame.header.flags = getU16Le(input + 10U);
  frame.header.sequence = getU32Le(input + 12U);
  frame.header.timestampUs = getU64Le(input + 16U);
  frame.payload = input + kHeaderLength;
  return true;
}

}  // namespace bridge
