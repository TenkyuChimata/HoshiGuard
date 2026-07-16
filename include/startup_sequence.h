#pragma once

#include <stdint.h>

struct StartupHooks {
  void *context;
  bool (*emitMetadata)(void *context);
  void (*beginPps)(void *context);
  void (*beginGnss)(void *context);
};

struct StartupSequenceResult {
  bool metadataComplete;
  bool ppsStarted;
  bool gnssStarted;
};

StartupSequenceResult runStartupSequence(const StartupHooks &hooks);
bool shouldEndSerialBeforeBegin(bool serialAlreadyStarted);
bool advanceBaudCandidate(uint8_t &candidate, uint8_t candidateCount);

