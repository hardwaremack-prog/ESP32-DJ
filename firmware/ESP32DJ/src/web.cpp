// web.cpp - esp_http_server based web app + websocket
#include "web.h"
#include <SD.h>
#include <WiFi.h>
#include <Update.h>
#include "esp_http_server.h"
#include "config.h"
#include "settings.h"
#include "control.h"
#include "library.h"
#include "mixer.h"
#include "webui.h"

static httpd_handle_t srv = nullptr;
static volatile int wsCount = 0;
int webClients() { return wsCount; }

// ----------------------------------------------------------- helpers
static void urlDecode(char* s) {
  char* o = s;
  for (; *s; s++) {
    if (*s == '+') *o++ = ' ';
    else if (*s == '%' && isxdigit((uint8_t)s[1]) && isxdigit((uint8_t)s[2])) {
      char h[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(h, nullptr, 16);
      s += 2;
    } else *o++ = *s;
  }
  *o = 0;
}

static bool qparam(httpd_req_t* r, const char* key, char* out, size_t n) {
  size_t ql = httpd_req_get_url_query_len(r);
  if (!ql) return false;
  char* q = (char*)malloc(ql + 1);
  if (!q) return false;
  httpd_req_get_url_query_str(r, q, ql + 1);
  bool ok = httpd_query_key_value(q, key, out, n) == ESP_OK;
  free(q);
  if (ok) urlDecode(out);
  return ok;
}

static bool safePath(const char* p) {
  return p[0] == '/' && !strstr(p, "..") && strlen(p) < 190;
}

static esp_err_t sendJson(httpd_req_t* r, const String& s) {
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, s.c_str(), s.length());
}

static esp_err_t fail(httpd_req_t* r, const char* status, const char* msg) {
  httpd_resp_set_status(r, status);
  httpd_resp_set_type(r, "application/json");
  String s = String("{\"ok\":false,\"error\":\"") + msg + "\"}";
  return httpd_resp_send(r, s.c_str(), s.length());
}

static void jsonStr(String& o, const char* s) {
  o += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { o += '\\'; o += *s; }
    else if ((uint8_t)*s < 0x20) o += ' ';
    else o += *s;
  }
  o += '"';
}

static void mkdirs(const char* path) {
  String p = path;
  int i = 1;
  while ((i = p.indexOf('/', i)) > 0) {
    String d = p.substring(0, i);
    if (!SD.exists(d)) SD.mkdir(d);
    i++;
  }
}

// ----------------------------------------------------------- handlers
static esp_err_t hIndex(httpd_req_t* r) {
  httpd_resp_set_type(r, "text/html");
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
  return httpd_resp_send(r, (const char*)WEBUI_GZ, WEBUI_GZ_LEN);
}

static esp_err_t hStatus(httpd_req_t* r) { return sendJson(r, statusJson()); }

static esp_err_t hInfo(httpd_req_t* r) {
  String o = "{";
  o += "\"version\":\"" DJ_VERSION "\",\"board\":\"" DJ_BOARD_NAME "\",";
  o += "\"psram\":" + String(ESP.getPsramSize() / 1024) + ",";
  o += "\"heap\":" + String(ESP.getFreeHeap() / 1024) + ",";
  o += "\"minHeap\":" + String(ESP.getMinFreeHeap() / 1024) + ",";
  uint64_t tot = SD.totalBytes(), used = SD.usedBytes();
  o += "\"sdTotal\":" + String((uint32_t)(tot / 1048576)) + ",\"sdUsed\":" + String((uint32_t)(used / 1048576)) + ",";
  o += "\"sdOk\":" + String(SD.cardType() != CARD_NONE ? "true" : "false") + ",";
  bool sta = WiFi.status() == WL_CONNECTED;
  o += "\"mode\":\"" + String(sta ? "wifi" : "hotspot") + "\",";
  o += "\"ip\":\"" + (sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  o += "\"ssid\":";
  jsonStr(o, sta ? WiFi.SSID().c_str() : WiFi.softAPSSID().c_str());
  o += ",\"tracks\":" + String(libCount());
  o += ",\"pads\":[";
  for (int i = 0; i < NUM_PADS; i++) { if (i) o += ","; jsonStr(o, padName(i)); }
  o += "]}";
  return sendJson(r, o);
}

static esp_err_t hLib(httpd_req_t* r) {
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  String chunk = "{\"v\":" + String(libVersion()) + ",\"q\":" + queueJson() + ",\"t\":[";
  libLock();
  int n = libCount();
  for (int i = 0; i < n; i++) {
    const char* p = libPath(i);
    if (i) chunk += ',';
    chunk += '[';
    jsonStr(chunk, p);
    float b; double f;
    if (libBpmLookupUnlocked(p, b, f)) { chunk += ','; chunk += String(b, 2); }
    chunk += ']';
    if (chunk.length() > 3000) {
      httpd_resp_send_chunk(r, chunk.c_str(), chunk.length());
      chunk = "";
    }
  }
  libUnlock();
  chunk += "]}";
  httpd_resp_send_chunk(r, chunk.c_str(), chunk.length());
  return httpd_resp_send_chunk(r, nullptr, 0);
}

static esp_err_t hLs(httpd_req_t* r) {
  char dir[200] = "/";
  qparam(r, "dir", dir, sizeof(dir));
  if (!safePath(dir)) return fail(r, "400 Bad Request", "bad path");
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return fail(r, "404 Not Found", "no such folder");
  String o = "{\"dir\":";
  jsonStr(o, dir);
  o += ",\"e\":[";
  bool first = true;
  for (;;) {
    File e = d.openNextFile();
    if (!e) break;
    String name = e.name();
    int sl = name.lastIndexOf('/');
    if (sl >= 0) name = name.substring(sl + 1);
    if (!first) o += ',';
    first = false;
    o += "{\"n\":";
    jsonStr(o, name.c_str());
    o += e.isDirectory() ? ",\"d\":1}" : (",\"s\":" + String((uint32_t)e.size()) + "}");
    e.close();
  }
  d.close();
  o += "]}";
  return sendJson(r, o);
}

static esp_err_t hUpload(httpd_req_t* r) {
  char path[200];
  if (!qparam(r, "path", path, sizeof(path)) || !safePath(path)) return fail(r, "400 Bad Request", "bad path");
  mkdirs(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return fail(r, "500 Internal Server Error", "cannot create file");
  const size_t B = 8192;
  char* buf = (char*)malloc(B);
  if (!buf) { f.close(); return fail(r, "500 Internal Server Error", "no memory"); }
  size_t remaining = r->content_len;
  int timeouts = 0;
  bool ok = true;
  while (remaining > 0) {
    int n = httpd_req_recv(r, buf, remaining < B ? remaining : B);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
    if (n <= 0) { ok = false; break; }
    timeouts = 0;
    if (f.write((uint8_t*)buf, n) != (size_t)n) { ok = false; break; }
    remaining -= n;
  }
  f.close();
  free(buf);
  if (!ok) { SD.remove(path); return fail(r, "500 Internal Server Error", "upload failed (card full?)"); }
  libRescan();
  return sendJson(r, "{\"ok\":true}");
}

static esp_err_t hFile(httpd_req_t* r) {
  char path[200];
  if (!qparam(r, "path", path, sizeof(path)) || !safePath(path)) return fail(r, "400 Bad Request", "bad path");
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) return fail(r, "404 Not Found", "not found");
  const char* ext = strrchr(path, '.');
  httpd_resp_set_type(r, ext && !strcasecmp(ext, ".mp3") ? "audio/mpeg" : "application/octet-stream");
  char* buf = (char*)malloc(4096);
  if (!buf) { f.close(); return fail(r, "500 Internal Server Error", "no memory"); }
  int n;
  esp_err_t e = ESP_OK;
  while ((n = f.read((uint8_t*)buf, 4096)) > 0) {
    e = httpd_resp_send_chunk(r, buf, n);
    if (e != ESP_OK) break;
  }
  free(buf);
  f.close();
  if (e == ESP_OK) httpd_resp_send_chunk(r, nullptr, 0);
  return e;
}

static esp_err_t hRm(httpd_req_t* r) {
  char path[200];
  if (!qparam(r, "path", path, sizeof(path)) || !safePath(path) || !strcmp(path, "/")) return fail(r, "400 Bad Request", "bad path");
  File f = SD.open(path);
  if (!f) return fail(r, "404 Not Found", "not found");
  bool dir = f.isDirectory();
  f.close();
  bool ok = dir ? SD.rmdir(path) : SD.remove(path);
  if (!ok) return fail(r, "409 Conflict", dir ? "folder not empty" : "delete failed");
  libRescan();
  return sendJson(r, "{\"ok\":true}");
}

static esp_err_t hMkdir(httpd_req_t* r) {
  char path[200];
  if (!qparam(r, "path", path, sizeof(path)) || !safePath(path)) return fail(r, "400 Bad Request", "bad path");
  mkdirs((String(path) + "/").c_str());
  return sendJson(r, "{\"ok\":true}");
}

static esp_err_t hSettingsGet(httpd_req_t* r) { return sendJson(r, settingsJson()); }

static void rebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP.restart();
}

static esp_err_t hSettingsPost(httpd_req_t* r) {
  size_t len = r->content_len;
  if (len > 4000) return fail(r, "413 Payload Too Large", "too big");
  char* body = (char*)calloc(1, len + 1);
  if (!body) return fail(r, "500 Internal Server Error", "no memory");
  size_t got = 0;
  while (got < len) {
    int n = httpd_req_recv(r, body + got, len - got);
    if (n <= 0) { free(body); return fail(r, "400 Bad Request", "read error"); }
    got += n;
  }
  char* save;
  for (char* kv = strtok_r(body, "&", &save); kv; kv = strtok_r(nullptr, "&", &save)) {
    char* eq = strchr(kv, '=');
    if (!eq) continue;
    *eq = 0;
    urlDecode(kv);
    urlDecode(eq + 1);
    settingsSet(kv, eq + 1);
  }
  free(body);
  settingsSave();
  sendJson(r, "{\"ok\":true}");
  xTaskCreate(rebootTask, "reboot", 2048, nullptr, 1, nullptr);
  return ESP_OK;
}

static esp_err_t hReboot(httpd_req_t* r) {
  sendJson(r, "{\"ok\":true}");
  xTaskCreate(rebootTask, "reboot", 2048, nullptr, 1, nullptr);
  return ESP_OK;
}

static esp_err_t hOta(httpd_req_t* r) {
  size_t len = r->content_len;
  if (!len || !Update.begin(len)) return fail(r, "500 Internal Server Error", "OTA begin failed");
  char* buf = (char*)malloc(4096);
  if (!buf) { Update.abort(); return fail(r, "500 Internal Server Error", "no memory"); }
  size_t rem = len;
  int timeouts = 0;
  while (rem > 0) {
    int n = httpd_req_recv(r, buf, rem < 4096 ? rem : 4096);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
    if (n <= 0) { free(buf); Update.abort(); return fail(r, "500 Internal Server Error", "upload interrupted"); }
    timeouts = 0;
    if (Update.write((uint8_t*)buf, n) != (size_t)n) { free(buf); Update.abort(); return fail(r, "500 Internal Server Error", "flash write failed"); }
    rem -= n;
  }
  free(buf);
  if (!Update.end(true)) return fail(r, "500 Internal Server Error", "image invalid");
  sendJson(r, "{\"ok\":true}");
  xTaskCreate(rebootTask, "reboot", 2048, nullptr, 1, nullptr);
  return ESP_OK;
}

// ----------------------------------------------------------- websocket
static esp_err_t hWs(httpd_req_t* r) {
  if (r->method == HTTP_GET) return ESP_OK;  // handshake done
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t e = httpd_ws_recv_frame(r, &f, 0);
  if (e != ESP_OK) return e;
  if (f.len == 0 || f.len > 4096) return ESP_OK;
  uint8_t* buf = (uint8_t*)malloc(f.len + 1);
  if (!buf) return ESP_ERR_NO_MEM;
  f.payload = buf;
  e = httpd_ws_recv_frame(r, &f, f.len);
  if (e == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) {
    buf[f.len] = 0;
    controlCommand((const char*)buf);
  }
  free(buf);
  return e;
}

static void wsSendAll(void* arg) {
  char* s = (char*)arg;
  size_t fds = 10;
  int list[10];
  int c = 0;
  if (httpd_get_client_list(srv, &fds, list) == ESP_OK) {
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = (uint8_t*)s;
    f.len = strlen(s);
    for (size_t i = 0; i < fds; i++) {
      if (httpd_ws_get_fd_info(srv, list[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_send_frame_async(srv, list[i], &f);
        c++;
      }
    }
  }
  wsCount = c;
  free(s);
}

static void broadcaster(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!srv) continue;
    String s = statusJson();
    char* copy = strdup(s.c_str());
    if (!copy) continue;
    if (httpd_queue_work(srv, wsSendAll, copy) != ESP_OK) free(copy);
  }
}

// ----------------------------------------------------------- start
static void reg(const char* uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t*), bool ws = false) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = m;
  u.handler = fn;
  u.is_websocket = ws;
  httpd_register_uri_handler(srv, &u);
}

void webStart() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.max_uri_handlers = 20;
  c.stack_size = 7168;
  c.lru_purge_enable = true;
  c.max_open_sockets = 8;
  c.recv_wait_timeout = 10;
  c.send_wait_timeout = 10;
  c.core_id = 0;
  if (httpd_start(&srv, &c) != ESP_OK) {
    Serial.println("[web] failed to start");
    return;
  }
  reg("/", HTTP_GET, hIndex);
  reg("/index.html", HTTP_GET, hIndex);
  reg("/ws", HTTP_GET, hWs, true);
  reg("/api/status", HTTP_GET, hStatus);
  reg("/api/info", HTTP_GET, hInfo);
  reg("/api/lib", HTTP_GET, hLib);
  reg("/api/ls", HTTP_GET, hLs);
  reg("/api/upload", HTTP_POST, hUpload);
  reg("/api/file", HTTP_GET, hFile);
  reg("/api/rm", HTTP_POST, hRm);
  reg("/api/mkdir", HTTP_POST, hMkdir);
  reg("/api/settings", HTTP_GET, hSettingsGet);
  reg("/api/settings", HTTP_POST, hSettingsPost);
  reg("/api/reboot", HTTP_POST, hReboot);
  reg("/api/ota", HTTP_POST, hOta);
  xTaskCreatePinnedToCore(broadcaster, "wsbcast", 4096, nullptr, 3, nullptr, 0);
  Serial.println("[web] http server up");
}
