// bpm.h - beat/tempo detector (onset envelope + autocorrelation + comb refine)
// Platform independent so it can be unit-tested on a PC.
#pragma once
#include <stdint.h>

class BpmAnalyzer {
 public:
  ~BpmAnalyzer();
  // maxSeconds of audio to analyse. Returns false if out of memory.
  bool begin(uint32_t sampleRate, int maxSeconds = 40);
  void end();
  // Feed interleaved 16-bit PCM. channels = 1 or 2.
  void feed(const int16_t* pcm, int frames, int channels);
  bool full() const { return n_ >= cap_; }
  int seconds() const { return n_ / 100; }
  // Compute tempo. firstBeat is in source sample frames from the start of the fed audio.
  bool compute(float& bpm, double& firstBeat, float& confidence);

 private:
  float* env_ = nullptr;  // onset strength at 100 Hz
  int cap_ = 0, n_ = 0;
  uint32_t rate_ = 44100;
  int hop_ = 441, hopPos_ = 0;
  float lpLow_ = 0, lpMid_ = 0, eLow_ = 0, eHigh_ = 0;
  float prevLow_ = 0, prevHigh_ = 0;
  float aLow_ = 0, aMid_ = 0;
  float acfAt(const float* x, int n, float lag) const;
};
