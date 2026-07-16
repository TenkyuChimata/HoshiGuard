#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "qzss_l1s.h"
#include "safe_format.h"
#include "tim_tp.h"
#include "ubx_parser.h"

size_t encodeHexUpper(const uint8_t *data, size_t length, char *output,
                      size_t capacity);
size_t formatUbxRawRecord(uint64_t timestampUs, const UbxPacketView &packet,
                          char *output, size_t capacity);
size_t formatQzssL1sUbxRecord(uint64_t timestampUs, uint32_t ppsSequence,
                              const SfrbxHeader &header,
                              const QzssL1sFrame &frame,
                              const UbxPacketView &packet,
                              char *output, size_t capacity);
size_t formatSfrbxRecord(uint64_t timestampUs, const SfrbxHeader &header,
                         const uint8_t *payload, uint16_t payloadLength,
                         char *output, size_t capacity);
size_t formatL1sRecord(const QzssL1sFrame &frame, char *output,
                       size_t capacity);
size_t formatSfrbxHeaderRecord(uint64_t timestampUs,
                               const SfrbxHeader &header,
                               char *output, size_t capacity);
size_t formatPpsRecord(uint32_t timestampUs, uint32_t sequence,
                       uint32_t intervalUs, uint32_t lost,
                       uint32_t abnormal, uint32_t consecutiveGood,
                       char *output, size_t capacity);
size_t formatTimTpRecord(uint32_t timestampUs, uint32_t previousPpsSequence,
                         const UbxTimTp &message, char *output,
                         size_t capacity);
size_t formatTimTpMatchRecord(uint32_t timestampUs, uint32_t ppsSequence,
                              bool matched, bool ambiguous,
                              const char *quality, const char *reason,
                              char *output, size_t capacity);

#ifndef ARDUINO
bool parseUbxRawRecord(const char *line, uint8_t *packet,
                       size_t packetCapacity, size_t &packetLength,
                       uint64_t &timestampUs);
#endif

template <size_t Depth, size_t RecordSize>
class FixedLogQueue {
 public:
  FixedLogQueue() : head_(0U), tail_(0U), count_(0U) {
    for (size_t i = 0U; i < Depth; ++i) {
      lengths_[i] = 0U;
      offsets_[i] = 0U;
    }
  }

  char *reserve() {
    return count_ < Depth ? records_[tail_] : 0;
  }

  bool commit(size_t length) {
    if (count_ >= Depth || length == 0U || length >= RecordSize) return false;
    lengths_[tail_] = static_cast<uint16_t>(length);
    offsets_[tail_] = 0U;
    tail_ = (tail_ + 1U) % Depth;
    ++count_;
    return true;
  }

  const char *frontData() const { return count_ == 0U ? 0 : records_[head_]; }
  uint16_t frontLength() const { return count_ == 0U ? 0U : lengths_[head_]; }
  uint16_t frontOffset() const { return count_ == 0U ? 0U : offsets_[head_]; }
  void advance(size_t amount) {
    if (count_ == 0U) return;
    const size_t remaining = lengths_[head_] - offsets_[head_];
    if (amount > remaining) amount = remaining;
    offsets_[head_] = static_cast<uint16_t>(offsets_[head_] + amount);
  }
  bool frontComplete() const {
    return count_ != 0U && offsets_[head_] == lengths_[head_];
  }
  void pop() {
    if (count_ == 0U) return;
    head_ = (head_ + 1U) % Depth;
    --count_;
  }
  size_t count() const { return count_; }

 private:
  static_assert(Depth > 0U, "queue depth must be positive");
  static_assert(RecordSize > 1U && RecordSize <= 0xFFFFU,
                "record size must fit uint16_t");
  char records_[Depth][RecordSize];
  uint16_t lengths_[Depth];
  uint16_t offsets_[Depth];
  size_t head_;
  size_t tail_;
  size_t count_;
};

#ifdef ARDUINO
#include <Arduino.h>
#include "log_arbiter.h"

struct RawCaptureStats {
  uint32_t droppedRecords;
  uint32_t maxRecordLength;
  uint32_t maxPendingBytes;
  uint32_t bytesPerSecond;
  uint32_t totalOutputBytes;
  uint32_t logRecordsQueued;
  uint32_t logRecordsDropped;
  uint32_t logMaxPending;
  uint32_t logInterleavePrevented;
  uint32_t logFormatErrors;
  uint32_t logFormatTruncated;
  uint32_t l1sRecordsGenerated;
  uint32_t l1sRecordsQueued;
  uint32_t l1sRecordsSent;
  uint32_t l1sRecordsDropped;
  uint32_t otherRecordsDropped;
  uint32_t criticalRecordsDropped;
  uint32_t uartOutputBytes;
  uint32_t queueHighWatermark;
};

class RawCaptureLogger {
 public:
  explicit RawCaptureLogger(LogArbiter &log);
  void begin(uint32_t nowMs);
  void enqueueUbx(uint64_t timestampUs, const UbxPacketView &packet);
  void enqueueQzssL1sUbx(uint64_t timestampUs, uint32_t ppsSequence,
                         const SfrbxHeader &header,
                         const QzssL1sFrame &frame,
                         const UbxPacketView &packet);
  void enqueueSfrbx(uint64_t timestampUs, const SfrbxHeader &header,
                    const uint8_t *payload, uint16_t payloadLength);
  void enqueueSfrbxHeader(uint64_t timestampUs, const SfrbxHeader &header);
  void enqueueL1s(const QzssL1sFrame &frame);
  void enqueuePps(uint32_t timestampUs, uint32_t sequence,
                  uint32_t intervalUs, uint32_t lost, uint32_t abnormal,
                  uint32_t consecutiveGood);
  void enqueueTimTp(uint32_t timestampUs, uint32_t previousPpsSequence,
                    const UbxTimTp &message);
  void enqueueTimTpMatch(uint32_t timestampUs, uint32_t ppsSequence,
                         bool matched, bool ambiguous, const char *quality,
                         const char *reason);
  void pump(uint32_t nowMs);
  bool busy() const;
  const RawCaptureStats &stats() const { return stats_; }

 private:
#if RAW_CAPTURE_MODE
  void commitRecord(size_t length, LogPriority priority = LogPriority::Capture);
  char record_[RAW_CAPTURE_MAX_RECORD];
  uint32_t windowBytes_;
  uint32_t windowStartMs_;
  uint32_t lastOutputBytes_;
#endif
  LogArbiter &log_;
  RawCaptureStats stats_;
};
#endif
