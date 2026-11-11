// Allocator shim for the Helix MP3 decoder.
// Decoder state goes in fast internal RAM when possible (PSRAM fallback).
#include <stdlib.h>
#include "helix_memory.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
void* helix_malloc(int size) {
  void* p = heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  return p;
}
void helix_free(void* p) { heap_caps_free(p); }
#else
void* helix_malloc(int size) { return calloc(1, size); }
void helix_free(void* p) { free(p); }
#endif
