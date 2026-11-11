// control.h - command parser, status JSON, sync engine, Auto-DJ
#pragma once
#include <Arduino.h>

void controlStart();
void controlCommand(const char* line);   // one or more '\n'-separated commands
String statusJson();
extern volatile bool autoDj;
extern volatile bool quantize;
