#pragma once

#include <stddef.h>

#include "safe_format.h"

struct StartupMetadata {
  const char *firmware;
  const char *buildDate;
  const char *buildTime;
  const char *gitCommit;
  const char *configLayer;
  const char *antenna;
  const char *region;
  const char *skyView;
  bool navCoordinatesEnabled;
};

SafeFormatResult formatStartupMetadata(const StartupMetadata &metadata,
                                       char *buffer, size_t capacity);
SafeFormatResult formatPrivacyMetadata(bool privacyMode,
                                       bool navPvtRawEnabled,
                                       char *buffer, size_t capacity);
