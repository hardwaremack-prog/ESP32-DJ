// deck.cpp - see deck.h
#include "deck.h"
#include <SD.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "analyzer.h"

#define INBUF 4096

Deck decks[NUM_DECKS];

static void* allocPrefer(size_t n, bool psram) {
  void* p = nullptr;
  if (psram) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void Deck::init(int deckId, bool psram) {
  id = deckId;
  // PSRAM (V4): 3 s ring. Internal RAM only (V3): 186 ms ring.
  uint32_t want = psram ? 131072 : 8192;
  idxStep = psram ? 2 : 8;
  while (!ring && want >= 2048) {
    size_t bytes = want * 4;
    if (psram || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) > bytes + 60000)
      ring = (int16_t*)allocPrefer(bytes, psram);
    if (!ring) want /= 2;
  }
  ringFrames = ring ? want : 0;
  mask = ringFrames - 1;
  history = ringFrames / 4;
  lookahead = ringFrames / 2;
  guard = psram ? 4096 : ringFrames / 4;
  if (ring) memset(ring, 0, ringFrames * 4);
  inbuf = (uint8_t*)heap_caps_malloc(INBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  pcm = (int16_t*)heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  inPtr = inbuf;
  pitchRange = 0.08f;
  q = xQueueCreate(6, sizeof(DeckCmd));
  char name[12];
  snprintf(name, sizeof(name), "deck%d", id);
  xTaskCreatePinnedToCore(taskTrampoline, name, 5120, this, 6, &task, 0);
  Serial.printf("[deck%d] ring %u frames (%s)\n", id, (unsigned)ringFrames, psram ? "PSRAM" : "internal");
}

// ---------------- control API ----------------
void Deck::load(const char* p) {
  DeckCmd c = {};
  c.type = DC_LOAD;
  strlcpy(c.path, p, sizeof(c.path));
  state = DS_LOADING;
  playing = false;
  xQueueSend(q, &c, pdMS_TO_TICKS(50));
}

void Deck::unload() {
  DeckCmd c = {};
  c.type = DC_UNLOAD;
  playing = false;
  xQueueSend(q, &c, pdMS_TO_TICKS(50));
}

void Deck::seek(double src) {
  if (state != DS_READY) return;
  if (src < 0) src = 0;
  if (durFrames > 0 && src > durFrames - 1) src = durFrames - 1;
  DeckCmd c = {};
  c.type = DC_SEEK;
  c.a = src;
  atEnd = false;
  xQueueSend(q, &c, pdMS_TO_TICKS(50));
}

void Deck::setLoop(double s, double e) {
  if (state != DS_READY || e <= s + 64) return;
  DeckCmd c = {};
  c.type = DC_LOOP;
  c.a = s; c.b = e;
  loopStart = s; loopEnd = e;
  loopOn = true;
  xQueueSend(q, &c, pdMS_TO_TICKS(50));
}

void Deck::exitLoop() {
  if (!loopOn) return;
  DeckCmd c = {};
  c.type = DC_LOOP_EXIT;
  loopOn = false;
  xQueueSend(q, &c, pdMS_TO_TICKS(50));
}

int64_t Deck::pubPlay() {
  portENTER_CRITICAL(&mux);
  int64_t t = pubPlayT;
  portEXIT_CRITICAL(&mux);
  return t;
}

double Deck::srcAtLocked(int64_t t) {
  if (!segCount) return (double)t;
  for (uint32_t k = 0; k < segCount; k++) {
    const Segment& s = segs[(segHead - 1 - k) & (MAX_SEGS - 1)];
    if (s.t <= t) return s.src + (double)(t - s.t);
  }
  const Segment& o = segs[(segHead - segCount) & (MAX_SEGS - 1)];
  return o.src + (double)(t - o.t);
}

double Deck::posSrc() {
  portENTER_CRITICAL(&mux);
  int64_t t = pendingJumpT >= 0 ? pendingJumpT : pubPlayT;
  double s = srcAtLocked(t);
  portEXIT_CRITICAL(&mux);
  return s < 0 ? 0 : s;
}

double Deck::beatPhase() {
  if (bpm <= 0) return 0;
  double x = (posSrc() - firstBeat) / beatFrames();
  return x - floor(x);
}

double Deck::nearestBeat(double src, bool floorIt) {
  if (bpm <= 0) return src;
  double bl = beatFrames();
  double k = (src - firstBeat) / bl;
  k = floorIt ? floor(k + 0.02) : round(k);
  double r = firstBeat + k * bl;
  return r < 0 ? 0 : r;
}

void Deck::setIndex(uint32_t* newIdx, uint32_t count, uint32_t totalMp3Frames) {
  uint32_t* old;
  portENTER_CRITICAL(&mux);
  old = idx;
  idx = newIdx;
  idxCount = count;
  portEXIT_CRITICAL(&mux);
  free(old);
  if (totalMp3Frames) {
    estFrames = totalMp3Frames;
    durFrames = (double)totalMp3Frames * spf;
  }
  exactIndex = newIdx != nullptr;
}

// ---------------- audio thread ----------------
static inline float hermite(float xm1, float x0, float x1, float x2, float t) {
  float c = (x1 - xm1) * 0.5f;
  float v = x0 - x1;
  float w = c + v;
  float a = w + v + (x2 - x0) * 0.5f;
  float b = w + a;
  return ((((a * t) - b) * t + c) * t + x0);
}

void Deck::render(float* L, float* R, int n) {
  if (state != DS_READY || !ring) {
    memset(L, 0, n * sizeof(float));
    memset(R, 0, n * sizeof(float));
    speedNow = 0;
    return;
  }
  int64_t wT;
  portENTER_CRITICAL(&mux);
  if (resetPlay) { playT = 0; resetPlay = false; }
  wT = writeT;
  if (pendingJumpT >= 0) {
    if (playT >= (double)pendingJumpT) pendingJumpT = -1;
    else if (wT >= pendingJumpT + 64 || eof) { playT = (double)pendingJumpT; pendingJumpT = -1; }
  }
  portEXIT_CRITICAL(&mux);

  float sp;
  if (scratching) {
    scratchSpeed += (jogVel - scratchSpeed) * 0.35f;
    sp = scratchSpeed;
  } else if (braking) {
    brakeMul *= 0.986f;
    sp = tempo() * brakeMul;
    if (brakeMul < 0.01f) { braking = false; playing = false; brakeMul = 1; }
  } else if (playing) {
    sp = tempo() + nudge;
    scratchSpeed = sp;
    brakeMul = 1;
  } else {
    memset(L, 0, n * sizeof(float));
    memset(R, 0, n * sizeof(float));
    speedNow = 0;
    scratchSpeed = 0;
    portENTER_CRITICAL(&mux);
    pubPlayT = (int64_t)playT;
    portEXIT_CRITICAL(&mux);
    return;
  }
  speedNow = sp;
  double step = (double)sp * srcRate / SAMPLE_RATE;
  double lo = (double)(wT - (int64_t)ringFrames + 8);
  if (lo < 1) lo = 1;
  const float k = 1.0f / 32768.0f;
  for (int i = 0; i < n; i++) {
    double t = playT;
    int64_t i0 = (int64_t)floor(t);
    if (i0 + 2 >= wT) {  // no data yet (or end of track)
      L[i] = R[i] = 0;
      if (eof && step > 0 && !scratching) { playing = false; atEnd = true; }
      if (step < 0) playT += step;
      continue;
    }
    if (i0 - 1 < (int64_t)lo) {  // ran back past the history
      L[i] = R[i] = 0;
      playT = lo + 1;
      continue;
    }
    float f = (float)(t - (double)i0);
    const int16_t* a = ring + ((i0 - 1) & mask) * 2;
    const int16_t* b = ring + ((i0) & mask) * 2;
    const int16_t* c = ring + ((i0 + 1) & mask) * 2;
    const int16_t* d = ring + ((i0 + 2) & mask) * 2;
    L[i] = hermite(a[0], b[0], c[0], d[0], f) * k;
    R[i] = hermite(a[1], b[1], c[1], d[1], f) * k;
    playT += step;
  }
  portENTER_CRITICAL(&mux);
  pubPlayT = (int64_t)playT;
  portEXIT_CRITICAL(&mux);
}

// ---------------- decoder task ----------------
void Deck::taskTrampoline(void* p) { ((Deck*)p)->taskLoop(); }

void Deck::taskLoop() {
  DeckCmd c;
  for (;;) {
    if (!file || eof || state != DS_READY) {
      if (xQueueReceive(q, &c, pdMS_TO_TICKS(20)) == pdTRUE) handle(c);
      continue;
    }
    while (xQueueReceive(q, &c, 0) == pdTRUE) handle(c);
    if (!file || eof || state != DS_READY) continue;
    int64_t pT = pubPlay();
    int64_t wT = writeT;
    bool full = (wT - pT) >= (int64_t)lookahead ||
                (wT + 1152 + 8) > (pT - (int64_t)history + (int64_t)ringFrames);
    if (full) {
      if (xQueueReceive(q, &c, pdMS_TO_TICKS(4)) == pdTRUE) handle(c);
      continue;
    }
    decodeOne();
  }
}

void Deck::addSegment(int64_t t, double src) {
  segs[segHead & (MAX_SEGS - 1)] = {t, src};
  segHead++;
  if (segCount < MAX_SEGS) segCount++;
}

void Deck::cutTimeline(int64_t cut) {
  portENTER_CRITICAL(&mux);
  if (cut < writeT) writeT = cut;
  while (segCount > 1) {
    const Segment& s = segs[(segHead - 1) & (MAX_SEGS - 1)];
    if (s.t >= cut) { segHead--; segCount--; }
    else break;
  }
  portEXIT_CRITICAL(&mux);
}

void Deck::handle(const DeckCmd& c) {
  switch (c.type) {
    case DC_LOAD:
      doLoad(c.path);
      break;
    case DC_UNLOAD:
      doClose();
      path[0] = title[0] = artist[0] = 0;
      bpm = 0;
      break;
    case DC_SEEK: {
      if (state != DS_READY) break;
      int64_t pT = pubPlay();
      int64_t cut = pT + guard;
      if (cut > writeT) cut = writeT;
      if (cut < pT) cut = pT;
      cutTimeline(cut);
      internalSeek((int64_t)c.a);
      portENTER_CRITICAL(&mux);
      addSegment(cut, (double)(int64_t)c.a);
      pendingJumpT = cut;
      portEXIT_CRITICAL(&mux);
      break;
    }
    case DC_LOOP: {
      if (state != DS_READY) break;
      double a = c.a, b = c.b;
      int64_t pT = pubPlay();
      double sp;
      Segment newest;
      portENTER_CRITICAL(&mux);
      sp = srcAtLocked(pT);
      newest = segs[(segHead - 1) & (MAX_SEGS - 1)];
      int64_t wT = writeT;
      portEXIT_CRITICAL(&mux);
      bool inside = sp >= a && sp < b;
      if (inside && decSrc <= (int64_t)b && newest.t <= pT) break;  // decoder will wrap by itself
      int64_t tEnd = newest.t + (int64_t)(b - newest.src);
      if (inside && newest.src <= b && tEnd >= pT + 64 && tEnd <= wT) {
        cutTimeline(tEnd);
        internalSeek((int64_t)a);
        portENTER_CRITICAL(&mux);
        addSegment(tEnd, a);
        portEXIT_CRITICAL(&mux);
      } else {
        int64_t cut = pT + guard;
        if (cut > wT) cut = wT;
        double sc;
        portENTER_CRITICAL(&mux);
        sc = srcAtLocked(cut);
        portEXIT_CRITICAL(&mux);
        double ns = sc >= a ? a + fmod(sc - a, b - a) : sc;
        cutTimeline(cut);
        internalSeek((int64_t)ns);
        portENTER_CRITICAL(&mux);
        addSegment(cut, (double)(int64_t)ns);
        portEXIT_CRITICAL(&mux);
      }
      break;
    }
    case DC_LOOP_EXIT: {
      if (state != DS_READY) break;
      int64_t pT = pubPlay();
      Segment newest;
      portENTER_CRITICAL(&mux);
      newest = segs[(segHead - 1) & (MAX_SEGS - 1)];
      portEXIT_CRITICAL(&mux);
      if (newest.t <= pT) break;  // data ahead is linear already
      int64_t cut = pT + guard;
      if (cut > writeT) cut = writeT;
      double sc;
      portENTER_CRITICAL(&mux);
      sc = srcAtLocked(cut);
      portEXIT_CRITICAL(&mux);
      cutTimeline(cut);
      internalSeek((int64_t)sc);
      portENTER_CRITICAL(&mux);
      addSegment(cut, (double)(int64_t)sc);
      portEXIT_CRITICAL(&mux);
      break;
    }
  }
}

void Deck::doClose() {
  state = DS_EMPTY;
  playing = false;
  if (file) file.close();
  eof = true;
  uint32_t* old;
  portENTER_CRITICAL(&mux);
  old = idx;
  idx = nullptr;
  idxCount = 0;
  portEXIT_CRITICAL(&mux);
  free(old);
}

static bool validAt(const uint8_t* b, int n, int i, Mp3Hdr& h) {
  if (i + 4 > n || !mp3ParseHeader(b + i, h)) return false;
  int j = i + h.frameBytes;
  if (j + 4 <= n) {
    Mp3Hdr h2;
    if (!mp3ParseHeader(b + j, h2) || !mp3SameStream(h, h2)) return false;
  }
  return true;
}

void Deck::doLoad(const char* p) {
  doClose();
  state = DS_LOADING;
  gen++;
  strlcpy(path, p, sizeof(path));
  title[0] = artist[0] = 0;
  bpm = 0; firstBeat = 0; cuePoint = 0;
  for (int i = 0; i < HOTCUES; i++) hot[i] = -1;
  loopOn = false; atEnd = false; playing = false; cuePreview = false;
  syncOn = false; syncPitch = 0; phaseAdj = 0; nudge = 0; braking = false; scratching = false;
  exactIndex = false;
  durFrames = 0;
  if (!ring || !inbuf || !pcm) { state = DS_ERROR; return; }
  file = SD.open(p, FILE_READ);
  if (!file || file.isDirectory()) { state = DS_ERROR; return; }
  fileSize = file.size();
  int n = file.read(inbuf, 10);
  uint32_t tag = id3v2TagSize(inbuf, n);
  if (tag) {
    file.seek(0);
    n = file.read(inbuf, tag < INBUF ? tag : INBUF);
    id3v2ReadText(inbuf, n, title, sizeof(title), artist, sizeof(artist));
  }
  if (!title[0]) {
    const char* s = strrchr(p, '/');
    strlcpy(title, s ? s + 1 : p, sizeof(title));
    char* dot = strrchr(title, '.');
    if (dot) *dot = 0;
  }
  // locate the first real frame (within 256 KB after the tag)
  uint32_t base = tag;
  bool found = false;
  Mp3Hdr h;
  for (int tries = 0; tries < 64 && !found; tries++) {
    file.seek(base);
    n = file.read(inbuf, INBUF);
    if (n < 8) break;
    for (int i = 0; i + 4 <= n - 4; i++) {
      if (inbuf[i] == 0xFF && validAt(inbuf, n, i, h)) {
        dataStart = base + i;
        found = true;
        uint32_t x = mp3XingFrames(inbuf + i, n - i, h);
        hdr0 = h;
        spf = h.samplesPerFrame;
        srcRate = h.sampleRate;
        estFrames = x ? x : (fileSize - dataStart) / (h.frameBytes ? h.frameBytes : 1);
        break;
      }
    }
    base += n - 8;
  }
  if (!found) { file.close(); state = DS_ERROR; return; }
  durFrames = (double)estFrames * spf;
  if (dec) MP3FreeDecoder(dec);
  dec = MP3InitDecoder();
  if (!dec) { file.close(); state = DS_ERROR; return; }
  file.seek(dataStart);
  inPtr = inbuf; inLeft = 0; fileEof = false;
  frameNo = 0; decSrc = 0; discard = 0;
  portENTER_CRITICAL(&mux);
  writeT = 0;
  pendingJumpT = -1;
  pubPlayT = 0;
  segHead = segCount = 0;
  addSegment(0, 0);
  resetPlay = true;
  eof = false;
  portEXIT_CRITICAL(&mux);
  state = DS_READY;
  Serial.printf("[deck%d] loaded %s (%d Hz, %u frames)\n", id, p, (int)srcRate, (unsigned)estFrames);
  analyzerEnqueue(id, gen);
}

void Deck::internalSeek(int64_t target) {
  if (target < 0) target = 0;
  int64_t mf = target / spf;
  // start a few frames early so the bit reservoir + overlap are primed
  int back = hdr0.frameBytes > 0 ? 1600 / hdr0.frameBytes + 2 : 10;
  if (back < 3) back = 3;
  if (back > 10) back = 10;
  int64_t want = mf - back;
  if (want < 0) want = 0;
  int64_t startFrame = 0;
  uint32_t off = dataStart;
  bool have = false;
  portENTER_CRITICAL(&mux);
  if (idx && idxCount) {
    uint32_t e = (uint32_t)(want / idxStep);
    if (e >= idxCount) e = idxCount - 1;
    off = idx[e];
    startFrame = (int64_t)e * idxStep;
    have = true;
  }
  portEXIT_CRITICAL(&mux);
  if (!have) {
    double bpf = (double)(fileSize - dataStart) / (estFrames ? estFrames : 1);
    off = dataStart + (uint32_t)(want * bpf);
    if (off >= fileSize) off = fileSize > 2048 ? fileSize - 2048 : dataStart;
    startFrame = want;
  }
  file.seek(off);
  inPtr = inbuf; inLeft = 0; fileEof = false;
  frameNo = startFrame;
  decSrc = startFrame * spf;
  discard = target - decSrc;
  if (discard < 0) discard = 0;
  eof = false;
}

bool Deck::refill() {
  if (fileEof) return false;
  if (inLeft > 0 && inPtr != inbuf) memmove(inbuf, inPtr, inLeft);
  inPtr = inbuf;
  int space = INBUF - inLeft;
  if (space <= 0) return true;
  int n = file.read(inbuf + inLeft, space);
  if (n <= 0) { fileEof = true; return false; }
  inLeft += n;
  return true;
}

void Deck::decodeOne() {
  if (inLeft < 2 * 1441 && !fileEof) refill();
  if (inLeft < 4) { if (fileEof) eof = true; return; }
  int off = MP3FindSyncWord(inPtr, inLeft);
  if (off < 0) {
    if (fileEof) { eof = true; return; }
    inPtr += inLeft - 3; inLeft = 3;
    refill();
    return;
  }
  inPtr += off; inLeft -= off;
  if (inLeft < 4) { if (fileEof) eof = true; else refill(); return; }
  Mp3Hdr h;
  if (!mp3ParseHeader(inPtr, h) || !mp3SameStream(h, hdr0)) { inPtr++; inLeft--; return; }
  if (inLeft < h.frameBytes) {
    if (fileEof) { eof = true; return; }
    refill();
    return;
  }
  uint8_t* fs = inPtr;
  int before = inLeft;
  int rc = MP3Decode(dec, &inPtr, &inLeft, pcm, 0);
  if (rc == ERR_MP3_INDATA_UNDERFLOW) {
    inPtr = fs; inLeft = before;
    if (fileEof) eof = true; else refill();
    return;
  }
  // always step exactly one frame so frame numbers match the index
  inPtr = fs + h.frameBytes;
  inLeft = before - h.frameBytes;
  frameNo++;
  if (rc == ERR_MP3_NONE) {
    MP3FrameInfo fi;
    MP3GetLastFrameInfo(dec, &fi);
    int ch = fi.nChans > 0 ? fi.nChans : 1;
    emit(pcm, fi.outputSamps / ch, ch);
  } else {
    emit(nullptr, spf, 2);  // reservoir underflow / bad frame -> silence
  }
}

void Deck::emit(const int16_t* p, int n, int ch) {
  int start = 0;
  if (discard > 0) {
    int64_t d = discard < n ? discard : n;
    start = (int)d;
    discard -= d;
    decSrc += d;
    if (start >= n) return;
  }
  int cnt = n - start;
  const int16_t* q2 = p ? p + start * ch : nullptr;
  if (loopOn) {
    int64_t le = (int64_t)loopEnd;
    if (decSrc < le && decSrc + cnt >= le) {
      int take = (int)(le - decSrc);
      writeFrames(q2, take, ch);
      decSrc += take;
      int64_t ls = (int64_t)loopStart;
      internalSeek(ls);
      portENTER_CRITICAL(&mux);
      addSegment(writeT, (double)ls);
      portEXIT_CRITICAL(&mux);
      return;
    }
  }
  writeFrames(q2, cnt, ch);
  decSrc += cnt;
}

void Deck::writeFrames(const int16_t* p, int n, int ch) {
  if (n <= 0) return;
  int64_t w = writeT;
  for (int i = 0; i < n; i++) {
    int16_t* dst = ring + ((w + i) & mask) * 2;
    if (!p) { dst[0] = dst[1] = 0; }
    else if (ch == 2) { dst[0] = p[2 * i]; dst[1] = p[2 * i + 1]; }
    else { dst[0] = dst[1] = p[i]; }
  }
  portENTER_CRITICAL(&mux);
  writeT = w + n;
  portEXIT_CRITICAL(&mux);
}
