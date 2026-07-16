#include "safe_format.h"

#include <stdio.h>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace {
SafeFormatStats gStats = {};
}

SafeFormatResult safeFormattedLength(int result, size_t capacity) {
  SafeFormatResult value = {0U, SafeFormatStatus::Error};
  if (result < 0 || capacity == 0U) return value;
  const size_t requested = static_cast<size_t>(result);
  if (requested >= capacity) {
    value.length = capacity - 1U;
    value.status = SafeFormatStatus::Truncated;
  } else {
    value.length = requested;
    value.status = SafeFormatStatus::Complete;
  }
  return value;
}

SafeFormatResult safeVsnprintf(char *buffer, size_t capacity,
                               const char *format, va_list arguments) {
  if (buffer == 0 || capacity == 0U || format == 0) {
    ++gStats.errors;
    return SafeFormatResult{0U, SafeFormatStatus::Error};
  }
  buffer[0] = '\0';
  const int result = vsnprintf(buffer, capacity, format, arguments);
  buffer[capacity - 1U] = '\0';
  const SafeFormatResult safe = safeFormattedLength(result, capacity);
  if (safe.status == SafeFormatStatus::Complete) ++gStats.completed;
  else if (safe.status == SafeFormatStatus::Truncated) ++gStats.truncated;
  else ++gStats.errors;
  return safe;
}

SafeFormatResult safeSnprintf(char *buffer, size_t capacity,
                              const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const SafeFormatResult result =
      safeVsnprintf(buffer, capacity, format, arguments);
  va_end(arguments);
  return result;
}

const SafeFormatStats &safeFormatStats() { return gStats; }

void resetSafeFormatStatsForTest() { gStats = SafeFormatStats{}; }

size_t formatUint64Decimal(uint64_t value, char *buffer, size_t capacity) {
  if (buffer == 0 || capacity == 0U) return 0U;
  char reverse[20];
  size_t count = 0U;
  do {
    reverse[count++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U && count < sizeof(reverse));
  if (count >= capacity) {
    buffer[0] = '\0';
    return 0U;
  }
  for (size_t i = 0U; i < count; ++i)
    buffer[i] = reverse[count - i - 1U];
  buffer[count] = '\0';
  return count;
}

#ifdef ARDUINO
size_t safeWrite(Print &output, const char *buffer,
                 const SafeFormatResult &result) {
  if (buffer == 0 || result.status == SafeFormatStatus::Error ||
      result.length == 0U) return 0U;
  return output.write(reinterpret_cast<const uint8_t *>(buffer), result.length);
}

size_t safePrintln(Print &output, const char *buffer,
                   const SafeFormatResult &result) {
  const size_t written = safeWrite(output, buffer, result);
  return written + output.println();
}
#endif
