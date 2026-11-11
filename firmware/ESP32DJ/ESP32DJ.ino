// ESP32 DJ - two-deck MP3 DJ mixer for Heltec WiFi LoRa 32 V3 / V4 (ESP32-S3)
//
// * MP3s from a microSD card, two decks mixed in real time
// * Control surface in any phone/PC browser (Wi-Fi hotspot or your network)
// * Audio out: PCM5102A I2S DAC (+ optional 2nd DAC for headphone cueing)
//
// Build: see README.md (Arduino ESP32 core 3.x, ThingPulse SSD1306 library)

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "src/config.h"
#include "src/settings.h"
#include "src/deck.h"
#include "src/mixer.h"
#include "src/analyzer.h"
#include "src/library.h"
#include "src/control.h"
#include "src/web.h"
#include "src/oled.h"

static SPIClass sdSpi(FSPI);
static bool sdOk = false;

static void startWifi() {
  WiFi.persistent(false);
  String ip, mode, ssid;
  if (cfg.ssid.length()) {
    oledMessage("ESP32 DJ", "Joining Wi-Fi", cfg.ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg.hostname.c_str());
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    mode = "wifi";
    ssid = WiFi.SSID();
    ip = WiFi.localIP().toString();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[32];
    snprintf(name, sizeof(name), AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    WiFi.softAP(name, cfg.apPass.c_str(), 6, 0, 4);
    mode = "hotspot";
    ssid = name;
    ip = WiFi.softAPIP().toString();
  }
  WiFi.setSleep(false);  // lower latency for the controls
  if (MDNS.begin(cfg.hostname.c_str())) MDNS.addService("http", "tcp", 80);
  oledSetNet(mode, ssid, ip, cfg.apPass);
  Serial.printf("[wifi] %s %s  http://%s/  (http://%s.local/)\n", mode.c_str(), ssid.c_str(), ip.c_str(),
                cfg.hostname.c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 DJ v" DJ_VERSION " (" DJ_BOARD_NAME ") ===");
  settingsLoad();
  oledBegin();
  oledMessage("ESP32 DJ", "starting...", DJ_BOARD_NAME);
  bool psram = psramFound();
  Serial.printf("PSRAM: %s (%u KB)\n", psram ? "yes" : "no", (unsigned)(ESP.getPsramSize() / 1024));

  sdSpi.begin(cfg.sdSck, cfg.sdMiso, cfg.sdMosi, cfg.sdCs);
  sdOk = SD.begin(cfg.sdCs, sdSpi, (uint32_t)cfg.sdMHz * 1000000UL, "/sd", 12);
  if (!sdOk) sdOk = SD.begin(cfg.sdCs, sdSpi, 4000000, "/sd", 12);  // retry slow
  Serial.printf("SD: %s\n", sdOk ? "ok" : "NOT FOUND");
  if (!sdOk) oledMessage("ESP32 DJ", "No SD card found", "check wiring / format FAT32");

  startWifi();
  webStart();
  for (int d = 0; d < NUM_DECKS; d++) decks[d].init(d, psram);
  mixerStart(psram);
  analyzerStart();
  libBegin(psram);
  controlStart();
  Serial.printf("Free heap %u KB, largest block %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  if (!sdOk) delay(2500);
}

void loop() {
  oledLoop();
  delay(20);
}
