#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

enum class SafeFormatStatus : uint8_t { Complete, Truncated, Error };

struct SafeFormatResult {
  size_t length;
  SafeFormatStatus status;
};

struct SafeFormatStats {
  uint32_t completed;
  uint32_t truncated;
  uint32_t errors;
};

SafeFormatResult safeFormattedLength(int result, size_t capacity);
SafeFormatResult safeVsnprintf(char *buffer, size_t capacity,
                               const char *format, va_list arguments);
SafeFormatResult safeSnprintf(char *buffer, size_t capacity,
                              const char *format, ...);
const SafeFormatStats &safeFormatStats();
void resetSafeFormatStatsForTest();
size_t formatUint64Decimal(uint64_t value, char *buffer, size_t capacity);

#ifdef ARDUINO
class Print;
size_t safeWrite(Print &output, const char *buffer,
                 const SafeFormatResult &result);
size_t safePrintln(Print &output, const char *buffer,
                   const SafeFormatResult &result);
#endif
