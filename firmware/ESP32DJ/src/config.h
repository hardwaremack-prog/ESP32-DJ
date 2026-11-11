// config.h - build-time constants for ESP32 DJ
#pragma once

#define DJ_VERSION "1.0.0"

#if defined(DJ_BOARD_V4)
#define DJ_BOARD_NAME "Heltec WiFi LoRa 32 V4"
#else
#define DJ_BOARD_NAME "Heltec WiFi LoRa 32 V3"
#endif

// Fixed Heltec pins (same on V3 and V4)
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RST 21
#define PIN_VEXT 36      // LOW = OLED/peripheral power on
#define PIN_LED 35
#define PIN_BUTTON 0     // PRG button

// Audio engine
#define SAMPLE_RATE 44100
#define BLOCK 128        // frames per mixer block (~2.9 ms)
#define NUM_DECKS 2
#define HOTCUES 4

// Default user-changeable pins (Settings page)
#define DEF_SD_CS 42
#define DEF_SD_SCK 41
#define DEF_SD_MOSI 40
#define DEF_SD_MISO 39
#define DEF_I2S_BCK 4
#define DEF_I2S_WS 5
#define DEF_I2S_DOUT 6
#define DEF_I2S2_BCK 47
#define DEF_I2S2_WS 48
#define DEF_I2S2_DOUT 33

#define AP_SSID_PREFIX "ESP32-DJ-"
#define AP_DEFAULT_PASS "djmix1234"
#define HOSTNAME_DEFAULT "esp32dj"
