#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class LogPriority : uint8_t {
  Critical = 0U,
  QzssL1s = 1U,
  Config = 2U,
  TimeQuality = 3U,
  Periodic = 4U,
  Capture = 5U,
  Verbose = 6U
};

enum class LogEnqueueResult : uint8_t { Queued, ReplacedLowerPriority, Dropped };

template <size_t Depth, size_t RecordSize, size_t L1sReservedSlots = 0U>
class LogRecordQueue {
 public:
  LogRecordQueue()
      : count_(0U), active_(Depth), activeOffset_(0U), nextSequence_(1U),
        pendingBytes_(0U), maxPendingBytes_(0U), maxCount_(0U),
        lastEvictedPriority_(LogPriority::Verbose) {
    for (size_t i = 0U; i < Depth; ++i) {
      slots_[i].used = false;
      slots_[i].length = 0U;
      slots_[i].sequence = 0U;
      slots_[i].priority = LogPriority::Verbose;
    }
  }

  LogEnqueueResult enqueue(LogPriority priority, const char *data,
                           size_t length) {
    if (data == 0 || length == 0U || length > RecordSize)
      return LogEnqueueResult::Dropped;
    size_t selected = Depth;
    size_t l1sQueued = 0U;
    for (size_t i = 0U; i < Depth; ++i)
      if (slots_[i].used && slots_[i].priority == LogPriority::QzssL1s)
        ++l1sQueued;
    const size_t reserveNeeded = L1sReservedSlots > l1sQueued
                                     ? L1sReservedSlots - l1sQueued : 0U;
    const bool protectedRecord = priority == LogPriority::Critical ||
                                 priority == LogPriority::QzssL1s;
    const bool mayUseEmpty = protectedRecord || count_ < Depth - reserveNeeded;
    if (mayUseEmpty) {
      for (size_t i = 0U; i < Depth; ++i) {
        if (!slots_[i].used) {
          selected = i;
          break;
        }
      }
    }
    LogEnqueueResult result = LogEnqueueResult::Queued;
    if (selected == Depth) {
      uint8_t worst = static_cast<uint8_t>(priority);
      uint32_t newest = 0U;
      for (size_t i = 0U; i < Depth; ++i) {
        if (i == active_) continue;
        const uint8_t candidate = static_cast<uint8_t>(slots_[i].priority);
        if (candidate > worst ||
            (candidate == worst && candidate > static_cast<uint8_t>(priority) &&
             slots_[i].sequence > newest)) {
          worst = candidate;
          newest = slots_[i].sequence;
          selected = i;
        }
      }
      if (selected == Depth) return LogEnqueueResult::Dropped;
      lastEvictedPriority_ = slots_[selected].priority;
      pendingBytes_ -= slots_[selected].length;
      result = LogEnqueueResult::ReplacedLowerPriority;
    } else {
      ++count_;
    }
    memcpy(slots_[selected].data, data, length);
    slots_[selected].length = static_cast<uint16_t>(length);
    slots_[selected].priority = priority;
    slots_[selected].sequence = nextSequence_++;
    slots_[selected].used = true;
    pendingBytes_ += length;
    if (pendingBytes_ > maxPendingBytes_) maxPendingBytes_ = pendingBytes_;
    if (count_ > maxCount_) maxCount_ = count_;
    return result;
  }

  bool activateFront() {
    if (active_ != Depth) return true;
    uint32_t oldest = 0xFFFFFFFFUL;
    size_t selected = Depth;
    for (size_t i = 0U; i < Depth; ++i) {
      if (slots_[i].used && slots_[i].sequence < oldest) {
        oldest = slots_[i].sequence;
        selected = i;
      }
    }
    if (selected == Depth) return false;
    active_ = selected;
    activeOffset_ = 0U;
    return true;
  }

  const char *frontData() const {
    return active_ == Depth ? 0 : slots_[active_].data + activeOffset_;
  }
  size_t frontRemaining() const {
    return active_ == Depth ? 0U : slots_[active_].length - activeOffset_;
  }
  LogPriority frontPriority() const {
    return active_ == Depth ? LogPriority::Verbose : slots_[active_].priority;
  }
  bool advance(size_t amount) {
    if (active_ == Depth) return false;
    const size_t remaining = frontRemaining();
    if (amount > remaining) amount = remaining;
    activeOffset_ = static_cast<uint16_t>(activeOffset_ + amount);
    pendingBytes_ -= amount;
    if (activeOffset_ == slots_[active_].length) {
      slots_[active_].used = false;
      slots_[active_].length = 0U;
      active_ = Depth;
      activeOffset_ = 0U;
      --count_;
      return true;
    }
    return false;
  }
  bool active() const { return active_ != Depth; }
  size_t count() const { return count_; }
  size_t pendingBytes() const { return pendingBytes_; }
  size_t maxPendingBytes() const { return maxPendingBytes_; }
  size_t maxCount() const { return maxCount_; }
  LogPriority lastEvictedPriority() const { return lastEvictedPriority_; }

 private:
  static_assert(Depth > 0U, "log queue depth must be positive");
  static_assert(L1sReservedSlots <= Depth, "L1S reservation exceeds queue");
  static_assert(RecordSize > 1U && RecordSize <= 0xFFFFU,
                "log record must fit uint16_t");
  struct Slot {
    char data[RecordSize];
    uint16_t length;
    uint32_t sequence;
    LogPriority priority;
    bool used;
  };
  Slot slots_[Depth];
  size_t count_;
  size_t active_;
  uint16_t activeOffset_;
  uint32_t nextSequence_;
  size_t pendingBytes_;
  size_t maxPendingBytes_;
  size_t maxCount_;
  LogPriority lastEvictedPriority_;
};

#ifdef ARDUINO
#include <Arduino.h>
#include "config.h"

#ifndef LOG_QUEUE_DEPTH
#define LOG_QUEUE_DEPTH 4U
#endif
#ifndef LOG_RECORD_MAX
#if RAW_CAPTURE_MODE
#define LOG_RECORD_MAX RAW_CAPTURE_MAX_RECORD
#else
#define LOG_RECORD_MAX 256U
#endif
#endif

struct LogArbiterStats {
  uint32_t recordsQueued;
  uint32_t recordsDropped;
  uint32_t recordsSent;
  uint32_t criticalDropped;
  uint32_t otherDropped;
  uint32_t l1sRecordsGenerated;
  uint32_t l1sRecordsQueued;
  uint32_t l1sRecordsSent;
  uint32_t l1sRecordsDropped;
  uint32_t maxPendingBytes;
  uint32_t queueHighWatermark;
  uint32_t interleavePrevented;
  uint32_t formatErrors;
  uint32_t formatTruncated;
  uint32_t totalOutputBytes;
};

class LogArbiter : public Print {
 public:
  explicit LogArbiter(HardwareSerial &serial);
  void begin(uint32_t baud);
  using Print::write;
  size_t write(uint8_t byte) override;
  bool enqueueRecord(LogPriority priority, const char *data, size_t length);
  void setPriority(LogPriority priority) { currentPriority_ = priority; }
  void setSuppressed(bool suppressed) { suppressed_ = suppressed; }
  void pump();
  bool busy() const { return queue_.count() != 0U || lineLength_ != 0U; }
  const LogArbiterStats &stats() const { return stats_; }

 private:
  void commitLine();
  HardwareSerial &serial_;
  LogRecordQueue<LOG_QUEUE_DEPTH, LOG_RECORD_MAX,
                 LOG_L1S_RESERVED_SLOTS> queue_;
  char line_[LOG_RECORD_MAX];
  size_t lineLength_;
  bool lineOverflow_;
  LogPriority currentPriority_;
  bool suppressed_;
  LogArbiterStats stats_;
};
#endif
