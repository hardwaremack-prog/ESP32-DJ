// bpm.cpp - see bpm.h
#include "bpm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static void* bigAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(n);
  return p;
}
#else
static void* bigAlloc(size_t n) { return malloc(n); }
#endif

BpmAnalyzer::~BpmAnalyzer() { end(); }

bool BpmAnalyzer::begin(uint32_t sampleRate, int maxSeconds) {
  end();
  rate_ = sampleRate ? sampleRate : 44100;
  hop_ = rate_ / 100;
  cap_ = maxSeconds * 100;
  env_ = (float*)bigAlloc(sizeof(float) * cap_);
  if (!env_) { cap_ = 0; return false; }
  n_ = 0; hopPos_ = 0;
  lpLow_ = lpMid_ = eLow_ = eHigh_ = 0;
  prevLow_ = prevHigh_ = 0;
  aLow_ = 1.0f - expf(-2.0f * (float)M_PI * 150.0f / rate_);
  aMid_ = 1.0f - expf(-2.0f * (float)M_PI * 2500.0f / rate_);
  return true;
}

void BpmAnalyzer::end() {
  free(env_);
  env_ = nullptr;
  cap_ = n_ = 0;
}

void BpmAnalyzer::feed(const int16_t* pcm, int frames, int channels) {
  if (!env_ || n_ >= cap_) return;
  for (int i = 0; i < frames; i++) {
    float m = channels == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) * (0.5f / 32768.0f) : pcm[i] * (1.0f / 32768.0f);
    lpLow_ += aLow_ * (m - lpLow_);
    lpMid_ += aMid_ * (m - lpMid_);
    float hi = m - lpMid_;
    eLow_ += lpLow_ * lpLow_;
    eHigh_ += hi * hi;
    if (++hopPos_ >= hop_) {
      float l = logf(1e-7f + eLow_ / hop_);
      float h = logf(1e-7f + eHigh_ / hop_);
      float o = 0;
      if (n_ > 0) {
        float dl = l - prevLow_, dh = h - prevHigh_;
        o = (dl > 0 ? dl : 0) + 0.6f * (dh > 0 ? dh : 0);
      }
      prevLow_ = l; prevHigh_ = h;
      env_[n_++] = o;
      eLow_ = eHigh_ = 0;
      hopPos_ = 0;
      if (n_ >= cap_) return;
    }
  }
}

float BpmAnalyzer::acfAt(const float* acf, int n, float lag) const {
  int i = (int)lag;
  if (i < 0 || i + 1 >= n) return 0;
  float f = lag - i;
  return acf[i] * (1 - f) + acf[i + 1] * f;
}

bool BpmAnalyzer::compute(float& bpm, double& firstBeat, float& confidence) {
  bpm = 0; firstBeat = 0; confidence = 0;
  int n = n_;
  if (n < 600) return false;  // need at least ~6 s
  const float er60 = 60.0f * (float)rate_ / hop_;  // env frames per minute
  // 1) detrend: subtract local mean, half-wave rectify
  float* x = (float*)bigAlloc(sizeof(float) * n);
  if (!x) return false;
  const int W = 10;
  for (int i = 0; i < n; i++) {
    int a = i - W < 0 ? 0 : i - W, b = i + W >= n ? n - 1 : i + W;
    float s = 0;
    for (int j = a; j <= b; j++) s += env_[j];
    float v = env_[i] - s / (b - a + 1);
    x[i] = v > 0 ? v : 0;
  }
  // 2) autocorrelation up to maxLag
  int maxLag = n / 2;
  if (maxLag > 820) maxLag = 820;
  float* acf = (float*)bigAlloc(sizeof(float) * (maxLag + 2));
  if (!acf) { free(x); return false; }
  for (int L = 0; L <= maxLag + 1; L++) {
    if (L > maxLag) { acf[L] = 0; continue; }
    double s = 0;
    for (int i = 0; i + L < n; i++) s += x[i] * x[i + L];
    acf[L] = (float)(s / (n - L));
  }
  // 3) coarse search with comb + tempo prior (lags 30..125 = 200..48 BPM)
  float bestScore = -1, bestT = 50;
  float sumScore = 0; int cnt = 0;
  for (float T = 30; T <= 125; T += 0.25f) {
    float s = 0; int k = 0;
    for (int m = 1; m * T < maxLag && m <= 8; m++) { s += acfAt(acf, maxLag + 1, m * T); k++; }
    if (!k) continue;
    s /= k;
    float b = er60 / T;
    float oct = log2f(b / 122.0f);
    float w = expf(-0.5f * (oct / 0.55f) * (oct / 0.55f));
    float sc = s * w;
    sumScore += sc; cnt++;
    if (sc > bestScore) { bestScore = sc; bestT = T; }
  }
  // 3b) metrical check: a dotted grouping (3 eighths) can beat the real pulse
  //     when hats sit on the off-beats. Prefer 2/3 of the period if it scores well.
  {
    auto comb = [&](float T) {
      float s = 0; int k = 0;
      for (int m = 1; m * T < maxLag && m <= 8; m++) { s += acfAt(acf, maxLag + 1, m * T); k++; }
      return k ? s / k : 0.0f;
    };
    float T23 = bestT * 2.0f / 3.0f;
    float best = 0, bestTT = T23;
    for (float d = -0.5f; d <= 0.5f; d += 0.25f) {
      float c = comb(T23 + d);
      if (c > best) { best = c; bestTT = T23 + d; }
    }
    if (T23 >= 28 && best >= 0.92f * comb(bestT)) bestT = bestTT;
  }
  // 4) fine refine around bestT using long lags (no prior)
  float fineT = bestT, fineS = -1;
  for (float T = bestT - 0.6f; T <= bestT + 0.6f; T += 0.01f) {
    float s = 0;
    for (int m = 1; m * T < maxLag; m++) s += acfAt(acf, maxLag + 1, m * T) * (1.0f + 0.15f * m);
    if (s > fineS) { fineS = s; fineT = T; }
  }
  float b = er60 / fineT;
  while (b < 78) b *= 2;
  while (b >= 180) b /= 2;
  float r = roundf(b);
  if (fabsf(b - r) < 0.15f) b = r;
  else b = roundf(b * 10) / 10;
  // 5) phase: best offset of a pulse train with this period
  float P = er60 / b;
  float bestPh = 0, bestPs = -1;
  for (float ph = 0; ph < P; ph += 0.1f) {
    float s = 0;
    for (float t = ph; t + 1 < n; t += P) {
      int i = (int)t; float f = t - i;
      s += x[i] * (1 - f) + x[i + 1] * f;
    }
    if (s > bestPs) { bestPs = s; bestPh = ph; }
  }
  bpm = b;
  // onset envelope lags the true attack by ~2 hops; compensate
  double fb = ((double)bestPh - 2.2) * hop_;
  double beatLen = (double)P * hop_;
  while (fb < 0) fb += beatLen;
  firstBeat = fb;
  confidence = cnt ? bestScore / (sumScore / cnt + 1e-9f) : 0;
  free(acf);
  free(x);
  return true;
}
