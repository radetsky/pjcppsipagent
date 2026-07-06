#include "silence.h"

SilenceDetector::SilenceDetector(unsigned threshold, unsigned timeout_ms)
    : threshold_(threshold), timeout_ms_(timeout_ms) {}

void SilenceDetector::arm() {
  armed_ = true;
  started_ = false;
  silence_start_ = 0;
}

bool SilenceDetector::feed(unsigned level, uint64_t now_ms) {
  if (!armed_) return false;
  if (level >= threshold_) {
    started_ = false;
    silence_start_ = 0;
    return false;
  }
  if (!started_) {
    started_ = true;
    silence_start_ = now_ms;
  }
  if (now_ms - silence_start_ >= timeout_ms_) {
    armed_ = false;
    started_ = false;
    silence_start_ = 0;
    return true;
  }
  return false;
}

unsigned SilenceDetector::getCurrentSilenceMs(uint64_t now_ms) const {
  if (!armed_ || !started_) return 0;
  return static_cast<unsigned>(now_ms - silence_start_);
}
