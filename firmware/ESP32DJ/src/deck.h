// deck.h - one DJ deck: MP3 decoding into a ring buffer + varispeed playback
//
// Threads:
//  * decoder task (core 0): owns the file, the Helix decoder and writeT
//  * audio task   (core 1): owns playT, reads the ring, calls render()
//  * control      (web/service): sends commands through a queue, reads state
//
// The ring is indexed by a monotonically increasing "timeline" T. Seeks and
// loop wraps append new audio at the end of the timeline and record a
// Segment (T -> source frame) so the playhead never has to stop.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include "config.h"
#include "mp3util.h"
#include "helix/mp3dec.h"

#define MAX_SEGS 16

struct Segment {
  int64_t t;
  double src;
};

enum DeckCmdType : uint8_t { DC_LOAD, DC_SEEK, DC_LOOP, DC_LOOP_EXIT, DC_UNLOAD };

struct DeckCmd {
  DeckCmdType type;
  double a, b;
  char path[200];
};

enum DeckState : uint8_t { DS_EMPTY, DS_LOADING, DS_READY, DS_ERROR };

class Deck {
 public:
  void init(int id, bool psram);

  // ---------- control API (any task) ----------
  void load(const char* path);
  void unload();
  void seek(double srcFrame);
  void setLoop(double startSrc, double endSrc);
  void exitLoop();
  double posSrc();                 // playhead in source frames
  double posSec() { return posSrc() / srcRate; }
  double durSec() { return srcRate ? durFrames / srcRate : 0; }
  double beatFrames() { return bpm > 0 ? 60.0 * srcRate / bpm : 0.5 * srcRate; }
  float tempo() { return 1.0f + (syncOn ? syncPitch : pitchFader * pitchRange) + phaseAdj; }
  float effBpm() { return bpm * (1.0f + (syncOn ? syncPitch : pitchFader * pitchRange)); }
  double beatPhase();              // 0..1 position inside the current beat
  double nearestBeat(double src, bool floorIt);
  void setIndex(uint32_t* idx, uint32_t count, uint32_t totalMp3Frames);

  // ---------- audio thread ----------
  void render(float* L, float* R, int n);

  // ---------- state (read by UI, written as noted) ----------
  int id = 0;
  volatile DeckState state = DS_EMPTY;
  volatile uint32_t gen = 0;         // bumps on every load
  char path[200] = {0};
  char title[96] = {0};
  char artist[96] = {0};
  volatile uint32_t srcRate = 44100;
  volatile double durFrames = 0;
  volatile bool exactIndex = false;
  volatile bool analyzing = false;
  uint32_t idxStep = 8;               // seek index keeps every Nth MP3 frame offset

  // transport / control (written by control thread)
  volatile bool playing = false;
  volatile bool cuePreview = false;
  volatile bool atEnd = false;
  volatile float pitchFader = 0;     // -1..1
  volatile float pitchRange = 0.08f; // 0.08 = +/-8 %
  volatile bool syncOn = false;
  volatile float syncPitch = 0;      // pitch used while synced
  volatile float phaseAdj = 0;       // tiny speed correction from sync phase lock
  volatile float nudge = 0;          // jog nudge (speed offset)
  volatile bool scratching = false;
  volatile float jogVel = 0;         // scratch speed (1 = normal)
  volatile bool braking = false;
  volatile bool reverse = false;
  volatile float bpm = 0;            // detected / set BPM (original tempo)
  volatile double firstBeat = 0;     // source frame of a downbeat
  volatile double cuePoint = 0;
  double hot[HOTCUES] = {-1, -1, -1, -1};
  volatile bool loopOn = false;
  volatile double loopStart = 0, loopEnd = 0;
  volatile float loopBeats = 0;
  volatile float speedNow = 0;       // actual playback speed (for jog wheel)

  // DSP params (control) - processed in mixer
  volatile float volume = 0.8f, trim = 1.0f, eqLo = 1, eqMid = 1, eqHi = 1, filter = 0;
  volatile bool pfl = false;

 private:
  static void taskTrampoline(void* p);
  void taskLoop();
  void handle(const DeckCmd& c);
  void doLoad(const char* p);
  void doClose();
  void internalSeek(int64_t target);
  void decodeOne();
  void emit(const int16_t* pcm, int n, int ch);
  void writeFrames(const int16_t* pcm, int n, int ch);
  void addSegment(int64_t t, double src);
  double srcAtLocked(int64_t t);
  int64_t pubPlay();
  void cutTimeline(int64_t cut);
  bool refill();

  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  QueueHandle_t q = nullptr;
  TaskHandle_t task = nullptr;

  // ring (stereo int16)
  int16_t* ring = nullptr;
  uint32_t ringFrames = 0, mask = 0, history = 0, lookahead = 0, guard = 0;
  volatile int64_t writeT = 0;
  volatile int64_t pendingJumpT = -1;
  volatile int64_t pubPlayT = 0;
  volatile bool eof = false;
  volatile bool resetPlay = false;
  double playT = 0;      // audio thread only
  float scratchSpeed = 0, brakeMul = 1;
  Segment segs[MAX_SEGS];
  uint32_t segHead = 0, segCount = 0;

  // decoder
  File file;
  HMP3Decoder dec = nullptr;
  uint8_t* inbuf = nullptr;
  int inLeft = 0;
  uint8_t* inPtr = nullptr;
  bool fileEof = false;
  uint32_t dataStart = 0, fileSize = 0;
  Mp3Hdr hdr0;
  int spf = 1152;
  int64_t frameNo = 0;
  int64_t decSrc = 0;
  int64_t discard = 0;
  int16_t* pcm = nullptr;
  uint32_t estFrames = 0;
  // seek index (published by analyzer)
  uint32_t* idx = nullptr;
  uint32_t idxCount = 0;
};

extern Deck decks[NUM_DECKS];
