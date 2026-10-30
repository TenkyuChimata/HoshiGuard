#include "log_arbiter.h"

#ifdef ARDUINO
LogArbiter::LogArbiter(HardwareSerial &serial)
    : serial_(serial), lineLength_(0U), lineOverflow_(false),
      currentPriority_(LogPriority::Config), suppressed_(false) {
  memset(&stats_, 0, sizeof(stats_));
}

void LogArbiter::begin(uint32_t baud) { serial_.begin(baud); }

size_t LogArbiter::write(uint8_t byte) {
  if (suppressed_) return 1U;
  if (!lineOverflow_) {
    if (lineLength_ < sizeof(line_)) line_[lineLength_++] = static_cast<char>(byte);
    else lineOverflow_ = true;
  }
  if (byte == '\n') commitLine();
  return 1U;
}

void LogArbiter::commitLine() {
  if (lineOverflow_) {
    ++stats_.formatTruncated;
    ++stats_.recordsDropped;
    if (currentPriority_ == LogPriority::Critical) ++stats_.criticalDropped;
  } else if (lineLength_ != 0U) {
    (void)enqueueRecord(currentPriority_, line_, lineLength_);
  }
  lineLength_ = 0U;
  lineOverflow_ = false;
  currentPriority_ = LogPriority::Config;
}

bool LogArbiter::enqueueRecord(LogPriority priority, const char *data,
                               size_t length) {
  if (priority == LogPriority::QzssL1s) ++stats_.l1sRecordsGenerated;
  if (data == 0 || length == 0U || length > LOG_RECORD_MAX) {
    ++stats_.formatErrors;
    ++stats_.recordsDropped;
    if (priority == LogPriority::Critical) ++stats_.criticalDropped;
    else if (priority == LogPriority::QzssL1s) ++stats_.l1sRecordsDropped;
    else ++stats_.otherDropped;
    return false;
  }
  if (queue_.active()) ++stats_.interleavePrevented;
  const LogEnqueueResult result = queue_.enqueue(priority, data, length);
  if (result == LogEnqueueResult::Dropped) {
    ++stats_.recordsDropped;
    if (priority == LogPriority::Critical) ++stats_.criticalDropped;
    else if (priority == LogPriority::QzssL1s) ++stats_.l1sRecordsDropped;
    else ++stats_.otherDropped;
    return false;
  }
  if (result == LogEnqueueResult::ReplacedLowerPriority) {
    ++stats_.recordsDropped;
    const LogPriority evicted = queue_.lastEvictedPriority();
    if (evicted == LogPriority::Critical) ++stats_.criticalDropped;
    else if (evicted == LogPriority::QzssL1s) ++stats_.l1sRecordsDropped;
    else ++stats_.otherDropped;
  }
  ++stats_.recordsQueued;
  if (priority == LogPriority::QzssL1s) ++stats_.l1sRecordsQueued;
  if (queue_.maxPendingBytes() > stats_.maxPendingBytes)
    stats_.maxPendingBytes = static_cast<uint32_t>(queue_.maxPendingBytes());
  if (queue_.maxCount() > stats_.queueHighWatermark)
    stats_.queueHighWatermark = static_cast<uint32_t>(queue_.maxCount());
  // 次のループを待たず、可能なら UART 送信を開始する。
  pump();
  return true;
}

void LogArbiter::pump() {
  if (!queue_.activateFront()) return;
  const int available = serial_.availableForWrite();
  if (available <= 0) return;
  const size_t remaining = queue_.frontRemaining();
  const size_t amount = remaining < static_cast<size_t>(available)
                            ? remaining : static_cast<size_t>(available);
  const size_t written = serial_.write(
      reinterpret_cast<const uint8_t *>(queue_.frontData()), amount);
  const size_t accepted = written < amount ? written : amount;
  const LogPriority priority = queue_.frontPriority();
  const bool completed = queue_.advance(accepted);
  stats_.totalOutputBytes += static_cast<uint32_t>(accepted);
  if (completed) {
    ++stats_.recordsSent;
    if (priority == LogPriority::QzssL1s) ++stats_.l1sRecordsSent;
  }
}
#endif
