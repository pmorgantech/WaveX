/* =====================================================================
 *  Sample RAM Manager (header-only): small-slab + large-extent
 *  FILE: include/wxsamp_mem.h
 * ===================================================================== */
#pragma once
#include <stdint.h>
#include <string.h>

// ===== Configuration =====
// Tune these to your Daisy SDRAM layout and usage profile.
#ifndef WXM_SMALL_PAGE_BYTES
#define WXM_SMALL_PAGE_BYTES   4096u      // slab page size for tiny samples
#endif
#ifndef WXM_LARGE_PAGE_BYTES
#define WXM_LARGE_PAGE_BYTES   (64u*1024u) // extent page size for large samples
#endif
#ifndef WXM_SMALL_CLASS_COUNT
#define WXM_SMALL_CLASS_COUNT  6          // number of size classes below
#endif
#ifndef WXM_SMALL_CLASSES_BYTES
#define WXM_SMALL_CLASSES_BYTES {32u, 64u, 128u, 256u, 512u, 1024u}
#endif
#ifndef WXM_SMALL_MAX_PAGES_PER_CLASS
#define WXM_SMALL_MAX_PAGES_PER_CLASS  64 // up to 64 * 4KB = 256KB per class (tweak)
#endif
#ifndef WXM_LARGE_MAX_RUNS
#define WXM_LARGE_MAX_RUNS  512          // free list capacity for extent pool
#endif

// ===== Public handle type =====
typedef struct {
    uint8_t  cls;     // 0..(WXM_SMALL_CLASS_COUNT-1) for small; 0xFF for large
    uint16_t page;    // small: page index within class; large: first page index
    uint16_t slot;    // small: slot index in page; large: page_count
    uint32_t len;     // logical bytes
    uint16_t refcnt;  // reference count for sharing
} wxsamp_t;

// ===== Stats =====
typedef struct {
    // small pool per class
    struct { uint16_t block; uint16_t pages; uint16_t slots_total; uint16_t slots_free; } small[WXM_SMALL_CLASS_COUNT];
    uint32_t small_total_bytes; uint32_t small_free_bytes;
    // large pool
    uint32_t large_total_bytes; uint32_t large_free_bytes; uint32_t largest_free_bytes;
    // global
    uint32_t in_use_bytes; uint32_t objects_alive; uint32_t failed_allocs;
} wxsamp_stats_t;

// ===== Small slab pool =====
namespace wxsamp_internal {
struct SlabPage {
    uint32_t base_off;        // offset in small region
    uint16_t used;            // number of allocated slots
    uint16_t slots;           // total slots in this page
    // bitmap: 1 = free, 0 = used (so ffz-style search works)
    // dynamically sized per page in code; here we cap by max slots for each class
    uint32_t bm[WXM_SMALL_PAGE_BYTES / 32u]; // upper entries unused; safe cap
};
}

class SmallSlabPool {
  public:
    void init(uint8_t* base, uint32_t bytes) {
        base_ = base; size_ = bytes;
        memset(class_pages_, 0, sizeof(class_pages_));
        memset(class_page_count_, 0, sizeof(class_page_count_));
        memset(&stats_, 0, sizeof(stats_));
        // Stage no pages yet; pages are formatted lazily on first alloc
        // Pre-warm common classes if desired via add_pages_for_class()
    }

    // Optional pre-allocation for a class: adds N fresh pages to that class
    bool add_pages_for_class(uint8_t cls, uint16_t count) {
        if (cls >= WXM_SMALL_CLASS_COUNT) return false;
        if (!ensure_space(count * WXM_SMALL_PAGE_BYTES)) return false;
        for (uint16_t i = 0; i < count; ++i) format_new_page(cls);
        return true;
    }

    bool alloc(uint32_t nbytes, wxsamp_t* out) {
        uint8_t cls; uint16_t bsize; if (!pick_class(nbytes, &cls, &bsize)) return false;
        // Find a page in this class with a free slot, else create new page
        uint16_t pcnt = class_page_count_[cls];
        for (uint16_t p = 0; p < pcnt; ++p) {
            auto& pg = class_pages_[cls][p];
            if (pg.used < pg.slots) {
                int slot = pop_free_slot(pg);
                if (slot >= 0) {
                    pg.used++;
                    fill_handle_small(*out, cls, p, (uint16_t)slot, nbytes);
                    stats_.objects_alive++; stats_.in_use_bytes += bsize;
                    return true;
                }
            }
        }
        // Need a new page
        if (!format_new_page(cls)) return false;
        auto& pg = class_pages_[cls][class_page_count_[cls]-1];
        int slot = pop_free_slot(pg);
        if (slot < 0) return false; // should not happen
        pg.used++;
        fill_handle_small(*out, cls, class_page_count_[cls]-1, (uint16_t)slot, nbytes);
        stats_.objects_alive++; stats_.in_use_bytes += bsize;
        return true;
    }

    bool ptr(const wxsamp_t& h, void** out_ptr) const {
        if (h.cls >= WXM_SMALL_CLASS_COUNT) return false;
        auto& pg = class_pages_[h.cls][h.page];
        uint32_t bsize = class_block_[h.cls];
        *out_ptr = base_ + pg.base_off + (uint32_t)h.slot * bsize;
        return true;
    }

    bool release(wxsamp_t* h) {
        if (h->cls >= WXM_SMALL_CLASS_COUNT) return false;
        auto& pg = class_pages_[h->cls][h->page];
        uint32_t bsize = class_block_[h->cls];
        if (!mark_slot_free(pg, h->slot)) return false;
        if (pg.used) pg.used--; // guard
        stats_.in_use_bytes = (stats_.in_use_bytes >= bsize) ? (stats_.in_use_bytes - bsize) : 0;
        stats_.objects_alive = (stats_.objects_alive ? stats_.objects_alive - 1 : 0);
        // Optional: reclaim empty pages back to unformatted space (kept simple)
        return true;
    }

    void get_stats(wxsamp_stats_t& s) const {
        (void)s; // caller will merge
    }

    // Expose per-class stats snapshot
    void fill_small_stats(wxsamp_stats_t& s) const {
        uint32_t total=0, freeb=0;
        for (uint8_t c=0;c<WXM_SMALL_CLASS_COUNT;++c){
            uint16_t b = class_block_[c];
            uint32_t slots_total=0, slots_free=0; uint16_t pages = class_page_count_[c];
            for (uint16_t p=0;p<pages;++p){
                const auto& pg = class_pages_[c][p];
                slots_total += pg.slots;
                slots_free  += (pg.slots - pg.used);
                total += pg.slots * b;
                freeb += (pg.slots - pg.used) * b;
            }
            s.small[c] = { b, pages, (uint16_t)slots_total, (uint16_t)slots_free };
        }
        s.small_total_bytes = total; s.small_free_bytes = freeb;
        s.in_use_bytes = stats_.in_use_bytes; s.objects_alive = stats_.objects_alive; s.failed_allocs = stats_.failed_allocs;
    }

  private:
    static constexpr uint16_t class_block_[WXM_SMALL_CLASS_COUNT] = WXM_SMALL_CLASSES_BYTES;

    uint8_t*  base_ = nullptr; // start of small region
    uint32_t  size_ = 0;       // bytes of small region
    uint32_t  used_bytes_ = 0; // formatted pages total bytes

    wxsamp_internal::SlabPage class_pages_[WXM_SMALL_CLASS_COUNT][WXM_SMALL_MAX_PAGES_PER_CLASS];
    uint16_t class_page_count_[WXM_SMALL_CLASS_COUNT];

    struct { uint32_t in_use_bytes; uint32_t objects_alive; uint32_t failed_allocs; } stats_{};

    bool pick_class(uint32_t n, uint8_t* out_cls, uint16_t* out_bsize){
        for (uint8_t i=0;i<WXM_SMALL_CLASS_COUNT;++i) {
            if (n <= class_block_[i]) { *out_cls=i; *out_bsize=class_block_[i]; return true; }
        }
        return false;
    }

    bool ensure_space(uint32_t add_bytes){
        if (used_bytes_ + add_bytes > size_) { stats_.failed_allocs++; return false; }
        return true;
    }

    bool format_new_page(uint8_t cls){
        if (class_page_count_[cls] >= WXM_SMALL_MAX_PAGES_PER_CLASS) { stats_.failed_allocs++; return false; }
        if (!ensure_space(WXM_SMALL_PAGE_BYTES)) return false;
        auto& pg = class_pages_[cls][class_page_count_[cls]];
        pg.base_off = used_bytes_;
        pg.used = 0;
        pg.slots = (uint16_t)(WXM_SMALL_PAGE_BYTES / class_block_[cls]);
        const uint32_t words = ( (pg.slots + 31u) / 32u );
        for (uint32_t i=0;i<words;i++) pg.bm[i] = 0xFFFFFFFFu; // all free
        // Mask off extra bits beyond slots
        const uint32_t extra = words*32u - pg.slots;
        if (extra) pg.bm[words-1] &= (0xFFFFFFFFu >> extra);
        used_bytes_ += WXM_SMALL_PAGE_BYTES;
        class_page_count_[cls]++;
        return true;
    }

    static inline int ffs32(uint32_t x){ return x ? __builtin_ctz(x) : -1; }

    static int pop_free_slot(wxsamp_internal::SlabPage& pg){
        const uint32_t words = ( (pg.slots + 31u) / 32u );
        for (uint32_t i=0;i<words;i++){
            uint32_t w = pg.bm[i];
            if (!w) continue; // no free bit
            int bit = ffs32(w);
            if (bit >= 0) {
                pg.bm[i] &= ~(1u << bit); // mark used
                return (int)(i*32u + bit);
            }
        }
        return -1;
    }

    static bool mark_slot_free(wxsamp_internal::SlabPage& pg, uint16_t slot){
        if (slot >= pg.slots) return false;
        uint32_t i = slot / 32u; uint32_t bit = slot % 32u;
        uint32_t mask = (1u << bit);
        if (pg.bm[i] & mask) return false; // was already free
        pg.bm[i] |= mask; return true;
    }

    static void fill_handle_small(wxsamp_t& h, uint8_t cls, uint16_t page, uint16_t slot, uint32_t len){
        h.cls = cls; h.page = page; h.slot = slot; h.len = len; h.refcnt = 1;
    }
};

// ===== Large extent pool (64KB pages, coalescing runs) =====
class LargeExtentPool {
  public:
    void init(uint8_t* base, uint32_t bytes){
        base_ = base; size_ = (bytes / WXM_LARGE_PAGE_BYTES) * WXM_LARGE_PAGE_BYTES; // page-align down
        run_count_ = 1; free_runs_[0] = {0u, pages_total()};
    }

    bool alloc(uint32_t nbytes, wxsamp_t* out){
        uint32_t need_pages = (nbytes + WXM_LARGE_PAGE_BYTES - 1u) / WXM_LARGE_PAGE_BYTES;
        int idx = find_best_fit(need_pages);
        if (idx < 0) return false;
        uint32_t first = free_runs_[idx].first;
        if (free_runs_[idx].count == need_pages) remove_run(idx);
        else { free_runs_[idx].first += need_pages; free_runs_[idx].count -= need_pages; }
        fill_handle_large(*out, first, (uint16_t)need_pages, nbytes);
        return true;
    }

    bool ptr(const wxsamp_t& h, void** out_ptr) const {
        if (h.cls != 0xFF) return false;
        *out_ptr = base_ + (uint32_t)h.page * WXM_LARGE_PAGE_BYTES;
        return true;
    }

    bool release(wxsamp_t* h){
        if (h->cls != 0xFF) return false;
        insert_run({h->page, h->slot});
        coalesce();
        return true;
    }

    void fill_large_stats(wxsamp_stats_t& s) const {
        s.large_total_bytes = size_;
        s.large_free_bytes = free_bytes();
        s.largest_free_bytes = largest_free_bytes();
    }

  private:
    struct Run { uint32_t first; uint32_t count; };
    uint8_t* base_ = nullptr; uint32_t size_ = 0;
    Run free_runs_[WXM_LARGE_MAX_RUNS]; uint16_t run_count_ = 0;

    inline uint32_t pages_total() const { return size_ / WXM_LARGE_PAGE_BYTES; }

    int find_best_fit(uint32_t need){
        int best=-1; uint32_t best_count=0xFFFFFFFFu;
        for (int i=0;i<run_count_;++i){ auto& r=free_runs_[i]; if (r.count>=need && r.count<best_count){ best=i; best_count=r.count; } }
        return best;
    }

    void remove_run(int idx){ for (int i=idx;i<run_count_-1;++i) free_runs_[i]=free_runs_[i+1]; run_count_--; }

    void insert_run(Run r){ if (run_count_ < WXM_LARGE_MAX_RUNS) free_runs_[run_count_++] = r; }

    void coalesce(){
        // simple insertion sort by first
        for (int i=1;i<run_count_;++i){ Run key=free_runs_[i]; int j=i-1; while(j>=0 && free_runs_[j].first>key.first){ free_runs_[j+1]=free_runs_[j]; j--; } free_runs_[j+1]=key; }
        // merge neighbors
        int w=0; for (int i=0;i<run_count_;++i){ if (w==0) { free_runs_[w++]=free_runs_[i]; continue; } auto& prev=free_runs_[w-1]; auto cur=free_runs_[i]; if (prev.first+prev.count==cur.first){ prev.count+=cur.count; } else { free_runs_[w++]=cur; } }
        run_count_=w;
    }

    uint32_t free_bytes() const { uint64_t s=0; for (int i=0;i<run_count_;++i) s += (uint64_t)free_runs_[i].count * WXM_LARGE_PAGE_BYTES; return (uint32_t)s; }
    uint32_t largest_free_bytes() const { uint32_t m=0; for (int i=0;i<run_count_;++i){ uint32_t b=free_runs_[i].count*WXM_LARGE_PAGE_BYTES; if (b>m) m=b; } return m; }

    static void fill_handle_large(wxsamp_t& h, uint32_t first_page, uint16_t page_count, uint32_t len){
        h.cls = 0xFF; h.page = (uint16_t)first_page; h.slot = page_count; h.len = len; h.refcnt = 1;
    }
};

// ===== Unified manager =====
class SampleMemMgr {
  public:
    // Provide a single contiguous SDRAM region; we'll carve small/large split
    void init(void* sdram_base, uint32_t sdram_bytes, uint32_t small_bytes){
        uint8_t* base = static_cast<uint8_t*>(sdram_base);
        uint32_t sb = (small_bytes / WXM_SMALL_PAGE_BYTES) * WXM_SMALL_PAGE_BYTES;
        small_.init(base, sb);
        large_.init(base + sb, sdram_bytes - sb);
    }

    // Route allocation by threshold
    bool alloc(uint32_t nbytes, wxsamp_t* out){
        if (nbytes <= class_threshold_bytes()) {
            if (small_.alloc(nbytes, out)) return true;
            // fallback: try large if small exhausted
            return large_.alloc(nbytes, out);
        }
        return large_.alloc(nbytes, out);
    }

    // Direct pointer for read access
    bool ptr(const wxsamp_t& h, void** out_ptr){
        if (h.cls == 0xFF) return large_.ptr(h, out_ptr);
        return small_.ptr(h, out_ptr);
    }

    void retain(wxsamp_t* h){ if (h) h->refcnt++; }
    void release(wxsamp_t* h){ if (!h) return; if (h->refcnt && --h->refcnt==0){ if (h->cls==0xFF) large_.release(h); else small_.release(h); *h = {}; } }

    void stats(wxsamp_stats_t* s){ memset(s,0,sizeof(*s)); small_.fill_small_stats(*s); large_.fill_large_stats(*s); }

    // Optional: prewarm N pages for a class
    bool prewarm_small(uint8_t cls, uint16_t pages){ return small_.add_pages_for_class(cls, pages); }

    static constexpr uint32_t class_threshold_bytes(){
        // largest small class size
        static constexpr uint16_t sizes[WXM_SMALL_CLASS_COUNT] = WXM_SMALL_CLASSES_BYTES;
        return sizes[WXM_SMALL_CLASS_COUNT-1];
    }

  private:
    SmallSlabPool  small_;
    LargeExtentPool large_;
};

/* =====================================================================
 *  Example usage on Daisy (init + quick test)
 * ===================================================================== */
#ifdef WXM_MEM_TEST
#include <assert.h>
static void wxm_mem_test()
{
    static uint8_t fake_sdram[2*1024*1024]; // 2 MiB test arena
    SampleMemMgr mm; mm.init(fake_sdram, sizeof(fake_sdram), 256*1024); // 256KB small pool

    // small alloc
    wxsamp_t a{}; bool ok = mm.alloc(200, &a); assert(ok && a.cls != 0xFF);
    void* p = nullptr; mm.ptr(a, &p); assert(p != nullptr);

    // large alloc ~200KB
    wxsamp_t b{}; ok = mm.alloc(200*1024, &b); assert(ok && b.cls == 0xFF);

    // stats
    wxsamp_stats_t st; mm.stats(&st); assert(st.in_use_bytes > 0);

    // free
    mm.release(&a); mm.release(&b);
}
#endif

/* =====================================================================
 *  Integration notes
 *  - Place SampleMemMgr in your Daisy app singleton; call init() after SDRAM bring-up.
 *  - Route all sample loads through alloc(); copy bytes into mm.ptr(handle) buffer(s).
 *  - Never malloc/free during audio; retain()/release() from voice engine as pads are assigned.
 *  - Expose stats() to your diagnostics UI.
 * ===================================================================== */
