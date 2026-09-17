#pragma once

#include "envelope_cache.h"
#include "envelope_fetcher.h"
#include "envelope_sink.h"
#include "spi_protocol/protocol.h"

#include <cstdint>
#include <vector>

namespace wavex_ui {

/**
 * @brief One sample's waveform, in up to three views, kept drawn.
 *
 * The whole request/receive/render cycle behind a waveform display, shared by
 * every page that shows one. A page hands it sinks (WaveformViews), names the
 * sample and a window per view, and calls service() from its UI tick; the
 * panel asks the cache for what each view is missing, one run at a time,
 * files replies, and re-renders every view from the cache as runs land. The
 * page reads back an Event to word its status line and nothing else.
 *
 * Before this the sample edit page and the sample browser each carried their
 * own copy of the loop - fetcher init, listener registration, the settle
 * timer, the service switch, the render - and the browser's copy went wrong
 * twice in ways the edit page's did not. A third copy for the record page
 * was the point at which one became cheaper than three.
 *
 * **Views, not one view.** The edit page shows the same sample three ways at
 * once: the continuous trace and, when a loop is set, the two halves of the
 * splice. Each is its own window at its own width, so each picks its own
 * cache tier; requesting them in turn through one fetcher, rather than one
 * fetcher each, is what keeps EnvelopeCache's single in-flight run honest.
 *
 * **Threading.** Everything here runs on the UI task except the chunk
 * trampoline, which the link calls on the UART RX task and which does nothing
 * but forward to EnvelopeFetcher::onChunk(). The link's listener is the one
 * process-wide slot for envelope chunks: attach() takes it, detach() releases
 * it, and a page that is entered while another still holds it steals it -
 * which is the intent, since only the visible page has anything to draw.
 *
 * No LVGL, no ESP-IDF: the link and the sinks are injected, so the whole
 * lifecycle is host-testable with a fake of each.
 */
class EnvelopePanel {
   public:
    using ChunkCb = void (*)(const WaveX::Protocol::EnvelopeChunkMessage& header,
                             const WaveX::Protocol::EnvelopeColumn* columns,
                             void* user);

    /// The transport. On the target, EspEnvelopeLink(); in tests, fakes.
    struct Link {
        EnvelopeFetcher::SendFn send = nullptr;
        /// Registers @p cb as the process's envelope chunk listener; a null
        /// @p cb unregisters whoever holds it.
        void (*listen)(ChunkCb cb, void* user) = nullptr;
        uint32_t (*request_cursor)(uint16_t sample_id, uint16_t generation) = nullptr;
        bool (*read_cursor)(WaveX::Protocol::SamplePlayheadMessage* out) = nullptr;
    };

    struct Config {
        /// How long a run may go unanswered before it is retried.
        uint32_t timeout_ms = 3000;
        /// Timeouts (or refused sends) tolerated before the panel gives up.
        uint8_t max_retries = 3;
        /// How long a window may keep moving before a request goes out. A
        /// scrub turns the encoder tens of times a second; asking on every
        /// tick would flood the link with runs for views already left.
        uint32_t settle_ms = 150;
    };

    /// Continuous trace plus the two splice halves. A page with fewer views
    /// attaches fewer sinks.
    static constexpr uint8_t kMaxViews = 3;

    enum class Event : uint8_t {
        None,        ///< Nothing changed.
        Drawn,       ///< At least one view was (re)rendered.
        Retrying,    ///< A run timed out; another goes out after the settle.
        GaveUp,      ///< Out of retries. Nothing more until setSample().
        SendFailed,  ///< The link refused a request; retried after the settle.
    };

    /**
     * @brief Takes the link and the listener slot; UI task, page onEnter.
     *
     * @p sinks are borrowed for the life of the attachment and must outlive
     * detach(). Every view starts enabled with an empty window; a view whose
     * window is empty is skipped, not requested, so a page may attach sinks
     * it will only name windows for later.
     */
    void attach(const Config& config,
                const Link& link,
                EnvelopeCache* cache,
                EnvelopeSink* const* sinks,
                uint8_t count);

    /// Releases the listener slot and any run in flight. Page onExit. Safe to
    /// call unattached, and safe to call twice.
    void detach();

    bool attached() const { return cache_ != nullptr; }

    /**
     * @brief Names the sample every view shows. Requests on the next service().
     *
     * Drops whatever was in flight for the previous sample and clears every
     * sink, so a sink never keeps showing the last sample while the new one's
     * data is still on its way. Calling it again with the same identity is
     * NOT a no-op: it is how a page forces a fresh start after give-up.
     */
    void setSample(uint16_t sample_id, uint16_t generation, uint32_t total_frames);

    /// No sample. Clears every sink and asks nothing until setSample().
    void clearSample();

    bool hasSample() const { return has_sample_; }

    /// The frames [@p start, @p end) view @p index shows. A change is
    /// requested after the settle; an unchanged window costs nothing.
    void setWindow(uint8_t index, uint32_t start, uint32_t end);

    /// A disabled view is neither requested nor rendered. Toggling it back on
    /// renders it from the cache at once if the data is there.
    void enableView(uint8_t index, bool enabled);

    /// Re-renders every enabled view from the cache on the next service(),
    /// including views already drawn complete.
    void redraw();

    /// Skips the settle for the next request.
    void requestNow();

    /// UI task, every tick. Files landed runs, requests what is missing once
    /// the settle has passed, renders what changed.
    Event service(uint32_t now_ms);

    bool busy() const { return fetcher_.busy(); }
    bool gaveUp() const { return gave_up_; }
    /// True once view @p index has been rendered complete - every column
    /// covered - for the current sample.
    bool drawn(uint8_t index) const;
    uint8_t retries() const { return retries_; }

   private:
    struct View {
        EnvelopeSink* sink = nullptr;
        uint32_t start = 0;
        uint32_t end = 0;
        bool enabled = true;
        bool drawn = false;  ///< Complete at its own tier; nothing more to render.
        /// What the last render of this window came from, so a commit that
        /// changed nothing for this view does not hand the sink the same
        /// columns again and cost a redraw. Same tier and same coverage from
        /// one generation of one sample is the same data.
        uint32_t shown_fpc = 0;
        uint16_t shown_covered = 0;
    };

    /// Forget what a view shows, so the next render() reaches the sink.
    static void invalidate(View& v) {
        if (v.sink)
            v.sink->setPlaybackPosition(false, 0, 0, 0);
        v.drawn = false;
        v.shown_fpc = 0;
        v.shown_covered = 0;
    }

    static void chunkTrampoline(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                const WaveX::Protocol::EnvelopeColumn* columns,
                                void* user);

    void scheduleRequest(uint32_t at_ms);
    bool requestDue(uint32_t now_ms) const;
    /// Asks for the first thing any enabled view is missing. Returns true if
    /// nothing was: every view is fully cached.
    bool requestMissing(uint32_t now_ms, Event& event);
    void render();
    void clearSinks();
    void serviceCursor(uint32_t now_ms);
    void resetCursor();
    uint32_t cursor_request_ = 0;
    uint32_t cursor_sent_at_ = 0;
    uint32_t cursor_received_at_ = 0;
    bool cursor_polled_ = false;
    bool cursor_valid_ = false;
    WaveX::Protocol::SamplePlayheadMessage cursor_{};

    Config config_{};
    Link link_{};
    EnvelopeCache* cache_ = nullptr;
    EnvelopeFetcher fetcher_;

    /// Render scratch, sized once at attach() for the widest sink in stereo.
    std::vector<WaveX::Protocol::EnvelopeColumn> buffer_;

    View views_[kMaxViews];
    uint8_t view_count_ = 0;

    uint16_t sample_id_ = 0;
    uint16_t generation_ = 0;
    uint32_t total_frames_ = 0;
    bool has_sample_ = false;

    /// A window or an enable moved since the last service(); request after
    /// the settle.
    bool changed_ = false;
    /// A request is wanted: at request_at_, or now if immediate_. A flag
    /// rather than a sentinel time so a wrapped clock cannot be mistaken for
    /// one.
    bool request_pending_ = false;
    bool immediate_ = false;
    uint32_t request_at_ = 0;

    bool dirty_ = false;
    bool gave_up_ = false;
    uint8_t retries_ = 0;
};

/// The target's link: inter_mcu_send_envelope_req() and the RX task's chunk
/// listener slot. Defined in envelope_panel_link.cpp, target-only.
EnvelopePanel::Link EspEnvelopeLink();

}  // namespace wavex_ui
