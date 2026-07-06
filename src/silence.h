#ifndef SILENCE_H
#define SILENCE_H

#include <cstdint>

class SilenceDetector {
 public:
  SilenceDetector(unsigned threshold, unsigned timeout_ms);

  // Reset the detector to inactive state
  void arm();

  // Call with a received audio level (0-255) and current time in ms.
  // Returns true exactly once when silence timeout is reached after arming.
  bool feed(unsigned level, uint64_t now_ms);

  unsigned getCurrentSilenceMs(uint64_t now_ms) const;

  unsigned getThreshold() const { return threshold_; }
  unsigned getTimeoutMs() const { return timeout_ms_; }
  bool isArmed() const { return armed_; }

 private:
  unsigned threshold_;
  unsigned timeout_ms_;
  uint64_t silence_start_ = 0;
  bool armed_ = false;
  bool started_ = false;
};

#endif // SILENCE_H
