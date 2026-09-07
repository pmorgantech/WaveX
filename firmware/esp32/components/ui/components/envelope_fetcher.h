#pragma once

#include "envelope_cache.h"
#include "spi_protocol/protocol.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace wavex_ui {

/**
 * @brief Drives one MSG_ENVELOPE_REQ run at a time and files it in the cache.
 *
 * The transport half of the waveform path: asking the backend for the columns
 * a view needs, assembling the reply, and handing the completed run to
 * EnvelopeCache. The cache decides *what* to ask for; this decides *when*, and
 * survives the reply arriving on a different task.
 *
 * It exists because two pages need it. The sample edit page had the only copy,
 * and the browser's detail panel needs the same thing for a fixed whole-file
 * view. Copying it would have meant two versions of the release/acquire
 * assembly below - and `ui_sample_browser.cpp` has already had three row
 * builders drift apart from one another, which is the same mistake one layer
 * up.
 *
 * **Threading.** `onChunk()` runs on the UART RX task; all other calls run
 * on the UI task. A mutex protects the complete receive identity, progress,
 * and staging writes. The receive state is Idle, Receiving, or Ready. Only
 * Receiving accepts chunks; committing or abandoning a run returns to Idle.
 * The UI task retires a Ready run under the mutex before ingesting its buffer
 * into the cache outside the lock. No sender, cache, or LVGL code runs under
 * this mutex, so RX only waits for bounded state changes or a chunk copy.
 * EnvelopeCache remains owned exclusively by the UI task.
 *
 * **Every abandoned run must be released.** `EnvelopeCache::noteRequest()`
 * blocks all further `nextRequest()` calls until the run commits, and that
 * guard is not per-sample. A run dropped without `abort()` therefore stops
 * *every* waveform in the process from ever loading again, until reboot. That
 * is not hypothetical - it is the defect this class inherited its shape from.
 * `service()` handles the timeout case; callers handle page exit.
 *
 * No LVGL, no ESP-IDF: time arrives as a parameter and sending is injected, so
 * the whole state machine is host-testable.
 */
class EnvelopeFetcher {
   public:
    /// Returns false if the request could not be handed to the link.
    using SendFn = bool (*)(uint16_t sample_id,
                            uint16_t columns,
                            uint32_t start_frame,
                            uint32_t end_frame);

    struct Config {
        /// Most columns one run may carry; sizes the staging buffer.
        uint16_t max_run_columns = 0;
        /// How long a run may go unanswered before it is abandoned.
        uint32_t timeout_ms = 3000;
        /// Timeouts tolerated before giving up on the view entirely.
        uint8_t max_retries = 3;
    };

    /// UI task, before registering the RX listener. Allocates staging at its
    /// ceiling; unregister the listener before reinitializing or destroying.
    void init(const Config& config, SendFn send, EnvelopeCache* cache);

    bool initialized() const { return send_ != nullptr && cache_ != nullptr; }

    enum class Request : uint8_t {
        Sent,           ///< A run is now in flight; wait for service().
        AlreadyCached,  ///< The cache covers this view. Draw now, ask nothing.
        Busy,           ///< A run is already in flight; try again after it lands.
        SendFailed,     ///< The link refused it. Nothing is armed.
        NotReady,       ///< init() has not been called.
    };

    /// UI task. Asks the cache what is missing for this view and requests it.
    /// `display_columns` is the width the view will be drawn at, per call:
    /// one fetcher serves views of different widths (the edit page's
    /// continuous trace and its two splice halves), and the width is what
    /// picks the cache tier.
    Request request(uint16_t sample_id,
                    uint16_t generation,
                    uint32_t view_start,
                    uint32_t view_end,
                    uint32_t total_frames,
                    uint16_t display_columns,
                    uint32_t now_ms);

    /// UART RX task. Assembles one chunk into the staging buffer.
    void onChunk(const WaveX::Protocol::EnvelopeChunkMessage& header,
                 const WaveX::Protocol::EnvelopeColumn* columns);

    enum class Service : uint8_t {
        Idle,       ///< Nothing to do.
        Committed,  ///< A run reached the cache. Redraw, and consider asking again.
        Retrying,   ///< Timed out; the caller should re-request after its settle delay.
        GaveUp,     ///< Timed out with no retries left. Tell the user.
    };

    /// UI task. Commits a completed run, or handles a run that never answered.
    Service service(uint32_t now_ms);

    /**
     * @brief Releases an armed run that will never complete.
     *
     * Call on page exit, and anywhere else a run is abandoned. Safe when
     * nothing is armed. See the class note on why skipping this wedges every
     * waveform in the process rather than just this one.
     */
    void abort();

    bool busy() const { return in_flight_; }
    uint8_t retries() const { return retries_; }

   private:
    Config config_{};
    SendFn send_ = nullptr;
    EnvelopeCache* cache_ = nullptr;

    /// Staging for the run in flight. Written by the RX task, read by the UI
    /// task after a Ready run is retired. Protected by receive_mutex_.
    std::vector<WaveX::Protocol::EnvelopeColumn> staging_;

    /// Complete receive identity, guarded with the buffer/progress below.
    uint16_t pending_sample_id_ = 0;
    uint16_t pending_generation_ = 0;
    uint32_t pending_start_ = 0;
    uint32_t pending_end_ = 0;
    uint32_t pending_response_end_ = 0;  ///< Backend clamps the window at EOF.
    uint16_t pending_columns_ = 0;

    enum class ReceiveState : uint8_t { Idle, Receiving, Ready };
    std::mutex receive_mutex_;
    ReceiveState receive_state_ = ReceiveState::Idle;
    uint16_t received_ = 0;
    uint8_t channels_ = 0;

    bool in_flight_ = false;
    uint32_t sent_ms_ = 0;
    uint8_t retries_ = 0;
};

}  // namespace wavex_ui
