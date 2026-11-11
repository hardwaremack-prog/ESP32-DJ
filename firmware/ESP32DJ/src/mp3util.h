// mp3util.h - tiny, platform-independent MP3 helpers (frame headers, ID3, Xing)
#pragma once
#include <stdint.h>
#include <stddef.h>

struct Mp3Hdr {
  int version;          // 10 = MPEG1, 20 = MPEG2, 25 = MPEG2.5
  int bitrate;          // kbps
  int sampleRate;       // Hz
  int channels;         // 1 or 2
  int frameBytes;       // full frame length incl. header
  int samplesPerFrame;  // 1152 (MPEG1) or 576
  int sideInfoBytes;
};

// Parse a 4-byte Layer III frame header. Returns false if not a valid L3 header.
bool mp3ParseHeader(const uint8_t* p, Mp3Hdr& h);

// True if two headers belong to the same stream (version/rate/channels match).
bool mp3SameStream(const Mp3Hdr& a, const Mp3Hdr& b);

// If buffer starts with an ID3v2 tag, return its total size (header+body+footer), else 0.
uint32_t id3v2TagSize(const uint8_t* p, size_t n);

// Extract title/artist (UTF-8) from a buffer holding the start of an ID3v2 tag.
void id3v2ReadText(const uint8_t* buf, size_t n, char* title, size_t titleLen,
                   char* artist, size_t artistLen);

// Look for a Xing/Info header inside the first frame. Returns frame count or 0.
uint32_t mp3XingFrames(const uint8_t* frame, size_t n, const Mp3Hdr& h);
