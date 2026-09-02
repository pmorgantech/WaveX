#pragma once

#include "spi_protocol/protocol.h"

#include <cstddef>
#include <cstdint>

namespace wavex_ui {

/**
 * @brief Mip-mapped waveform envelope cache (roadmap 1.5.5 items 3 and 4).
 *
 * Every waveform redraw used to be a round trip: the ESP32 asked, the Daisy
 * re-read from SD, decimated, and streamed the result back, so zooming or
 * moving a marker off-window refetched from scratch. That round trip is the
 * only reason the edit page needs a request debounce at all.
 *
 * This holds envelope runs at power-of-two decimation TIERS, keyed by sample
 * id *and* content generation. A tier's frames-per-column is chosen so the
 * cached data is at least as fine as the display asking for it, which makes
 * merging tier columns into display columns exact rather than approximate:
 * the min of a set of minima is the true minimum.
 *
 * Two properties worth stating, because they are what make it safe:
 *
 * - **Single-threaded by construction.** Nothing here takes a lock. Chunks
 *   arrive on the UART RX task, but the page hands them over on the UI task,
 *   which is also the only caller of render(). Doing LVGL work - or, here,
 *   allocator work - from the RX task is the mistake that froze the edit page
 *   once already.
 * - **Generation-keyed.** Marker and gain edits do not change the audio, so
 *   they must not throw the cache away; a destructive render does, and bumps
 *   SampleMetadata::generation. An entry from generation N can never be read
 *   for generation N+1, because the generation is part of the key rather than
 *   something checked after the fact.
 *
 * Memory comes from an injected allocator so this stays host-testable and so
 * the caller - not this class - decides how much PSRAM it may take. Measure
 * the budget against actual free PSRAM: LVGL's draw buffers and the display
 * rotation path are already the largest consumers there, and a cache that
 * starves them trades a fast waveform for a slow UI.
 */
class EnvelopeCache {
   public:
    struct Allocator {
        void* (*alloc)(size_t bytes) = nullptr;
        void (*release)(void* ptr) = nullptr;
    };

    /// Runs held at once. The edit page uses two or three tiers of one sample
    /// while zooming; the rest of the slots absorb a sample switch without
    /// immediately evicting what the user came back from.
    static constexpr size_t kMaxEntries = 12;

    /// Widest run a single entry may hold: 4096 columns is 32 KB stereo, and
    /// caps what one over-eager request can take out of the budget.
    static constexpr uint32_t kMaxEntryColumns = 4096;

    ~EnvelopeCache();

    void init(size_t budget_bytes, const Allocator& allocator);
    void reset();  ///< Drops every entry and any request in flight.

    bool initialized() const { return alloc_.alloc != nullptr; }

    /**
     * @brief Frames per column for the tier serving this view.
     *
     * The largest power of two no coarser than one display column, so a tier
     * column never spans more audio than a pixel does and the merge in
     * render() cannot invent detail it does not have. Never 0.
     */
    static uint32_t tierFramesPerColumn(uint32_t span_frames, uint16_t display_columns);

    /**
     * @brief The next run that has to come from the backend to draw this view.
     *
     * Returns false when the cache already covers the window - which is the
     * whole point: a zoom step that lands inside a cached tier costs nothing.
     * When it returns true the caller sends one MSG_ENVELOPE_REQ with these
     * values and calls noteRequest() with the same ones. Only ever ONE run at
     * a time: a wide view can need more columns than a packet run holds, and
     * asking for the rest after the first arrives keeps the staging buffer to
     * a single run.
     */
    bool nextRequest(uint16_t sample_id,
                     uint16_t generation,
                     uint32_t view_start,
                     uint32_t view_end,
                     uint32_t total_frames,
                     uint16_t display_columns,
                     uint16_t max_columns_per_request,
                     uint32_t& req_start,
                     uint32_t& req_end,
                     uint16_t& req_columns) const;

    /// Arms the receiver for the run just requested. A chunk that does not
    /// match is dropped: the frames-per-column of a run is not recoverable
    /// from the reply alone once the backend clamps the last column at EOF,
    /// so the requester's own view of the run is the authority.
    void noteRequest(uint16_t sample_id,
                     uint16_t generation,
                     uint32_t req_start,
                     uint32_t req_end,
                     uint16_t req_columns);

    /// Takes one chunk. Returns true once the run it belongs to is complete
    /// and has been committed to an entry.
    bool ingest(const WaveX::Protocol::EnvelopeChunkMessage& header,
                const WaveX::Protocol::EnvelopeColumn* columns);

    /**
     * @brief Disarms a run that will never complete.
     *
     * noteRequest() blocks every further nextRequest() until the run it armed
     * commits, which is what keeps the staging buffer to a single run. But a
     * run can be abandoned rather than completed - the send can fail, or the
     * backend can drop a scan silently when the sample under it is reloaded -
     * and without this there is no way back: the cache stays armed for a reply
     * that is not coming, nextRequest() refuses everything (the guard is not
     * per-sample), and nothing is ever requested or drawn again for the rest of
     * the boot. Every caller that gives up on a run MUST call this.
     */
    void abortPending();

    /// True while a run is armed and not yet committed. For assertions and
    /// tests; callers drive the lifecycle with noteRequest()/abortPending().
    bool requestPending() const { return pending_.active; }

    /**
     * @brief Merges cached tier columns into display columns.
     *
     * Writes `display_columns * out_channels` values, channel-interleaved per
     * column. Display columns with no cached data are written as silence, so a
     * partially filled view draws progressively rather than not at all.
     *
     * @return how many display columns were backed by real data (0 = nothing).
     */
    uint16_t render(uint16_t sample_id,
                    uint16_t generation,
                    uint32_t view_start,
                    uint32_t view_end,
                    uint16_t display_columns,
                    WaveX::Protocol::EnvelopeColumn* out,
                    size_t out_capacity,
                    uint8_t& out_channels) const;

    /// Drops every entry for a sample. For unload, not for edits: markers and
    /// gain do not change the audio the envelope describes.
    void invalidateSample(uint16_t sample_id);

    size_t bytesUsed() const { return bytes_used_; }
    size_t budgetBytes() const { return budget_bytes_; }
    size_t entryCount() const;

   private:
    struct Entry {
        bool used = false;
        uint16_t sample_id = 0;
        uint16_t generation = 0;
        uint32_t fpc = 0;           // frames per column at this tier
        uint32_t first_column = 0;  // tier column index of the first column held
        uint32_t columns = 0;
        uint8_t channels = 1;
        mutable uint32_t last_used = 0;  // render() is const but still touches LRU
        WaveX::Protocol::EnvelopeColumn* data = nullptr;
    };

    struct Pending {
        bool active = false;
        uint16_t sample_id = 0;
        uint16_t generation = 0;
        uint32_t start_frame = 0;
        uint32_t end_frame = 0;
        uint32_t fpc = 0;
        uint32_t first_column = 0;
        uint16_t columns = 0;
        uint16_t received = 0;  // contiguous columns filled from column 0
        uint8_t channels = 0;   // 0 until the first chunk names it
        WaveX::Protocol::EnvelopeColumn* staging = nullptr;
        size_t staging_bytes = 0;
    };

    const Entry* findCovering(uint16_t sample_id,
                              uint16_t generation,
                              uint32_t view_start,
                              uint32_t view_end) const;
    Entry* findRun(uint16_t sample_id, uint16_t generation, uint32_t fpc, uint8_t channels);
    void releaseEntry(Entry& e);
    bool makeRoom(size_t bytes_needed);
    bool commitPending();

    Allocator alloc_{};
    size_t budget_bytes_ = 0;
    size_t bytes_used_ = 0;
    mutable uint32_t tick_ = 0;
    Entry entries_[kMaxEntries]{};
    Pending pending_{};
};

/// Process-wide cache, shared by every page that draws a waveform. The edit
/// page and the browser detail panel must not each hold their own copy of the
/// same envelope.
EnvelopeCache& GetEnvelopeCache();

/**
 * @brief Initialises the shared cache against actual free PSRAM, once.
 *
 * Idempotent, so every page that draws a waveform can call it on entry without
 * caring who got there first. Defined in envelope_cache_init.cpp, which is
 * firmware-only: the budget comes from heap_caps and there is no PSRAM on the
 * host.
 */
void EnsureEnvelopeCacheInitialised();

}  // namespace wavex_ui
