// oled.h - status screen on the Heltec's built-in 128x64 OLED + PRG button
#pragma once
#include <Arduino.h>

void oledBegin();
void oledMessage(const char* l1, const char* l2 = "", const char* l3 = "");
void oledLoop();  // call from loop(): redraw + button handling
void oledSetNet(const String& mode, const String& ssid, const String& ip, const String& pass);
