#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bridge {

static const uint8_t kMagic0 = 0x48U;  // 'H'
static const uint8_t kMagic1 = 0x47U;  // 'G'
static const uint8_t kProtocolMajor = 1U;
static const uint8_t kProtocolMinor = 0U;
static const uint16_t kHeaderLength = 24U;
static const size_t kCrcLength = 4U;
static const size_t kMaximumUbxLength = 264U;
static const size_t kMaximumLogicalFrame = 384U;
static const size_t kMaximumEncodedFrame =
    kMaximumLogicalFrame + (kMaximumLogicalFrame / 254U) + 2U;

enum class FrameType : uint8_t {
  Hello = 1U,
  L1sFrame = 2U,
  Status = 3U,
  TimeStatus = 4U,
  Error = 5U,
  Heartbeat = 6U
};

enum L1sFlags : uint16_t {
  L1sFlagUbxChecksumValid = 1U << 0,
  L1sFlagLengthValid = 1U << 1,
  L1sFlagPabValid = 1U << 2,
  L1sFlagCrcValid = 1U << 3,
  L1sFlagDuplicateSatelliteCopy = 1U << 4,
  L1sFlagPpsMatched = 1U << 5,
  L1sFlagUtcAvailable = 1U << 6,
  L1sFlagTpLocked = 1U << 7,
  L1sFlagTimeLimited = 1U << 8,
  L1sFlagTimeConfirmed = 1U << 9,
  L1sFlagQueuePressure = 1U << 10
};

struct Header {
  FrameType frameType;
  uint16_t payloadLength;
  uint16_t flags;
  uint32_t sequence;
  uint64_t timestampUs;
};

struct DecodedFrame {
  Header header;
  const uint8_t *payload;
  uint32_t receivedCrc;
  uint32_t calculatedCrc;
};

// CRC-32/ISO-HDLC: poly 0x04C11DB7（反転実装 0xEDB88320）、init 0xFFFFFFFF、refin/refout true、xorout 0xFFFFFFFF。
// wire 値はリトルエンディアン。
uint32_t crc32IsoHdlc(const uint8_t *data, size_t length);

bool cobsEncode(const uint8_t *input, size_t inputLength, uint8_t *output,
                size_t outputCapacity, size_t &outputLength);
bool cobsDecode(const uint8_t *input, size_t inputLength, uint8_t *output,
                size_t outputCapacity, size_t &outputLength);

void putU16Le(uint8_t *out, uint16_t value);
void putU32Le(uint8_t *out, uint32_t value);
void putU64Le(uint8_t *out, uint64_t value);
uint16_t getU16Le(const uint8_t *in);
uint32_t getU32Le(const uint8_t *in);
uint64_t getU64Le(const uint8_t *in);

bool encodeLogicalFrame(const Header &header, const uint8_t *payload,
                        uint8_t *output, size_t outputCapacity,
                        size_t &outputLength);
bool encodeWireFrame(const Header &header, const uint8_t *payload,
                     uint8_t *output, size_t outputCapacity,
                     size_t &outputLength);
bool decodeLogicalFrame(const uint8_t *input, size_t inputLength,
                        DecodedFrame &frame, uint8_t supportedMajor =
                            kProtocolMajor);

enum class Priority : uint8_t {
  Critical = 0U,
  L1s = 1U,
  Time = 2U,
  Status = 3U
};

enum class EnqueueResult : uint8_t { Queued, ReplacedLower, Dropped };

// 末尾のゼロ区切りを含む完全な COBS レコードを保存する。送信中のレコードは置換せず、UART の部分書き込みでも一体性を保つ。
template <size_t Depth, size_t RecordSize, size_t L1sReservedSlots>
class RecordQueue {
 public:
  RecordQueue()
      : active_(Depth), activeOffset_(0U), count_(0U), nextOrder_(1U),
        highWatermark_(0U), lastEvicted_(Priority::Status) {
    for (size_t i = 0U; i < Depth; ++i) slots_[i].used = false;
  }

  EnqueueResult enqueue(Priority priority, const uint8_t *data,
                        size_t length) {
    if (data == 0 || length == 0U || length > RecordSize)
      return EnqueueResult::Dropped;
    size_t l1sCount = 0U;
    for (size_t i = 0U; i < Depth; ++i)
      if (slots_[i].used && slots_[i].priority == Priority::L1s) ++l1sCount;
    const size_t reserve = L1sReservedSlots > l1sCount
                               ? L1sReservedSlots - l1sCount
                               : 0U;
    const bool protectedFrame = priority == Priority::Critical ||
                                priority == Priority::L1s;
    size_t selected = Depth;
    if (protectedFrame || count_ < Depth - reserve) {
      for (size_t i = 0U; i < Depth; ++i) {
        if (!slots_[i].used) {
          selected = i;
          break;
        }
      }
    }
    EnqueueResult result = EnqueueResult::Queued;
    if (selected == Depth) {
      uint8_t worst = static_cast<uint8_t>(priority);
      uint32_t newest = 0U;
      for (size_t i = 0U; i < Depth; ++i) {
        if (!slots_[i].used || i == active_) continue;
        if (slots_[i].priority == Priority::L1s) continue;
        const uint8_t candidate = static_cast<uint8_t>(slots_[i].priority);
        if (candidate > worst ||
            (candidate == worst && candidate > static_cast<uint8_t>(priority) &&
             slots_[i].order > newest)) {
          worst = candidate;
          newest = slots_[i].order;
          selected = i;
        }
      }
      if (selected == Depth) return EnqueueResult::Dropped;
      lastEvicted_ = slots_[selected].priority;
      result = EnqueueResult::ReplacedLower;
    } else {
      ++count_;
    }
    memcpy(slots_[selected].data, data, length);
    slots_[selected].length = static_cast<uint16_t>(length);
    slots_[selected].priority = priority;
    slots_[selected].order = nextOrder_++;
    slots_[selected].used = true;
    if (count_ > highWatermark_) highWatermark_ = count_;
    return result;
  }

  bool activateFront() {
    if (active_ != Depth) return true;
    uint32_t oldest = 0xFFFFFFFFUL;
    size_t selected = Depth;
    for (size_t i = 0U; i < Depth; ++i) {
      if (slots_[i].used && slots_[i].order < oldest) {
        oldest = slots_[i].order;
        selected = i;
      }
    }
    if (selected == Depth) return false;
    active_ = selected;
    activeOffset_ = 0U;
    return true;
  }

  const uint8_t *frontData() const {
    return active_ == Depth ? 0 : slots_[active_].data + activeOffset_;
  }
  size_t frontRemaining() const {
    return active_ == Depth ? 0U : slots_[active_].length - activeOffset_;
  }
  Priority frontPriority() const {
    return active_ == Depth ? Priority::Status : slots_[active_].priority;
  }
  bool advance(size_t amount) {
    if (active_ == Depth) return false;
    if (amount > frontRemaining()) amount = frontRemaining();
    activeOffset_ = static_cast<uint16_t>(activeOffset_ + amount);
    if (activeOffset_ != slots_[active_].length) return false;
    slots_[active_].used = false;
    active_ = Depth;
    activeOffset_ = 0U;
    --count_;
    return true;
  }
  size_t count() const { return count_; }
  size_t highWatermark() const { return highWatermark_; }
  bool active() const { return active_ != Depth; }
  Priority lastEvictedPriority() const { return lastEvicted_; }

 private:
  static_assert(Depth > 0U, "bridge queue depth must be positive");
  static_assert(L1sReservedSlots <= Depth, "invalid L1S reservation");
  static_assert(RecordSize <= 0xFFFFU, "bridge record length field overflow");
  struct Slot {
    uint8_t data[RecordSize];
    uint16_t length;
    uint32_t order;
    Priority priority;
    bool used;
  };
  Slot slots_[Depth];
  size_t active_;
  uint16_t activeOffset_;
  size_t count_;
  uint32_t nextOrder_;
  size_t highWatermark_;
  Priority lastEvicted_;
};

}  // namespace bridge
