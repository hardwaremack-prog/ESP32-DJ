// mixer.cpp - see mixer.h
#include "mixer.h"
#include <math.h>
#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "settings.h"
#include "deck.h"

MixerState mix;

// ---------------------------------------------------------------- biquad
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1[2] = {0, 0}, z2[2] = {0, 0};
  inline float run(float x, int c) {
    float y = b0 * x + z1[c];
    z1[c] = b1 * x - a1 * y + z2[c];
    z2[c] = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
  void lowpass(float f, float q) {
    float w = 2 * (float)M_PI * f / SAMPLE_RATE, cs = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
    b0 = (1 - cs) / 2 / a0; b1 = (1 - cs) / a0; b2 = b0; a1 = -2 * cs / a0; a2 = (1 - al) / a0;
  }
  void highpass(float f, float q) {
    float w = 2 * (float)M_PI * f / SAMPLE_RATE, cs = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
    b0 = (1 + cs) / 2 / a0; b1 = -(1 + cs) / a0; b2 = b0; a1 = -2 * cs / a0; a2 = (1 - al) / a0;
  }
};

struct Strip {
  Biquad lo, hi, flt;
  float gLo = 1, gMid = 1, gHi = 1;
  float fltVal = 0;
  bool fltOn = false;
  float gain = 0;  // smoothed fader gain
};
static Strip strips[NUM_DECKS];

// ---------------------------------------------------------------- FX unit
struct Fx {
  int16_t* echo = nullptr;
  uint32_t echoFrames = 0, echoPos = 0;
  int echoCh = 1;
  float fl[2][512];
  uint32_t flPos = 0;
  float flPhase = 0;
  float crushHold[2] = {0, 0};
  float crushCnt = 0;
  float wetSm = 0;
  float gateSm = 1;
} fx;

static float beatSec(int target) {
  float b = 0;
  if (target < 2) b = decks[target].effBpm();
  else {
    float best = -1;
    for (int d = 0; d < NUM_DECKS; d++)
      if (decks[d].playing && decks[d].bpm > 0) {
        float w = d == 0 ? 1 - mix.xfader : mix.xfader;
        if (w > best) { best = w; b = decks[d].effBpm(); }
      }
  }
  if (b < 40) b = 120;
  return 60.0f / b;
}

static float beatPhaseOf(int target) {
  if (target < 2) return (float)decks[target].beatPhase();
  int d = mix.xfader < 0.5f ? 0 : 1;
  if (!decks[d].playing) d = 1 - d;
  return (float)decks[d].beatPhase();
}

static void fxProcess(float* L, float* R, int n, int target) {
  bool on = mix.fxOn;
  float amt = mix.fxAmount;
  int type = mix.fxType;
  if (type == FX_ECHO && fx.echo) {
    // keep running after switch-off so the tail rings out
    uint32_t dly = (uint32_t)(beatSec(target) * mix.fxBeats * SAMPLE_RATE);
    while (dly >= fx.echoFrames && dly > 1) dly /= 2;
    if (dly < 64) dly = 64;
    float fb = 0.35f + 0.5f * amt;
    float wet = 0.4f + 0.6f * amt;
    for (int i = 0; i < n; i++) {
      uint32_t rp = (fx.echoPos + fx.echoFrames - dly) % fx.echoFrames;
      float dl, dr;
      if (fx.echoCh == 2) { dl = fx.echo[rp * 2] / 32768.0f; dr = fx.echo[rp * 2 + 1] / 32768.0f; }
      else { dl = dr = fx.echo[rp] / 32768.0f; }
      float inL = on ? L[i] : 0, inR = on ? R[i] : 0;
      float wl = inL + dl * fb, wr = inR + dr * fb;
      // soft-limit the feedback path, slight darkening
      wl = wl > 1 ? 1 : (wl < -1 ? -1 : wl);
      wr = wr > 1 ? 1 : (wr < -1 ? -1 : wr);
      if (fx.echoCh == 2) { fx.echo[fx.echoPos * 2] = (int16_t)(wl * 32000); fx.echo[fx.echoPos * 2 + 1] = (int16_t)(wr * 32000); }
      else fx.echo[fx.echoPos] = (int16_t)((wl + wr) * 16000);
      fx.echoPos = (fx.echoPos + 1) % fx.echoFrames;
      L[i] += dl * wet;
      R[i] += dr * wet;
    }
    return;
  }
  if (!on) return;
  if (type == FX_FLANGER) {
    float rate = 1.0f / (beatSec(target) * 8);  // one sweep per 8 beats
    for (int i = 0; i < n; i++) {
      fx.flPhase += rate / SAMPLE_RATE;
      if (fx.flPhase >= 1) fx.flPhase -= 1;
      float lfo = 0.5f - 0.5f * cosf(2 * (float)M_PI * fx.flPhase);
      float d = 20 + lfo * 300;  // 0.45 .. 7 ms
      float rp = (float)fx.flPos - d;
      if (rp < 0) rp += 512;
      int i0 = (int)rp;
      float f = rp - i0;
      int i1 = (i0 + 1) & 511;
      float dl = fx.fl[0][i0] * (1 - f) + fx.fl[0][i1] * f;
      float dr = fx.fl[1][i0] * (1 - f) + fx.fl[1][i1] * f;
      fx.fl[0][fx.flPos] = L[i] + dl * 0.6f;
      fx.fl[1][fx.flPos] = R[i] + dr * 0.6f;
      fx.flPos = (fx.flPos + 1) & 511;
      L[i] = L[i] * (1 - amt * 0.5f) + dl * amt;
      R[i] = R[i] * (1 - amt * 0.5f) + dr * amt;
    }
  } else if (type == FX_CRUSH) {
    float hold = 1 + amt * 14;               // sample-rate reduction
    float steps = powf(2.0f, 12 - amt * 9);  // 12 -> 3 bits
    for (int i = 0; i < n; i++) {
      fx.crushCnt += 1;
      if (fx.crushCnt >= hold) {
        fx.crushCnt -= hold;
        fx.crushHold[0] = roundf(L[i] * steps) / steps;
        fx.crushHold[1] = roundf(R[i] * steps) / steps;
      }
      L[i] = fx.crushHold[0];
      R[i] = fx.crushHold[1];
    }
  } else if (type == FX_GATE) {
    float ph = beatPhaseOf(target);
    float div = mix.fxBeats > 0 ? mix.fxBeats : 0.25f;
    float bs = beatSec(target);
    float adv = 1.0f / (bs * SAMPLE_RATE);
    for (int i = 0; i < n; i++) {
      float x = (ph + adv * i) / div;
      float frac = x - floorf(x);
      float target2 = frac < 0.5f ? 1.0f : 1.0f - amt;
      fx.gateSm += (target2 - fx.gateSm) * 0.02f;
      L[i] *= fx.gateSm;
      R[i] *= fx.gateSm;
    }
  }
}

// ---------------------------------------------------------------- pads
struct Voice {
  bool on = false;
  int type = 0;
  uint32_t t = 0;
  float p1 = 0, p2 = 0, p3 = 0, lp = 0, lp2 = 0;
  uint32_t seed = 1;
};
#define VOICES 4
static Voice voices[VOICES];
static volatile int padReq = -1;
static const char* PAD_NAMES[NUM_PADS] = {"Air Horn", "Siren", "Laser", "Boom", "Riser", "Clap", "808 Kick", "Rewind"};
const char* padName(int i) { return (i >= 0 && i < NUM_PADS) ? PAD_NAMES[i] : ""; }

void padTrigger(int i) { if (i >= 0 && i < NUM_PADS) padReq = i; }

static inline float noiseF(uint32_t& s) {
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return (int32_t)s * (1.0f / 2147483648.0f);
}
static inline float saw(float& ph, float f) {
  ph += f / SAMPLE_RATE;
  if (ph >= 1) ph -= 1;
  return 2 * ph - 1;
}
static inline float sine(float& ph, float f) {
  ph += f / SAMPLE_RATE;
  if (ph >= 1) ph -= 1;
  return sinf(2 * (float)M_PI * ph);
}

static float voiceSample(Voice& v) {
  float s = v.t / (float)SAMPLE_RATE;
  float out = 0;
  switch (v.type) {
    case 0: {  // air horn: short-short-long
      float g = (s < 0.11f) || (s > 0.15f && s < 0.26f) || (s > 0.30f && s < 0.95f) ? 1 : 0;
      float f = 415 * (1 - 0.04f * s);
      float x = saw(v.p1, f) + saw(v.p2, f * 1.007f) + 0.8f * saw(v.p3, f * 1.5f);
      v.lp += (x - v.lp) * 0.35f;
      v.lp2 += (g - v.lp2) * 0.004f;
      out = tanhf(v.lp * 1.4f) * 0.35f * v.lp2;
      if (s > 1.0f) v.on = false;
      break;
    }
    case 1: {  // siren
      float f = 750 + 380 * sinf(2 * (float)M_PI * 2.2f * s);
      float x = tanhf(3 * sine(v.p1, f));
      float env = s < 1.7f ? 1 : (2.0f - s) / 0.3f;
      out = x * 0.22f * env;
      if (s > 2.0f) v.on = false;
      break;
    }
    case 2: {  // laser x3
      float ls = fmodf(s, 0.2f);
      float f = 2600 * expf(-ls * 18) + 160;
      out = sine(v.p1, f) * expf(-ls * 11) * 0.35f;
      if (s > 0.6f) v.on = false;
      break;
    }
    case 3: {  // boom / sub drop
      float f = 32 + 90 * expf(-s * 6);
      float click = s < 0.006f ? noiseF(v.seed) * (1 - s / 0.006f) : 0;
      out = (tanhf(1.5f * sine(v.p1, f)) * expf(-s * 1.6f) + click * 0.5f) * 0.8f;
      if (s > 2.6f) v.on = false;
      break;
    }
    case 4: {  // riser
      float k = s / 3.0f;
      float fc = 200 * powf(40.0f, k);
      float a = 1 - expf(-2 * (float)M_PI * fc / SAMPLE_RATE);
      float nz = noiseF(v.seed);
      v.lp += (nz - v.lp) * a;
      float tone = sine(v.p1, 200 * powf(8.0f, k)) * 0.3f;
      out = (v.lp * 0.8f + tone) * k * 0.45f;
      if (s > 3.0f) v.on = false;
      break;
    }
    case 5: {  // clap
      float nz = noiseF(v.seed);
      v.lp += (nz - v.lp) * 0.2f;
      v.lp2 += (v.lp - v.lp2) * 0.05f;
      float bp = v.lp - v.lp2;
      float env = 0;
      for (int b = 0; b < 3; b++) {
        float st = b * 0.011f;
        if (s >= st) env = fmaxf(env, expf(-(s - st) * 250));
      }
      if (s > 0.022f) env = fmaxf(env, expf(-(s - 0.022f) * 18) * 0.8f);
      out = bp * env * 1.6f;
      if (s > 0.4f) v.on = false;
      break;
    }
    case 6: {  // 808 kick
      float f = 48 + 140 * expf(-s * 28);
      out = tanhf(2 * sine(v.p1, f) * expf(-s * 3.5f)) * 0.8f;
      if (s > 1.2f) v.on = false;
      break;
    }
    case 7: {  // rewind
      float f = 900 * expf(-s * 2.2f) * (1 + 0.25f * sinf(2 * (float)M_PI * 14 * s));
      float x = saw(v.p1, f);
      float nz = noiseF(v.seed);
      v.lp += (x * 0.6f + nz * 0.4f - v.lp) * 0.25f;
      float env = s < 0.05f ? s / 0.05f : (s > 0.8f ? (1.0f - s) / 0.2f : 1);
      out = v.lp * env * 0.4f;
      if (s > 1.0f) v.on = false;
      break;
    }
  }
  v.t++;
  return out;
}

// ---------------------------------------------------------------- outputs
static i2s_chan_handle_t tx1 = nullptr, tx2 = nullptr;

static bool validPin(int p) { return p >= 0 && p <= 48; }

static i2s_chan_handle_t makeI2S(i2s_port_t port, int bck, int ws, int dout) {
  if (!validPin(bck) || !validPin(ws) || !validPin(dout)) return nullptr;
  i2s_chan_handle_t h = nullptr;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = 240;
  cc.auto_clear = true;
  if (i2s_new_channel(&cc, &h, nullptr) != ESP_OK) return nullptr;
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)bck,
          .ws = (gpio_num_t)ws,
          .dout = (gpio_num_t)dout,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {false, false, false},
      },
  };
  if (i2s_channel_init_std_mode(h, &sc) != ESP_OK || i2s_channel_enable(h) != ESP_OK) {
    i2s_del_channel(h);
    return nullptr;
  }
  return h;
}

// ---------------------------------------------------------------- engine
static float bufL[NUM_DECKS][BLOCK], bufR[NUM_DECKS][BLOCK];
static float mL[BLOCK], mR[BLOCK], cL[BLOCK], cR[BLOCK];
static int16_t out1[BLOCK * 2], out2[BLOCK * 2];
static float limGain = 1;

static inline float faderCurve(float v) { return v * v; }

static void xfGains(float x, float& a, float& b) {
  switch (cfg.xfCurve) {
    case 1:  // sharp (scratch)
      a = fminf(1, (1 - x) * 16); b = fminf(1, x * 16); break;
    case 2:  // linear
      a = 1 - x; b = x; break;
    default:  // constant power
      a = cosf(x * (float)M_PI / 2); b = sinf(x * (float)M_PI / 2); break;
  }
}

static void updateStripFilters(Strip& s, Deck& d) {
  float f = d.filter;
  if (fabsf(f - s.fltVal) > 0.0005f) {
    s.fltVal = f;
    if (fabsf(f) <= 0.02f) {
      s.fltOn = false;
    } else {
      if (!s.fltOn) s.flt.reset();
      s.fltOn = true;
      float a = (fabsf(f) - 0.02f) / 0.98f;
      if (f < 0) s.flt.lowpass(18000.0f * powf(70.0f / 18000.0f, a), 1.1f);
      else s.flt.highpass(25.0f * powf(7000.0f / 25.0f, a), 1.1f);
    }
  }
}

static void renderBlock() {
  // pad trigger
  int pr = padReq;
  if (pr >= 0) {
    padReq = -1;
    int slot = 0;
    for (int i = 0; i < VOICES; i++) if (!voices[i].on) { slot = i; break; }
    Voice& v = voices[slot];
    v = Voice();
    v.on = true;
    v.type = pr;
    v.seed = esp_random() | 1;
  }
  float ga, gb;
  xfGains(mix.xfader, ga, gb);
  memset(mL, 0, sizeof(mL)); memset(mR, 0, sizeof(mR));
  memset(cL, 0, sizeof(cL)); memset(cR, 0, sizeof(cR));
  bool anyPfl = false;
  for (int d = 0; d < NUM_DECKS; d++) {
    Deck& dk = decks[d];
    Strip& s = strips[d];
    float* L = bufL[d];
    float* R = bufR[d];
    dk.render(L, R, BLOCK);
    updateStripFilters(s, dk);
    float tLo = dk.eqLo, tMid = dk.eqMid, tHi = dk.eqHi, trim = dk.trim;
    float target = faderCurve(dk.volume) * (d == 0 ? ga : gb);
    float g0 = s.gain, dg = (target - g0) / BLOCK;
    float lo0 = s.gLo, dlo = (tLo - lo0) / BLOCK;
    float mi0 = s.gMid, dmi = (tMid - mi0) / BLOCK;
    float hi0 = s.gHi, dhi = (tHi - hi0) / BLOCK;
    bool pfl = dk.pfl;
    anyPfl |= pfl;
    float peak = 0;
    for (int i = 0; i < BLOCK; i++) {
      float gl = lo0 + dlo * i, gm = mi0 + dmi * i, gh = hi0 + dhi * i;
      for (int c = 0; c < 2; c++) {
        float x = (c ? R[i] : L[i]) * trim;
        float lo = s.lo.run(x, c);
        float hi = s.hi.run(x, c);
        float mid = x - lo - hi;
        float y = lo * gl + mid * gm + hi * gh;
        if (s.fltOn) y = s.flt.run(y, c);
        if (c) R[i] = y; else L[i] = y;
      }
      if (pfl) { cL[i] += L[i]; cR[i] += R[i]; }
      float g = g0 + dg * i;
      L[i] *= g; R[i] *= g;
    }
    s.gain = target; s.gLo = tLo; s.gMid = tMid; s.gHi = tHi;
    if (mix.fxTarget == d) fxProcess(L, R, BLOCK, d);
    for (int i = 0; i < BLOCK; i++) {
      mL[i] += L[i]; mR[i] += R[i];
      float a = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);
      if (a > peak) peak = a;
    }
    float v = mix.vu[d] * 0.92f;
    mix.vu[d] = peak > v ? peak : v;
  }
  if (mix.fxTarget == 2) fxProcess(mL, mR, BLOCK, 2);
  // pads
  float pv = mix.padVol;
  for (int k = 0; k < VOICES; k++) {
    Voice& v = voices[k];
    if (!v.on) continue;
    for (int i = 0; i < BLOCK && v.on; i++) {
      float x = voiceSample(v) * pv;
      mL[i] += x; mR[i] += x;
    }
  }
  // master volume + limiter
  float mv = mix.masterVol;
  float peak = 0;
  for (int i = 0; i < BLOCK; i++) {
    float l = mL[i] * mv, r = mR[i] * mv;
    float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
    if (a * limGain > 0.94f) limGain = 0.94f / a;
    else limGain += (1 - limGain) * 0.00015f;
    l *= limGain; r *= limGain;
    mL[i] = l; mR[i] = r;
    if (a * limGain > peak) peak = a * limGain;
  }
  float v = mix.vu[2] * 0.92f;
  mix.vu[2] = peak > v ? peak : v;
  // headphone bus
  float hm = mix.hpMix, hv = mix.hpVol;
  for (int i = 0; i < BLOCK; i++) {
    float cl = anyPfl ? cL[i] : 0, cr = anyPfl ? cR[i] : 0;
    cL[i] = (cl * (1 - hm) + mL[i] * hm) * hv;
    cR[i] = (cr * (1 - hm) + mR[i] * hm) * hv;
  }
  auto s16 = [](float x) -> int16_t {
    if (x > 1) x = 1;
    if (x < -1) x = -1;
    return (int16_t)(x * 32767);
  };
  bool split = cfg.splitCue && !tx2;
  for (int i = 0; i < BLOCK; i++) {
    if (split) {
      out1[2 * i] = s16((cL[i] + cR[i]) * 0.5f);
      out1[2 * i + 1] = s16((mL[i] + mR[i]) * 0.5f);
    } else {
      out1[2 * i] = s16(mL[i]);
      out1[2 * i + 1] = s16(mR[i]);
    }
    out2[2 * i] = s16(cL[i]);
    out2[2 * i + 1] = s16(cR[i]);
  }
}

static void audioTask(void*) {
  const int64_t blockUs = (int64_t)BLOCK * 1000000 / SAMPLE_RATE;
  int64_t next = esp_timer_get_time();
  float load = 0;
  for (;;) {
    int64_t t0 = esp_timer_get_time();
    renderBlock();
    int64_t t1 = esp_timer_get_time();
    load += ((float)(t1 - t0) / blockUs - load) * 0.02f;
    mix.cpu = load;
    size_t w;
    if (tx1) {
      if (i2s_channel_write(tx1, out1, sizeof(out1), &w, 100) != ESP_OK) mix.underruns++;
      if (tx2) i2s_channel_write(tx2, out2, sizeof(out2), &w, 100);
    } else {
      next += blockUs;
      int64_t now = esp_timer_get_time();
      if (next > now) vTaskDelay(pdMS_TO_TICKS((next - now) / 1000 + 1));
      else next = now;
    }
  }
}

void mixerStart(bool psram) {
  for (int d = 0; d < NUM_DECKS; d++) {
    strips[d].lo.lowpass(300, 0.707f);
    strips[d].hi.highpass(3000, 0.707f);
  }
  // echo buffer: stereo 2.5 s in PSRAM, otherwise mono ~0.75 s internal
  if (psram) {
    fx.echoFrames = SAMPLE_RATE * 5 / 2;
    fx.echo = (int16_t*)heap_caps_calloc(fx.echoFrames * 2, 2, MALLOC_CAP_SPIRAM);
    fx.echoCh = 2;
  }
  if (!fx.echo) {
    fx.echoCh = 1;
    fx.echoFrames = SAMPLE_RATE * 3 / 4;
    while (fx.echoFrames > 4096 && !fx.echo) {
      if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) > fx.echoFrames * 2 + 110000)
        fx.echo = (int16_t*)heap_caps_calloc(fx.echoFrames, 2, MALLOC_CAP_INTERNAL);
      if (!fx.echo) fx.echoFrames /= 2;
    }
  }
  memset(fx.fl, 0, sizeof(fx.fl));
  tx1 = makeI2S(I2S_NUM_0, cfg.i2sBck, cfg.i2sWs, cfg.i2sDout);
  if (cfg.dac2) tx2 = makeI2S(I2S_NUM_1, cfg.i2s2Bck, cfg.i2s2Ws, cfg.i2s2Dout);
  mix.out1 = tx1 != nullptr;
  mix.out2 = tx2 != nullptr;
  Serial.printf("[mixer] I2S main %s, headphones %s, echo %u frames x%d\n", tx1 ? "ok" : "OFF",
                tx2 ? "ok" : "off", (unsigned)fx.echoFrames, fx.echoCh);
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, configMAX_PRIORITIES - 3, nullptr, 1);
}
