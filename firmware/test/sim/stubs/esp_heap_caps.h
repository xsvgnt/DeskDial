// Simulator stand-in for ESP-IDF's capability-based allocator.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static inline void *heap_caps_malloc(size_t n, int) { return malloc(n); }
static inline void *heap_caps_calloc(size_t a, size_t n, int) { return calloc(a, n); }
