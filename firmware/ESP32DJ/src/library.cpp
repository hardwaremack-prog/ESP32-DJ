// library.cpp - see library.h
#include "library.h"
#include <SD.h>
#include <vector>
#include <string>
#include "settings.h"
#include "esp_heap_caps.h"

static SemaphoreHandle_t lk;
static char* pool = nullptr;      // all paths, '\0' separated
static uint32_t poolLen = 0, poolCap = 0, poolMax = 0;
static uint32_t* offs = nullptr;  // offsets into pool
static int count = 0, offCap = 0, maxTracks = 0;
static volatile uint32_t ver = 1;
static volatile bool scanning = false;
static bool usePsram = false;

struct BpmEntry { uint32_t hash; float bpm; double first; };
static std::vector<BpmEntry> bpmCache;
static std::vector<std::string> q;
static uint32_t recent[12];
static int recentPos = 0;

static uint32_t fnv(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

static void* xalloc(void* old, size_t n) {
  void* p = usePsram ? heap_caps_realloc(old, n, MALLOC_CAP_SPIRAM) : nullptr;
  if (!p) p = realloc(old, n);
  return p;
}

void libLock() { xSemaphoreTake(lk, portMAX_DELAY); }
void libUnlock() { xSemaphoreGive(lk); }

static bool isMp3(const char* n) {
  size_t l = strlen(n);
  return l > 4 && strcasecmp(n + l - 4, ".mp3") == 0;
}

static void addPath(const char* p) {
  if (count >= maxTracks) return;
  uint32_t l = strlen(p) + 1;
  if (poolLen + l > poolCap) {
    uint32_t nc = poolCap ? poolCap * 2 : 8192;
    while (nc < poolLen + l) nc *= 2;
    if (nc > poolMax) nc = poolMax;
    if (poolLen + l > nc) return;
    char* np = (char*)xalloc(pool, nc);
    if (!np) return;
    pool = np; poolCap = nc;
  }
  if (count >= offCap) {
    int nc = offCap ? offCap * 2 : 256;
    uint32_t* no = (uint32_t*)xalloc(offs, nc * 4);
    if (!no) return;
    offs = no; offCap = nc;
  }
  memcpy(pool + poolLen, p, l);
  offs[count++] = poolLen;
  poolLen += l;
}

static void scanDir(const String& dir, int depth) {
  if (depth > 6) return;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return;
  for (;;) {
    File e = d.openNextFile();
    if (!e) break;
    String name = e.name();
    int sl = name.lastIndexOf('/');
    if (sl >= 0) name = name.substring(sl + 1);
    String full = dir.endsWith("/") ? dir + name : dir + "/" + name;
    bool isDir = e.isDirectory();
    e.close();
    if (name.startsWith(".") || name == "System Volume Information" || full == "/dj") continue;
    if (isDir) scanDir(full, depth + 1);
    else if (isMp3(name.c_str())) addPath(full.c_str());
    if ((count & 31) == 0) vTaskDelay(1);
  }
  d.close();
}

static void loadBpmCache() {
  bpmCache.clear();
  File f = SD.open("/dj/bpm.tsv", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int a = line.indexOf('\t');
    int b = line.indexOf('\t', a + 1);
    if (a < 0 || b < 0) continue;
    BpmEntry e;
    e.bpm = line.substring(0, a).toFloat();
    e.first = line.substring(a + 1, b).toDouble();
    String p = line.substring(b + 1);
    p.trim();
    e.hash = fnv(p.c_str());
    bool replaced = false;
    for (auto& x : bpmCache) if (x.hash == e.hash) { x = e; replaced = true; break; }
    if (!replaced) bpmCache.push_back(e);
  }
  f.close();
}

static void scanTask(void*) {
  xSemaphoreTake(lk, portMAX_DELAY);
  count = 0; poolLen = 0;
  scanDir(cfg.musicDir.length() ? cfg.musicDir : String("/"), 0);
  xSemaphoreGive(lk);
  ver++;
  scanning = false;
  Serial.printf("[lib] %d tracks\n", count);
  vTaskDelete(nullptr);
}

void libBegin(bool psram) {
  usePsram = psram;
  maxTracks = psram ? 8000 : 700;
  poolMax = psram ? 1024 * 1024 : 48 * 1024;
  lk = xSemaphoreCreateMutex();
  SD.mkdir("/dj");
  if (!SD.exists("/music")) SD.mkdir("/music");
  loadBpmCache();
  libRescan();
}

void libRescan() {
  if (scanning) return;
  scanning = true;
  xTaskCreatePinnedToCore(scanTask, "libscan", 6144, nullptr, 1, nullptr, 0);
}

uint32_t libVersion() { return ver; }
int libCount() { return count; }
const char* libPath(int i) { return (i >= 0 && i < count) ? pool + offs[i] : ""; }

bool libBpmLookup(const char* path, float& bpm, double& first) {
  uint32_t h = fnv(path);
  bool ok = false;
  xSemaphoreTake(lk, portMAX_DELAY);
  for (auto& e : bpmCache) if (e.hash == h) { bpm = e.bpm; first = e.first; ok = true; break; }
  xSemaphoreGive(lk);
  return ok;
}

bool libBpmLookupUnlocked(const char* path, float& bpm, double& first) {
  uint32_t h = fnv(path);
  for (auto& e : bpmCache) if (e.hash == h) { bpm = e.bpm; first = e.first; return true; }
  return false;
}

void libBpmStore(const char* path, float bpm, double first) {
  uint32_t h = fnv(path);
  xSemaphoreTake(lk, portMAX_DELAY);
  bool found = false;
  for (auto& e : bpmCache) if (e.hash == h) { e.bpm = bpm; e.first = first; found = true; break; }
  if (!found && bpmCache.size() < 6000) bpmCache.push_back({h, bpm, first});
  xSemaphoreGive(lk);
  File f = SD.open("/dj/bpm.tsv", FILE_APPEND);
  if (f) {
    f.printf("%.2f\t%.1f\t%s\n", bpm, first, path);
    f.close();
  }
  ver++;
}

void libMarkPlayed(const char* path) {
  recent[recentPos++ % 12] = fnv(path);
}

const char* libRandom() {
  static char out[200];
  if (!count) return nullptr;
  xSemaphoreTake(lk, portMAX_DELAY);
  const char* pick = nullptr;
  for (int tries = 0; tries < 30; tries++) {
    const char* p = libPath(esp_random() % count);
    uint32_t h = fnv(p);
    bool rec = false;
    for (int i = 0; i < 12; i++) if (recent[i] == h) rec = true;
    pick = p;
    if (!rec) break;
  }
  if (pick) strlcpy(out, pick, sizeof(out));
  xSemaphoreGive(lk);
  return pick ? out : nullptr;
}

static SemaphoreHandle_t qlk() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}
struct QG { QG() { xSemaphoreTake(qlk(), portMAX_DELAY); } ~QG() { xSemaphoreGive(qlk()); } };

void queueAdd(const char* p) { QG g; if (q.size() < 100) q.push_back(p); ver++; }
void queueRemove(int i) { QG g; if (i >= 0 && i < (int)q.size()) q.erase(q.begin() + i); ver++; }
void queueClear() { QG g; q.clear(); ver++; }
int queueCount() { QG g; return q.size(); }
bool queuePop(char* out, size_t n) {
  QG g;
  if (q.empty()) return false;
  strlcpy(out, q.front().c_str(), n);
  q.erase(q.begin());
  ver++;
  return true;
}
String queueJson() {
  QG g;
  String j = "[";
  for (size_t i = 0; i < q.size(); i++) {
    if (i) j += ",";
    j += "\"";
    for (char c : q[i]) { if (c == '"' || c == '\\') j += '\\'; j += c; }
    j += "\"";
  }
  return j + "]";
}
