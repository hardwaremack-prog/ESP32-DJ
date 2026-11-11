// library.h - music library on the SD card, BPM cache and Auto-DJ queue
#pragma once
#include <Arduino.h>

void libBegin(bool psram);
void libRescan();                 // async full rescan
uint32_t libVersion();
int libCount();
const char* libPath(int i);       // valid until next rescan (hold libLock)
void libLock();
void libUnlock();
bool libBpmLookup(const char* path, float& bpm, double& firstBeat);
bool libBpmLookupUnlocked(const char* path, float& bpm, double& firstBeat);  // caller holds libLock
void libBpmStore(const char* path, float bpm, double firstBeat);
const char* libRandom();          // random track, avoids recently played
void libMarkPlayed(const char* path);

// Auto-DJ queue
void queueAdd(const char* path);
void queueRemove(int i);
void queueClear();
bool queuePop(char* out, size_t n);
int queueCount();
String queueJson();
