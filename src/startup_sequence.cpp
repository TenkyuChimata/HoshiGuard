#include "startup_sequence.h"

StartupSequenceResult runStartupSequence(const StartupHooks &hooks) {
  StartupSequenceResult result = {};
  if (hooks.emitMetadata != 0)
    result.metadataComplete = hooks.emitMetadata(hooks.context);
  if (hooks.beginPps != 0) {
    hooks.beginPps(hooks.context);
    result.ppsStarted = true;
  }
  if (hooks.beginGnss != 0) {
    hooks.beginGnss(hooks.context);
    result.gnssStarted = true;
  }
  return result;
}

bool shouldEndSerialBeforeBegin(bool serialAlreadyStarted) {
  return serialAlreadyStarted;
}

bool advanceBaudCandidate(uint8_t &candidate, uint8_t candidateCount) {
  if (candidate < 0xFFU) ++candidate;
  return candidate < candidateCount;
}

