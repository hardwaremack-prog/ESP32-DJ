// settings.h - persistent settings (NVS)
#pragma once
#include <Arduino.h>

struct Settings {
  String ssid, pass;          // home Wi-Fi (empty = hotspot only)
  String apPass;              // hotspot password
  String hostname;
  int sdCs, sdSck, sdMosi, sdMiso, sdMHz;
  int i2sBck, i2sWs, i2sDout;
  bool dac2;                  // second PCM5102A for headphones
  int i2s2Bck, i2s2Ws, i2s2Dout;
  bool splitCue;              // single DAC: L = cue, R = master
  int xfCurve;                // 0 smooth, 1 sharp (scratch), 2 linear
  int fadeBeats;              // Auto-DJ crossfade length
  int pitchRange;             // default pitch range in % (8, 16, 50)
  String musicDir;
};

extern Settings cfg;
void settingsLoad();
void settingsSave();
// apply a key=value pair (from the web form). Returns false if unknown key.
bool settingsSet(const String& key, const String& val);
String settingsJson();
