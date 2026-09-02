// Sizes and initialises the shared envelope cache from PSRAM.
//
// Split out of the sample edit page when the sample browser's detail panel
// became a second consumer. It is deliberately NOT in envelope_cache.cpp: that
// file is compiled for the host test suite, where heap_caps and PSRAM do not
// exist, and the cache stays allocator-injected precisely so it can be tested
// without them. This is the one place that knows the allocator is PSRAM.

#include <esp_heap_caps.h>

#include "envelope_cache.h"

namespace wavex_ui {
namespace {

// PSRAM the envelope cache may take. Measured against what is actually free
// rather than a board spec, and capped: LVGL's draw buffers and the display
// rotation path are already the largest consumers of the same pool, and a
// cache that starves them trades a fast waveform for a slow UI.
constexpr size_t kCacheBudgetMin = 128u * 1024u;
constexpr size_t kCacheBudgetMax = 2048u * 1024u;

void* CacheAlloc(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void CacheFree(void* p) {
    heap_caps_free(p);
}

}  // namespace

void EnsureEnvelopeCacheInitialised() {
    EnvelopeCache& cache = GetEnvelopeCache();
    if (cache.initialized()) {
        return;
    }
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = free_psram / 8;  // an eighth of what is left, not of the spec
    if (budget < kCacheBudgetMin) {
        budget = (free_psram > kCacheBudgetMin) ? kCacheBudgetMin : free_psram / 2;
    }
    if (budget > kCacheBudgetMax) {
        budget = kCacheBudgetMax;
    }
    EnvelopeCache::Allocator allocator;
    allocator.alloc = &CacheAlloc;
    allocator.release = &CacheFree;
    cache.init(budget, allocator);
}

}  // namespace wavex_ui
