// oled.cpp - see oled.h
#include "oled.h"
#include <Wire.h>
#include <SSD1306Wire.h>
#include "config.h"
#include "deck.h"
#include "mixer.h"
#include "control.h"
#include "web.h"
#include "library.h"

static SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);
static bool ok = false;
static int page = 0;
static String nMode, nSsid, nIp, nPass;
static uint32_t lastDraw = 0, btnDown = 0;
static bool btnPrev = false, longDone = false;

void oledBegin() {
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);  // power the OLED
  delay(50);
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(20);
  ok = display.init();
  if (ok) {
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
  }
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
}

void oledMessage(const char* l1, const char* l2, const char* l3) {
  if (!ok) return;
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 2, l1);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 26, l2);
  display.drawString(64, 42, l3);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.display();
}

void oledSetNet(const String& mode, const String& ssid, const String& ip, const String& pass) {
  nMode = mode; nSsid = ssid; nIp = ip; nPass = pass;
}

static String fmtTime(double s) {
  if (s < 0) s = 0;
  int m = (int)s / 60, sec = (int)s % 60;
  char b[12];
  snprintf(b, sizeof(b), "%d:%02d", m, sec);
  return b;
}

static void drawDeck(int d, int y) {
  Deck& k = decks[d];
  char line[40];
  const char* st = k.state == DS_LOADING ? "..." : (k.playing ? ">" : (k.state == DS_READY ? "||" : "-"));
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, y, d ? "B" : "A");
  display.drawString(10, y, st);
  if (k.state == DS_READY) {
    if (k.bpm > 0) snprintf(line, sizeof(line), "%.1f", k.effBpm());
    else snprintf(line, sizeof(line), k.analyzing ? "bpm.." : "---");
    display.drawString(24, y, line);
    String t = "-" + fmtTime(k.durSec() - k.posSec());
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, y, t);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    if (k.syncOn) display.drawString(62, y, "SYNC");
    if (k.loopOn) display.fillRect(92, y + 3, 4, 4);
    // beat flash
    if (k.playing && k.bpm > 0 && k.beatPhase() < 0.2) display.fillRect(18, y + 3, 3, 6);
    // progress
    double dur = k.durSec();
    int w = dur > 0 ? (int)(126 * k.posSec() / dur) : 0;
    display.drawRect(0, y + 13, 128, 4);
    display.fillRect(1, y + 14, w, 2);
  } else {
    display.drawString(24, y, k.state == DS_ERROR ? "can't read file" : "empty");
  }
}

static void drawMain() {
  display.clear();
  drawDeck(0, 0);
  drawDeck(1, 19);
  // crossfader
  int x = 4 + (int)(mix.xfader * 116);
  display.drawLine(4, 44, 124, 44);
  display.fillRect(x - 2, 41, 5, 7);
  display.setFont(ArialMT_Plain_10);
  String bottom = nIp;
  if (autoDj) bottom = "AUTO  " + bottom;
  display.drawString(0, 52, bottom);
  // master VU
  int vu = (int)(mix.vu[2] * 30);
  if (vu > 30) vu = 30;
  display.fillRect(98, 60 - vu / 4, 30 * vu / 30, 2);
  display.display();
}

static void drawNet() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, nMode == "wifi" ? "Wi-Fi connected" : "Hotspot (join this)");
  display.drawString(0, 13, "Net: " + nSsid);
  if (nMode != "wifi") display.drawString(0, 26, "Pass: " + nPass);
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 40, nIp);
  display.display();
}

static void drawSys() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "ESP32 DJ v" DJ_VERSION);
  display.drawString(0, 12, String("CPU ") + (int)(mix.cpu * 100) + "%  heap " + (ESP.getFreeHeap() / 1024) + "k");
  display.drawString(0, 24, String("Tracks ") + libCount() + "  clients " + webClients());
  display.drawString(0, 36, String("DAC ") + (mix.out1 ? "ok" : "OFF") + "  HP " + (mix.out2 ? "ok" : "-"));
  display.drawString(0, 48, "Hold PRG: Auto-DJ");
  display.display();
}

void oledLoop() {
  // button: short press = next page, long press = toggle Auto-DJ
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !btnPrev) { btnDown = now; longDone = false; }
  if (down && !longDone && now - btnDown > 900) { autoDj = !autoDj; longDone = true; }
  if (!down && btnPrev && !longDone && now - btnDown > 30) page = (page + 1) % 3;
  btnPrev = down;
  digitalWrite(PIN_LED, (decks[0].playing || decks[1].playing) && (decks[0].beatPhase() < 0.1 || decks[1].beatPhase() < 0.1));
  if (!ok || now - lastDraw < 80) return;
  lastDraw = now;
  if (page == 0) drawMain();
  else if (page == 1) drawNet();
  else drawSys();
}
