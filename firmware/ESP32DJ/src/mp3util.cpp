// mp3util.cpp - see mp3util.h
#include "mp3util.h"
#include <string.h>

static const int BR_V1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1};
static const int BR_V2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};
static const int SR_V1[4] = {44100, 48000, 32000, -1};

bool mp3ParseHeader(const uint8_t* p, Mp3Hdr& h) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  int ver = (p[1] >> 3) & 3;    // 0=2.5, 1=reserved, 2=MPEG2, 3=MPEG1
  int layer = (p[1] >> 1) & 3;  // 1 = Layer III
  if (ver == 1 || layer != 1) return false;
  int bri = (p[2] >> 4) & 15;
  int sri = (p[2] >> 2) & 3;
  int pad = (p[2] >> 1) & 1;
  int mode = (p[3] >> 6) & 3;
  if (bri == 0 || bri == 15 || sri == 3) return false;  // free-format not supported
  h.version = ver == 3 ? 10 : (ver == 2 ? 20 : 25);
  h.bitrate = (ver == 3) ? BR_V1[bri] : BR_V2[bri];
  int sr = SR_V1[sri];
  if (ver == 2) sr /= 2;
  if (ver == 0) sr /= 4;
  h.sampleRate = sr;
  h.channels = (mode == 3) ? 1 : 2;
  if (ver == 3) {
    h.frameBytes = 144 * h.bitrate * 1000 / sr + pad;
    h.samplesPerFrame = 1152;
    h.sideInfoBytes = h.channels == 1 ? 17 : 32;
  } else {
    h.frameBytes = 72 * h.bitrate * 1000 / sr + pad;
    h.samplesPerFrame = 576;
    h.sideInfoBytes = h.channels == 1 ? 9 : 17;
  }
  return h.frameBytes > 4;
}

bool mp3SameStream(const Mp3Hdr& a, const Mp3Hdr& b) {
  return a.version == b.version && a.sampleRate == b.sampleRate;
}

uint32_t id3v2TagSize(const uint8_t* p, size_t n) {
  if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
  if ((p[6] | p[7] | p[8] | p[9]) & 0x80) return 0;
  uint32_t sz = ((uint32_t)p[6] << 21) | ((uint32_t)p[7] << 14) | ((uint32_t)p[8] << 7) | p[9];
  sz += 10;
  if (p[5] & 0x10) sz += 10;  // footer
  return sz;
}

// Append a unicode code point as UTF-8
static size_t putUtf8(char* out, size_t pos, size_t cap, uint32_t cp) {
  if (cp < 0x80) {
    if (pos + 1 < cap) out[pos++] = (char)cp;
  } else if (cp < 0x800) {
    if (pos + 2 < cap) { out[pos++] = 0xC0 | (cp >> 6); out[pos++] = 0x80 | (cp & 0x3F); }
  } else {
    if (pos + 3 < cap) {
      out[pos++] = 0xE0 | (cp >> 12);
      out[pos++] = 0x80 | ((cp >> 6) & 0x3F);
      out[pos++] = 0x80 | (cp & 0x3F);
    }
  }
  return pos;
}

static void decodeText(const uint8_t* d, size_t len, char* out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (len < 1) return;
  uint8_t enc = d[0];
  d++; len--;
  size_t pos = 0;
  if (enc == 0 || enc == 3) {  // Latin-1 or UTF-8
    for (size_t i = 0; i < len && d[i]; i++) {
      if (enc == 3) { if (pos + 1 < cap) out[pos++] = (char)d[i]; }
      else pos = putUtf8(out, pos, cap, d[i]);
    }
  } else {  // UTF-16 (1 = with BOM, 2 = big endian)
    bool be = (enc == 2);
    size_t i = 0;
    if (enc == 1 && len >= 2) {
      if (d[0] == 0xFE && d[1] == 0xFF) { be = true; i = 2; }
      else if (d[0] == 0xFF && d[1] == 0xFE) { be = false; i = 2; }
    }
    for (; i + 1 < len; i += 2) {
      uint32_t c = be ? ((d[i] << 8) | d[i + 1]) : ((d[i + 1] << 8) | d[i]);
      if (c == 0) break;
      if (c >= 0xD800 && c <= 0xDFFF) c = '?';
      pos = putUtf8(out, pos, cap, c);
    }
  }
  out[pos] = 0;
  // trim trailing spaces
  while (pos > 0 && out[pos - 1] == ' ') out[--pos] = 0;
}

void id3v2ReadText(const uint8_t* buf, size_t n, char* title, size_t titleLen,
                   char* artist, size_t artistLen) {
  uint32_t tagSize = id3v2TagSize(buf, n);
  if (!tagSize) return;
  int major = buf[3];
  size_t end = tagSize < n ? tagSize : n;
  size_t pos = 10;
  if (buf[5] & 0x40 && major >= 3) {  // extended header
    if (pos + 4 > end) return;
    uint32_t ext = major == 4
        ? (((uint32_t)buf[pos] << 21) | (buf[pos + 1] << 14) | (buf[pos + 2] << 7) | buf[pos + 3])
        : (((uint32_t)buf[pos] << 24) | (buf[pos + 1] << 16) | (buf[pos + 2] << 8) | buf[pos + 3]) + 4;
    pos += ext;
  }
  int hdr = major == 2 ? 6 : 10;
  while (pos + hdr <= end) {
    const uint8_t* f = buf + pos;
    if (f[0] == 0) break;  // padding
    uint32_t fsz;
    char id[5] = {0};
    if (major == 2) {
      memcpy(id, f, 3);
      fsz = (f[3] << 16) | (f[4] << 8) | f[5];
    } else {
      memcpy(id, f, 4);
      if (major == 4) fsz = ((uint32_t)f[4] << 21) | (f[5] << 14) | (f[6] << 7) | f[7];
      else fsz = ((uint32_t)f[4] << 24) | (f[5] << 16) | (f[6] << 8) | f[7];
    }
    if (pos + hdr + fsz > end) break;
    const uint8_t* data = f + hdr;
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) decodeText(data, fsz, title, titleLen);
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) decodeText(data, fsz, artist, artistLen);
    pos += hdr + fsz;
  }
}

uint32_t mp3XingFrames(const uint8_t* frame, size_t n, const Mp3Hdr& h) {
  size_t off = 4 + h.sideInfoBytes;
  if (off + 12 > n) return 0;
  const uint8_t* x = frame + off;
  if (memcmp(x, "Xing", 4) && memcmp(x, "Info", 4)) return 0;
  uint32_t flags = ((uint32_t)x[4] << 24) | (x[5] << 16) | (x[6] << 8) | x[7];
  if (!(flags & 1)) return 0;
  return ((uint32_t)x[8] << 24) | (x[9] << 16) | (x[10] << 8) | x[11];
}
