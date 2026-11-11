// control.cpp - see control.h
#include "control.h"
#include <math.h>
#include "deck.h"
#include "mixer.h"
#include "library.h"
#include "settings.h"

volatile bool autoDj = false;
volatile bool quantize = true;
static volatile uint32_t lastJog[NUM_DECKS] = {0, 0};
static uint32_t taps[NUM_DECKS][8];
static int tapN[NUM_DECKS] = {0, 0};
static SemaphoreHandle_t cmdLock;

// ----------------------------------------------------------- helpers
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static void jsonEsc(String& o, const char* s) {
  o += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  o += '"';
}

static int other(int d) { return 1 - d; }

static float syncRatio(Deck& d, Deck& o) {
  float r = o.effBpm() / d.bpm;
  if (r > 1.5f) r /= 2;
  else if (r < 0.75f) r *= 2;
  return r;
}

// make deck d's beat phase match the other deck right now (used on play)
static void phaseSnap(int d) {
  Deck& a = decks[d];
  Deck& b = decks[other(d)];
  if (!b.playing || a.bpm <= 0 || b.bpm <= 0) return;
  float r = syncRatio(a, b);
  if (fabsf(r - (b.effBpm() / a.bpm)) > 0.01f) return;  // only for 1:1 tempo
  double err = b.beatPhase() - a.beatPhase();
  err -= floor(err + 0.5);
  if (fabs(err) < 0.01) return;
  a.seek(a.posSrc() + err * a.beatFrames());
}

// turn sync off but keep the current synced tempo on the pitch fader
static void syncOff(Deck& k) {
  if (!k.syncOn) return;
  float sp = k.syncPitch;
  if (k.pitchRange < fabsf(sp)) k.pitchRange = fminf(1.0f, fabsf(sp) * 1.25f);
  k.pitchFader = clampf(sp / (k.pitchRange > 0 ? k.pitchRange : 0.08f), -1, 1);
  k.syncOn = false;
  k.phaseAdj = 0;
}

static void doPlay(int d, int want) {
  Deck& k = decks[d];
  if (k.state != DS_READY) return;
  bool on = want < 0 ? !k.playing : want != 0;
  if (on) {
    if (k.atEnd) { k.seek(k.cuePoint); k.atEnd = false; }
    if (k.syncOn) phaseSnap(d);
    k.cuePreview = false;
    k.braking = false;
    k.playing = true;
  } else {
    k.playing = false;
  }
}

static void doCue(int d, bool down) {
  Deck& k = decks[d];
  if (k.state != DS_READY) return;
  if (down) {
    if (k.playing && !k.cuePreview) {
      k.playing = false;
      k.seek(k.cuePoint);
    } else {
      double pos = k.posSrc();
      if (fabs(pos - k.cuePoint) > k.srcRate * 0.02) {
        k.cuePoint = quantize ? k.nearestBeat(pos, false) : pos;
        if (quantize && fabs(k.cuePoint - pos) > 64) k.seek(k.cuePoint);
      }
      k.cuePreview = true;
      k.playing = true;
    }
  } else if (k.cuePreview) {
    k.cuePreview = false;
    k.playing = false;
    k.seek(k.cuePoint);
  }
}

static void doLoop(int d, float beats) {
  Deck& k = decks[d];
  if (k.state != DS_READY || beats <= 0) return;
  double bf = k.beatFrames();
  double pos = k.posSrc();
  double start;
  if (k.loopOn && pos >= k.loopStart && pos < k.loopEnd) start = k.loopStart;  // resize
  else if (quantize && k.bpm > 0) {
    double unit = bf * (beats < 1 ? beats : 1);
    start = k.firstBeat + floor((pos - k.firstBeat) / unit + 0.001) * unit;
    if (start < 0) start = 0;
  } else start = pos;
  k.loopBeats = beats;
  k.setLoop(start, start + beats * bf);
}

static void doTap(int d) {
  Deck& k = decks[d];
  uint32_t now = millis();
  int& n = tapN[d];
  if (n > 0 && now - taps[d][(n - 1) % 8] > 2000) n = 0;
  taps[d][n % 8] = now;
  n++;
  if (n >= 3) {
    int m = n < 8 ? n : 8;
    uint32_t first = taps[d][(n - m) % 8], last = taps[d][(n - 1) % 8];
    float iv = (last - first) / (float)(m - 1);
    if (iv > 200 && iv < 2000) {
      // tapped tempo is the *played* tempo; store original tempo
      float played = 60000.0f / iv;
      float t = k.tempo();
      k.bpm = roundf(played / (t > 0.1f ? t : 1) * 10) / 10;
      double bf = k.beatFrames();
      double p = k.posSrc();
      k.firstBeat = fmod(p, bf);
    }
  }
}

static String rest(const char* s, int skipTokens) {
  const char* p = s;
  for (int i = 0; i < skipTokens; i++) {
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
  }
  return String(p);
}

// ----------------------------------------------------------- commands
static void command(const char* line) {
  char cmd[12] = {0};
  int d = -1;
  float v = 0;
  char arg[16] = {0};
  int n = sscanf(line, "%11s %d %15s", cmd, &d, arg);
  if (n < 1) return;
  v = atof(arg);
  bool hasD = n >= 2 && d >= 0 && d < NUM_DECKS;
  Deck* k = hasD ? &decks[d] : nullptr;
  float gv = n >= 2 ? (float)d : 0;  // for global commands the 2nd token is the value
  if (n >= 2) gv = atof(rest(line, 1).c_str());
  String c = cmd;

  // ---- global ----
  if (c == "xf") { mix.xfader = clampf(gv, 0, 1); return; }
  if (c == "mvol") { mix.masterVol = clampf(gv, 0, 1.5f); return; }
  if (c == "hpmix") { mix.hpMix = clampf(gv, 0, 1); return; }
  if (c == "hpvol") { mix.hpVol = clampf(gv, 0, 1.5f); return; }
  if (c == "padvol") { mix.padVol = clampf(gv, 0, 1.5f); return; }
  if (c == "pad") { padTrigger((int)gv); return; }
  if (c == "fx") { mix.fxType = constrain((int)gv, 0, FX_COUNT - 1); return; }
  if (c == "fxt") { mix.fxTarget = constrain((int)gv, 0, 2); return; }
  if (c == "fxa") { mix.fxAmount = clampf(gv, 0, 1); return; }
  if (c == "fxb") { mix.fxBeats = clampf(gv, 0.0625f, 4); return; }
  if (c == "fxon") { mix.fxOn = gv != 0; return; }
  if (c == "adj") { autoDj = gv != 0; return; }
  if (c == "quant") { quantize = gv != 0; return; }
  if (c == "q+") { queueAdd(rest(line, 1).c_str()); return; }
  if (c == "q-") { queueRemove((int)gv); return; }
  if (c == "qc") { queueClear(); return; }
  if (c == "rescan") { libRescan(); return; }

  if (!k) return;
  // ---- deck ----
  if (c == "load") {
    String p = rest(line, 2);
    if (p.length()) { k->load(p.c_str()); libMarkPlayed(p.c_str()); }
  } else if (c == "eject") k->unload();
  else if (c == "play") doPlay(d, n >= 3 ? (int)v : -1);
  else if (c == "cue") doCue(d, v != 0);
  else if (c == "sync") {
    bool want = n >= 3 ? v != 0 : !k->syncOn;
    if (!want) syncOff(*k);
    else {
      k->syncOn = true;
      Deck& o = decks[other(d)];
      if (o.state == DS_READY && o.bpm > 0 && k->bpm > 0) k->syncPitch = syncRatio(*k, o) - 1;
      else k->syncPitch = k->pitchFader * k->pitchRange;
      if (k->playing) phaseSnap(d);
    }
  }
  else if (c == "vol") k->volume = clampf(v, 0, 1);
  else if (c == "trim") k->trim = clampf(v, 0, 2.5f);
  else if (c == "lo") k->eqLo = clampf(v, 0, 2);
  else if (c == "mid") k->eqMid = clampf(v, 0, 2);
  else if (c == "hi") k->eqHi = clampf(v, 0, 2);
  else if (c == "flt") k->filter = clampf(v, -1, 1);
  else if (c == "pfl") k->pfl = v != 0;
  else if (c == "pitch") { k->pitchFader = clampf(v, -1, 1); k->syncOn = false; k->phaseAdj = 0; }
  else if (c == "range") k->pitchRange = clampf(v / 100.0f, 0.02f, 1.0f);
  else if (c == "jog") { k->nudge = clampf(v, -0.6f, 0.6f); lastJog[d] = millis(); }
  else if (c == "scr") { k->scratching = v != 0; k->jogVel = 0; }
  else if (c == "sv") { k->jogVel = clampf(v, -4, 4); }
  else if (c == "seek") { if (k->durFrames > 0) k->seek(clampf(v, 0, 0.999f) * k->durFrames); }
  else if (c == "hc") {
    int i = (int)v;
    if (i < 0 || i >= HOTCUES || k->state != DS_READY) return;
    if (k->hot[i] < 0) {
      double p = k->posSrc();
      k->hot[i] = quantize ? k->nearestBeat(p, false) : p;
    } else {
      k->seek(k->hot[i]);
      if (k->loopOn && (k->hot[i] < k->loopStart || k->hot[i] >= k->loopEnd)) k->exitLoop();
      if (!k->playing && !k->cuePreview) k->cuePoint = k->hot[i];
    }
  }
  else if (c == "hcdel") { int i = (int)v; if (i >= 0 && i < HOTCUES) k->hot[i] = -1; }
  else if (c == "loop") doLoop(d, v);
  else if (c == "loopx") k->exitLoop();
  else if (c == "loophalf") { if (k->loopOn && k->loopBeats > 0.126f) doLoop(d, k->loopBeats / 2); }
  else if (c == "loopdbl") { if (k->loopOn && k->loopBeats < 32) doLoop(d, k->loopBeats * 2); }
  else if (c == "jump") {
    double off = v * k->beatFrames();
    if (k->loopOn) { k->loopStart += off; k->loopEnd += off; k->setLoop(k->loopStart, k->loopEnd); }
    k->seek(k->posSrc() + off);
  }
  else if (c == "brake") { if (k->playing) { k->braking = true; } }
  else if (c == "tap") doTap(d);
  else if (c == "bpm") { if (v > 30 && v < 300) k->bpm = v; }
  else if (c == "bpmx") { if (k->bpm > 0 && v > 0) k->bpm = clampf(k->bpm * v, 30, 300); }
  else if (c == "grid") { double bf = k->beatFrames(); k->firstBeat = fmod(k->posSrc(), bf); }
  else if (c == "save") { if (k->bpm > 0) libBpmStore(k->path, k->bpm, k->firstBeat); }
}

void controlCommand(const char* text) {
  xSemaphoreTake(cmdLock, portMAX_DELAY);
  char line[240];
  const char* p = text;
  while (*p) {
    const char* e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    if (len) command(line);
    if (!e) break;
    p = e + 1;
  }
  xSemaphoreGive(cmdLock);
}

// ----------------------------------------------------------- status
static void f2(String& o, const char* k, double v, int dec = 2) {
  o += '"'; o += k; o += "\":"; o += String(v, dec); o += ',';
}
static void fb(String& o, const char* k, bool v) {
  o += '"'; o += k; o += "\":"; o += v ? "true" : "false"; o += ',';
}

String statusJson() {
  String o;
  o.reserve(1800);
  o += "{\"d\":[";
  for (int d = 0; d < NUM_DECKS; d++) {
    Deck& k = decks[d];
    if (d) o += ',';
    o += "{\"st\":"; o += (int)k.state; o += ',';
    o += "\"ti\":"; jsonEsc(o, k.title); o += ',';
    o += "\"ar\":"; jsonEsc(o, k.artist); o += ',';
    o += "\"p\":"; jsonEsc(o, k.path); o += ',';
    double sr = k.srcRate ? k.srcRate : 44100;
    f2(o, "pos", k.state == DS_READY ? k.posSec() : 0, 3);
    f2(o, "dur", k.durSec(), 2);
    fb(o, "pl", k.playing);
    fb(o, "cp", k.cuePreview);
    f2(o, "bpm", k.bpm, 2);
    f2(o, "eb", k.effBpm(), 2);
    float pitch = k.syncOn ? k.syncPitch : k.pitchFader * k.pitchRange;
    f2(o, "pi", pitch * 100, 2);
    f2(o, "pf", k.pitchFader, 3);
    f2(o, "rg", k.pitchRange * 100, 0);
    fb(o, "sy", k.syncOn);
    fb(o, "lo", k.loopOn);
    f2(o, "ls", k.loopStart / sr, 3);
    f2(o, "le", k.loopEnd / sr, 3);
    f2(o, "lb", k.loopBeats, 3);
    o += "\"hc\":[";
    for (int i = 0; i < HOTCUES; i++) {
      if (i) o += ',';
      o += k.hot[i] < 0 ? String("-1") : String(k.hot[i] / sr, 3);
    }
    o += "],";
    f2(o, "cue", k.cuePoint / sr, 3);
    f2(o, "fb", k.firstBeat / sr, 4);
    fb(o, "pfl", k.pfl);
    fb(o, "an", k.analyzing);
    fb(o, "ex", k.exactIndex);
    f2(o, "ph", k.state == DS_READY ? k.beatPhase() : 0, 3);
    f2(o, "sp", k.speedNow, 3);
    fb(o, "end", k.atEnd);
    f2(o, "vol", k.volume, 3);
    f2(o, "tr", k.trim, 3);
    f2(o, "eql", k.eqLo, 3);
    f2(o, "eqm", k.eqMid, 3);
    f2(o, "eqh", k.eqHi, 3);
    f2(o, "fl", k.filter, 3);
    f2(o, "vu", mix.vu[d], 3);
    o += "\"id\":"; o += d; o += '}';
  }
  o += "],";
  f2(o, "xf", mix.xfader, 3);
  f2(o, "mv", mix.masterVol, 3);
  f2(o, "hm", mix.hpMix, 3);
  f2(o, "hv", mix.hpVol, 3);
  f2(o, "pv", mix.padVol, 3);
  f2(o, "vum", mix.vu[2], 3);
  f2(o, "cpu", mix.cpu * 100, 0);
  o += "\"fx\":{\"y\":"; o += mix.fxType; o += ",\"t\":"; o += mix.fxTarget;
  o += ",\"a\":"; o += String(mix.fxAmount, 2); o += ",\"b\":"; o += String(mix.fxBeats, 4);
  o += ",\"on\":"; o += mix.fxOn ? "true" : "false"; o += "},";
  fb(o, "adj", autoDj);
  fb(o, "qz", quantize);
  fb(o, "o1", mix.out1);
  fb(o, "o2", mix.out2);
  o += "\"lv\":"; o += libVersion(); o += ',';
  o += "\"nq\":"; o += queueCount(); o += ',';
  o += "\"heap\":"; o += (uint32_t)(ESP.getFreeHeap() / 1024); o += '}';
  return o;
}

// ----------------------------------------------------------- service task
static struct {
  bool fading = false;
  int from = 0, to = 1;
  uint32_t start = 0, dur = 0;
  float xfFrom = 0, xfTo = 1;
  bool nextReady[NUM_DECKS] = {false, false};
  uint32_t lastCheck = 0;
} adj;

static bool loadNext(int d) {
  char p[200];
  if (!queuePop(p, sizeof(p))) {
    const char* r = libRandom();
    if (!r) return false;
    strlcpy(p, r, sizeof(p));
  }
  Deck& k = decks[d];
  k.load(p);
  libMarkPlayed(p);
  k.pitchFader = 0;
  k.syncOn = false;
  k.volume = 0.8f;
  k.eqLo = k.eqMid = k.eqHi = 1;
  k.filter = 0;
  adj.nextReady[d] = true;
  return true;
}

static void autoDjStep() {
  uint32_t now = millis();
  if (adj.fading) {
    float p = (now - adj.start) / (float)adj.dur;
    if (p > 1) p = 1;
    mix.xfader = adj.xfFrom + (adj.xfTo - adj.xfFrom) * p;
    // bass swap at the halfway point
    Deck& in = decks[adj.to];
    Deck& out = decks[adj.from];
    in.eqLo = p < 0.5f ? 0.15f : 1.0f;
    out.eqLo = p < 0.5f ? 1.0f : 0.15f;
    if (p >= 1) {
      out.playing = false;
      out.eqLo = 1;
      out.syncOn = false;
      syncOff(in);
      // keep the incoming deck's tempo where it is
      adj.fading = false;
      adj.nextReady[adj.from] = false;
    }
    return;
  }
  if (now - adj.lastCheck < 200) return;
  adj.lastCheck = now;
  int active = mix.xfader < 0.5f ? 0 : 1;
  if (!decks[active].playing && decks[other(active)].playing) active = other(active);
  Deck& a = decks[active];
  if (!a.playing) {
    // nothing playing: start a track on A
    Deck& s = decks[0];
    if (s.state == DS_READY && !s.atEnd && s.posSec() < 1) {
      s.playing = true;
      mix.xfader = 0;
    } else if (s.state != DS_LOADING) {
      loadNext(0);
    }
    return;
  }
  int o = other(active);
  Deck& b = decks[o];
  float bpm = a.effBpm() > 40 ? a.effBpm() : 120;
  float fadeSec = clampf(cfg.fadeBeats * 60.0f / bpm, 3, 40);
  float remaining = a.durSec() - a.posSec();
  if (!adj.nextReady[o] && remaining < fadeSec + 25) loadNext(o);
  if (adj.nextReady[o] && b.state == DS_READY && !b.analyzing && remaining <= fadeSec + 0.25f) {
    b.seek(b.bpm > 0 ? b.firstBeat : 0);
    if (b.bpm > 0 && a.bpm > 0) {
      float r = syncRatio(b, a);
      b.syncOn = fabsf(r - 1) < 0.08f;
      if (b.syncOn) b.syncPitch = r - 1;
    }
    b.atEnd = false;
    b.playing = true;
    adj.fading = true;
    adj.from = active;
    adj.to = o;
    adj.start = now;
    adj.dur = (uint32_t)(fadeSec * 1000);
    adj.xfFrom = mix.xfader;
    adj.xfTo = o == 0 ? 0 : 1;
  } else if (b.state == DS_ERROR && adj.nextReady[o]) {
    loadNext(o);  // bad file, try another
  }
}

static void serviceTask(void*) {
  for (;;) {
    uint32_t now = millis();
    for (int d = 0; d < NUM_DECKS; d++) {
      Deck& k = decks[d];
      if (k.nudge != 0 && now - lastJog[d] > 120) k.nudge = 0;
      // sync: tempo follow + gentle phase lock
      Deck& o = decks[other(d)];
      if (k.syncOn && o.state == DS_READY && o.bpm > 0 && k.bpm > 0) {
        bool master = o.syncOn && ((d == 0) == (mix.xfader < 0.5f));  // both synced: audible deck leads
        if (!master) {
          float r = syncRatio(k, o);
          k.syncPitch = clampf(r - 1, -0.5f, 0.5f);
          if (k.playing && o.playing && k.nudge == 0 && !k.scratching && fabsf(r - o.effBpm() / k.bpm) < 0.01f) {
            double err = o.beatPhase() - k.beatPhase();
            err -= floor(err + 0.5);
            float adjv = fabs(err) < 0.004 ? 0 : (float)err * 0.12f;
            k.phaseAdj = clampf(adjv, -0.04f, 0.04f);
          } else k.phaseAdj = 0;
        } else k.phaseAdj = 0;
      } else k.phaseAdj = 0;
    }
    if (autoDj) {
      xSemaphoreTake(cmdLock, portMAX_DELAY);
      autoDjStep();
      xSemaphoreGive(cmdLock);
    } else {
      adj.fading = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void controlStart() {
  cmdLock = xSemaphoreCreateMutex();
  for (int d = 0; d < NUM_DECKS; d++) decks[d].pitchRange = cfg.pitchRange / 100.0f;
  xTaskCreatePinnedToCore(serviceTask, "service", 4096, nullptr, 4, nullptr, 1);
}
