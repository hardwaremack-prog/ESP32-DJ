// mixer.h - audio engine: per-deck EQ/filter, FX unit, crossfader, pads, outputs
#pragma once
#include <Arduino.h>

enum FxType { FX_NONE = 0, FX_ECHO, FX_FLANGER, FX_CRUSH, FX_GATE, FX_COUNT };

struct MixerState {
  volatile float xfader = 0.5f;     // 0 = A, 1 = B
  volatile float masterVol = 0.8f;
  volatile float hpMix = 0.3f;      // 0 = cue only, 1 = master only
  volatile float hpVol = 0.7f;
  volatile float padVol = 0.7f;
  volatile int fxType = FX_ECHO;
  volatile int fxTarget = 2;        // 0 = deck A, 1 = deck B, 2 = master
  volatile float fxAmount = 0.5f;   // wet / intensity
  volatile float fxBeats = 0.5f;    // beat fraction for echo / gate
  volatile bool fxOn = false;
  // meters (written by audio thread)
  volatile float vu[3] = {0, 0, 0}; // A, B, master (peak, 0..1+)
  volatile float cpu = 0;           // audio thread load 0..1
  volatile bool out1 = false, out2 = false;
  volatile uint32_t underruns = 0;
};

extern MixerState mix;
void mixerStart(bool psram);
void padTrigger(int i);
const char* padName(int i);
#define NUM_PADS 8
