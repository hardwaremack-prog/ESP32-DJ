// analyzer.h - background track analysis: exact seek index, duration, BPM + beat grid
#pragma once
#include <Arduino.h>

void analyzerStart();
void analyzerEnqueue(int deck, uint32_t gen);
