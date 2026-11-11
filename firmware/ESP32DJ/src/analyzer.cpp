// analyzer.cpp - one pass over the file:
//   * parses every MP3 frame header -> seek index (every IDX_STEP frames) + exact length
//   * decodes the first 40 s -> BpmAnalyzer (skipped if BPM is cached)
#include "analyzer.h"
#include <SD.h>
#include "deck.h"
#include "bpm.h"
#include "library.h"
#include "esp_heap_caps.h"

struct Job { int deck; uint32_t gen; };
static QueueHandle_t jobs;

#define ABUF 8192

static void* allocAny(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(n);
  return p;
}
static void* reallocAny(void* old, size_t n) {
  void* p = heap_caps_realloc(old, n, MALLOC_CAP_SPIRAM);
  if (!p) p = realloc(old, n);
  return p;
}

static void analyse(const Job& j) {
  Deck& d = decks[j.deck];
  if (d.gen != j.gen || d.state != DS_READY) return;
  char path[200];
  strlcpy(path, d.path, sizeof(path));
  float cBpm; double cFirst;
  bool needBpm = !libBpmLookup(path, cBpm, cFirst);
  if (!needBpm) {
    d.bpm = cBpm;
    d.firstBeat = cFirst;
  }
  d.analyzing = true;
  File f = SD.open(path, FILE_READ);
  if (!f) { d.analyzing = false; return; }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(ABUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t* pcm = (int16_t*)heap_caps_malloc(1152 * 2 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  HMP3Decoder dec = needBpm ? MP3InitDecoder() : nullptr;
  BpmAnalyzer* bpm = needBpm ? new BpmAnalyzer() : nullptr;
  if (!buf || !pcm || (needBpm && (!dec || !bpm))) {
    free(buf); free(pcm);
    if (dec) MP3FreeDecoder(dec);
    delete bpm;
    f.close();
    d.analyzing = false;
    return;
  }
  uint32_t fsize = f.size();
  uint32_t cap = 256, count = 0;
  uint32_t* idx = (uint32_t*)allocAny(cap * 4);
  // skip ID3v2
  int n = f.read(buf, 10);
  uint32_t pos = id3v2TagSize(buf, n);
  uint32_t bufStart = pos;  // file offset of buf[0]
  f.seek(pos);
  int have = f.read(buf, ABUF);
  int bp = 0;
  uint32_t frames = 0;
  Mp3Hdr h0 = {};
  bool first = true, bpmStarted = false;
  int sampleRate = 44100;
  uint32_t lastYield = millis();

  while (have > 0) {
    if (d.gen != j.gen) break;  // track changed - abort
    if (have - bp < 1500) {      // refill keeping the tail
      int keep = have - bp;
      memmove(buf, buf + bp, keep);
      bufStart += bp;
      bp = 0;
      int r = f.read(buf + keep, ABUF - keep);
      have = keep + (r > 0 ? r : 0);
      if (r <= 0 && keep < 4) break;
    }
    Mp3Hdr h;
    if (bp + 4 > have) break;
    if (!mp3ParseHeader(buf + bp, h) || (!first && !mp3SameStream(h, h0))) { bp++; continue; }
    if (first) {
      // require a following valid header to avoid false sync
      if (bp + h.frameBytes + 4 <= have) {
        Mp3Hdr h2;
        if (!mp3ParseHeader(buf + bp + h.frameBytes, h2) || !mp3SameStream(h, h2)) { bp++; continue; }
      }
      h0 = h; first = false;
      sampleRate = h.sampleRate;
      if (bpm && !bpm->begin(sampleRate, psramFound() ? 40 : 30)) { delete bpm; bpm = nullptr; }
      bpmStarted = bpm != nullptr;
    }
    if ((frames % d.idxStep) == 0) {
      if (count >= cap) {
        cap *= 2;
        uint32_t* ni = (uint32_t*)reallocAny(idx, cap * 4);
        if (!ni) break;
        idx = ni;
      }
      idx[count++] = bufStart + bp;
    }
    if (bpmStarted && !bpm->full() && bp + h.frameBytes <= have) {
      unsigned char* p = buf + bp;
      int left = have - bp;
      int rc = MP3Decode(dec, &p, &left, pcm, 0);
      if (rc == ERR_MP3_NONE) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        int ch = fi.nChans > 0 ? fi.nChans : 1;
        bpm->feed(pcm, fi.outputSamps / ch, ch);
      }
    }
    frames++;
    bp += h.frameBytes;
    if (millis() - lastYield > 40) { vTaskDelay(1); lastYield = millis(); }
  }
  f.close();
  bool ok = d.gen == j.gen;
  if (ok && count) {
    d.setIndex(idx, count, frames);
    idx = nullptr;
  }
  free(idx);
  if (ok && bpm && bpmStarted) {
    float b, conf; double fb;
    if (bpm->compute(b, fb, conf)) {
      d.bpm = b;
      d.firstBeat = fb;
      if (d.cuePoint == 0) d.cuePoint = 0;
      libBpmStore(path, b, fb);
      Serial.printf("[analyzer] %s: %.2f BPM, first beat %.3fs, conf %.1f\n", path, b, fb / sampleRate, conf);
    }
  }
  delete bpm;
  if (dec) MP3FreeDecoder(dec);
  free(buf);
  free(pcm);
  (void)fsize;
  d.analyzing = false;
}

static void task(void*) {
  Job j;
  for (;;) {
    if (xQueueReceive(jobs, &j, portMAX_DELAY) == pdTRUE) analyse(j);
  }
}

void analyzerStart() {
  jobs = xQueueCreate(8, sizeof(Job));
  xTaskCreatePinnedToCore(task, "analyzer", 6144, nullptr, 2, nullptr, 0);
}

void analyzerEnqueue(int deck, uint32_t gen) {
  if (!jobs) return;
  Job j = {deck, gen};
  xQueueSend(jobs, &j, 0);
}
