#include "envelope_fetcher.h"

#include <algorithm>

namespace wavex_ui {

void EnvelopeFetcher::init(const Config& config, SendFn send, EnvelopeCache* cache) {
    config_ = config;
    send_ = send;
    cache_ = cache;

    // At its ceiling, in stereo, once. See the header on why this is never
    // resized afterwards.
    staging_.assign(static_cast<size_t>(config.max_run_columns) * 2,
                    WaveX::Protocol::EnvelopeColumn());

    // Initialize the target mutex before registering the RX listener.
    std::lock_guard<std::mutex> lock(receive_mutex_);
    receive_state_ = ReceiveState::Idle;
    received_ = 0;
    channels_ = 0;
    in_flight_ = false;
    retries_ = 0;
}

EnvelopeFetcher::Request EnvelopeFetcher::request(uint16_t sample_id,
                                                  uint16_t generation,
                                                  uint32_t view_start,
                                                  uint32_t view_end,
                                                  uint32_t total_frames,
                                                  uint16_t display_columns,
                                                  uint32_t now_ms) {
    if (!initialized() || staging_.empty()) {
        return Request::NotReady;
    }
    if (in_flight_) {
        return Request::Busy;  // one run at a time; service() re-enters here
    }

    uint32_t req_start = 0;
    uint32_t req_end = 0;
    uint16_t req_columns = 0;
    if (!cache_->nextRequest(sample_id,
                             generation,
                             view_start,
                             view_end,
                             total_frames,
                             display_columns,
                             config_.max_run_columns,
                             req_start,
                             req_end,
                             req_columns)) {
        return Request::AlreadyCached;
    }

    // Publish the entire identity before send(), which can immediately result
    // in a reply. No RX copy can overlap retiring/rearming this identity.
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        received_ = 0;
        channels_ = 0;
        pending_sample_id_ = sample_id;
        pending_generation_ = generation;
        pending_start_ = req_start;
        pending_end_ = req_end;
        pending_response_end_ = std::min(req_end, total_frames);
        pending_columns_ = req_columns;
        receive_state_ = ReceiveState::Receiving;
    }

    // Send BEFORE arming the cache. noteRequest() blocks every later
    // nextRequest() until the run commits, so arming first and then failing to
    // send left the cache waiting on a reply that was never asked for - and
    // because the send failure also skipped in_flight_, the timeout never ran
    // either. Both halves have to be armed together or not at all. Safe to
    // order this way: ingest() is only reached from service() on this task, so
    // no chunk can be filed between the send and the noteRequest().
    if (!send_(sample_id, req_columns, req_start, req_end)) {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        receive_state_ = ReceiveState::Idle;
        return Request::SendFailed;
    }
    cache_->noteRequest(sample_id, generation, req_start, req_end, req_columns);
    in_flight_ = true;
    sent_ms_ = now_ms;
    return Request::Sent;
}

void EnvelopeFetcher::onChunk(const WaveX::Protocol::EnvelopeChunkMessage& header,
                              const WaveX::Protocol::EnvelopeColumn* columns) {
    std::lock_guard<std::mutex> lock(receive_mutex_);
    if (!columns || receive_state_ != ReceiveState::Receiving || staging_.empty()) {
        return;  // nothing armed, or the last run is still waiting to be filed
    }
    if (header.sample_id != pending_sample_id_ || header.generation != pending_generation_ ||
        header.start_frame != pending_start_ || header.end_frame != pending_response_end_ ||
        header.total_columns != pending_columns_) {
        return;  // a reply to a view the user has already left
    }
    if (header.channels == 0 || header.channels > 2 || header.columns == 0) {
        return;
    }
    const uint8_t seen_channels = channels_;
    if (seen_channels != 0 && header.channels != seen_channels) {
        return;
    }
    const uint32_t end_column = static_cast<uint32_t>(header.first_column) + header.columns;
    if (end_column > pending_columns_ ||
        static_cast<size_t>(end_column) * header.channels > staging_.size()) {
        return;
    }
    // A resend after a full TX queue repeats columns rather than reordering
    // them, so overlap is expected and a real gap is not.
    if (header.first_column > received_) {
        return;
    }

    std::copy(columns,
              columns + static_cast<size_t>(header.columns) * header.channels,
              staging_.begin() + static_cast<size_t>(header.first_column) * header.channels);

    channels_ = header.channels;
    if (end_column > received_) {
        received_ = static_cast<uint16_t>(end_column);
    }
    if (received_ >= pending_columns_) {
        receive_state_ = ReceiveState::Ready;
    }
}

EnvelopeFetcher::Service EnvelopeFetcher::service(uint32_t now_ms) {
    if (!initialized()) {
        return Service::Idle;
    }

    bool complete = false;
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        complete = receive_state_ == ReceiveState::Ready;
        if (!complete && (!in_flight_ || (now_ms - sent_ms_) < config_.timeout_ms)) {
            return Service::Idle;
        }
        // Retire before ingest/timeout. RX cannot mutate staging until this
        // UI task explicitly requests another run after service() returns.
        receive_state_ = ReceiveState::Idle;
    }

    if (complete) {
        WaveX::Protocol::EnvelopeChunkMessage header;
        header.sample_id = pending_sample_id_;
        header.generation = pending_generation_;
        header.start_frame = pending_start_;
        header.end_frame = pending_end_;
        header.total_columns = pending_columns_;
        header.first_column = 0;
        header.columns = pending_columns_;
        header.channels = channels_;

        if (cache_->ingest(header, staging_.data())) {
            in_flight_ = false;
            retries_ = 0;
            return Service::Committed;
        }
        // Allocation/budget failure is a failed run too. Reporting Committed
        // would reset retries and make the panel request the same data forever.
    }

    // The backend drops a scan when the sample under it is reloaded, and does
    // not say so. Give up on this run rather than leaving the page unable to
    // ask for anything ever again.
    //
    // Clearing in_flight_ alone is not enough, and that was the original bug:
    // the CACHE is still armed from noteRequest(), and its guard is not
    // per-sample, so one dropped run stopped the whole process from ever
    // requesting another envelope. Release both.
    in_flight_ = false;
    cache_->abortPending();

    if (retries_ < config_.max_retries) {
        ++retries_;
        return Service::Retrying;
    }
    return Service::GaveUp;
}

void EnvelopeFetcher::abort() {
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        receive_state_ = ReceiveState::Idle;
        received_ = 0;
        channels_ = 0;
    }
    if (in_flight_) {
        in_flight_ = false;
        if (cache_) {
            cache_->abortPending();
        }
    }
    // A fresh start, not a continuation: the timeouts counted so far belonged
    // to the view being abandoned, and carrying them over would give the next
    // sample fewer chances than the first - none at all, once one had given
    // up.
    retries_ = 0;
}

}  // namespace wavex_ui
