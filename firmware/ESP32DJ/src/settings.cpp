// settings.cpp - see settings.h
#include "settings.h"
#include "config.h"
#include <Preferences.h>

Settings cfg;
static Preferences prefs;

void settingsLoad() {
  prefs.begin("dj", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.apPass = prefs.getString("appass", AP_DEFAULT_PASS);
  cfg.hostname = prefs.getString("host", HOSTNAME_DEFAULT);
  cfg.sdCs = prefs.getInt("sdcs", DEF_SD_CS);
  cfg.sdSck = prefs.getInt("sdsck", DEF_SD_SCK);
  cfg.sdMosi = prefs.getInt("sdmosi", DEF_SD_MOSI);
  cfg.sdMiso = prefs.getInt("sdmiso", DEF_SD_MISO);
  cfg.sdMHz = prefs.getInt("sdmhz", 20);
  cfg.i2sBck = prefs.getInt("bck", DEF_I2S_BCK);
  cfg.i2sWs = prefs.getInt("ws", DEF_I2S_WS);
  cfg.i2sDout = prefs.getInt("dout", DEF_I2S_DOUT);
  cfg.dac2 = prefs.getBool("dac2", false);
  cfg.i2s2Bck = prefs.getInt("bck2", DEF_I2S2_BCK);
  cfg.i2s2Ws = prefs.getInt("ws2", DEF_I2S2_WS);
  cfg.i2s2Dout = prefs.getInt("dout2", DEF_I2S2_DOUT);
  cfg.splitCue = prefs.getBool("split", false);
  cfg.xfCurve = prefs.getInt("xfc", 0);
  cfg.fadeBeats = prefs.getInt("fade", 16);
  cfg.pitchRange = prefs.getInt("prange", 8);
  cfg.musicDir = prefs.getString("mdir", "/");
  prefs.end();
  if (cfg.apPass.length() < 8) cfg.apPass = AP_DEFAULT_PASS;
}

void settingsSave() {
  prefs.begin("dj", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("appass", cfg.apPass);
  prefs.putString("host", cfg.hostname);
  prefs.putInt("sdcs", cfg.sdCs);
  prefs.putInt("sdsck", cfg.sdSck);
  prefs.putInt("sdmosi", cfg.sdMosi);
  prefs.putInt("sdmiso", cfg.sdMiso);
  prefs.putInt("sdmhz", cfg.sdMHz);
  prefs.putInt("bck", cfg.i2sBck);
  prefs.putInt("ws", cfg.i2sWs);
  prefs.putInt("dout", cfg.i2sDout);
  prefs.putBool("dac2", cfg.dac2);
  prefs.putInt("bck2", cfg.i2s2Bck);
  prefs.putInt("ws2", cfg.i2s2Ws);
  prefs.putInt("dout2", cfg.i2s2Dout);
  prefs.putBool("split", cfg.splitCue);
  prefs.putInt("xfc", cfg.xfCurve);
  prefs.putInt("fade", cfg.fadeBeats);
  prefs.putInt("prange", cfg.pitchRange);
  prefs.putString("mdir", cfg.musicDir);
  prefs.end();
}

static bool toBool(const String& v) { return v == "1" || v == "true" || v == "on"; }

bool settingsSet(const String& k, const String& v) {
  int i = v.toInt();
  if (k == "ssid") cfg.ssid = v;
  else if (k == "pass") { if (v != "********") cfg.pass = v; }
  else if (k == "apPass") { if (v.length() >= 8) cfg.apPass = v; }
  else if (k == "hostname") { if (v.length()) cfg.hostname = v; }
  else if (k == "sdCs") cfg.sdCs = i;
  else if (k == "sdSck") cfg.sdSck = i;
  else if (k == "sdMosi") cfg.sdMosi = i;
  else if (k == "sdMiso") cfg.sdMiso = i;
  else if (k == "sdMHz") cfg.sdMHz = constrain(i, 1, 40);
  else if (k == "i2sBck") cfg.i2sBck = i;
  else if (k == "i2sWs") cfg.i2sWs = i;
  else if (k == "i2sDout") cfg.i2sDout = i;
  else if (k == "dac2") cfg.dac2 = toBool(v);
  else if (k == "i2s2Bck") cfg.i2s2Bck = i;
  else if (k == "i2s2Ws") cfg.i2s2Ws = i;
  else if (k == "i2s2Dout") cfg.i2s2Dout = i;
  else if (k == "splitCue") cfg.splitCue = toBool(v);
  else if (k == "xfCurve") cfg.xfCurve = constrain(i, 0, 2);
  else if (k == "fadeBeats") cfg.fadeBeats = constrain(i, 4, 64);
  else if (k == "pitchRange") cfg.pitchRange = constrain(i, 4, 100);
  else if (k == "musicDir") { cfg.musicDir = v.length() ? v : "/"; }
  else return false;
  return true;
}

static String esc(const String& s) {
  String o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

String settingsJson() {
  String j = "{";
  j += "\"ssid\":\"" + esc(cfg.ssid) + "\",";
  j += "\"pass\":\"" + String(cfg.pass.length() ? "********" : "") + "\",";
  j += "\"apPass\":\"" + esc(cfg.apPass) + "\",";
  j += "\"hostname\":\"" + esc(cfg.hostname) + "\",";
  j += "\"sdCs\":" + String(cfg.sdCs) + ",\"sdSck\":" + String(cfg.sdSck) + ",\"sdMosi\":" + String(cfg.sdMosi) +
       ",\"sdMiso\":" + String(cfg.sdMiso) + ",\"sdMHz\":" + String(cfg.sdMHz) + ",";
  j += "\"i2sBck\":" + String(cfg.i2sBck) + ",\"i2sWs\":" + String(cfg.i2sWs) + ",\"i2sDout\":" + String(cfg.i2sDout) + ",";
  j += "\"dac2\":" + String(cfg.dac2 ? "true" : "false") + ",";
  j += "\"i2s2Bck\":" + String(cfg.i2s2Bck) + ",\"i2s2Ws\":" + String(cfg.i2s2Ws) + ",\"i2s2Dout\":" + String(cfg.i2s2Dout) + ",";
  j += "\"splitCue\":" + String(cfg.splitCue ? "true" : "false") + ",";
  j += "\"xfCurve\":" + String(cfg.xfCurve) + ",\"fadeBeats\":" + String(cfg.fadeBeats) + ",";
  j += "\"pitchRange\":" + String(cfg.pitchRange) + ",";
  j += "\"musicDir\":\"" + esc(cfg.musicDir) + "\"}";
  return j;
}
