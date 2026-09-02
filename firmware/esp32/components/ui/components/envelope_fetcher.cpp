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

    epoch_.fetch_add(1, std::memory_order_acq_rel);
    ready_.store(false, std::memory_order_relaxed);
    received_.store(0, std::memory_order_relaxed);
    channels_.store(0, std::memory_order_relaxed);
    in_flight_ = false;
    retries_ = 0;
}

EnvelopeFetcher::Request EnvelopeFetcher::request(uint16_t sample_id,
                                                  uint16_t generation,
                                                  uint32_t view_start,
                                                  uint32_t view_end,
                                                  uint32_t total_frames,
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
                             config_.display_columns,
                             config_.max_run_columns,
                             req_start,
                             req_end,
                             req_columns)) {
        return Request::AlreadyCached;
    }

    // Arm the receiver before sending, and bump the epoch first so a chunk
    // from the previous run cannot be filed against this one. acq_rel so a
    // chunk copying concurrently sees the new epoch on its re-check and
    // discards itself rather than filing against this run.
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    ready_.store(false, std::memory_order_relaxed);
    received_.store(0, std::memory_order_relaxed);
    channels_.store(0, std::memory_order_relaxed);
    pending_sample_id_ = sample_id;
    pending_generation_ = generation;
    pending_start_ = req_start;
    pending_end_ = req_end;
    pending_columns_ = req_columns;

    // Send BEFORE arming the cache. noteRequest() blocks every later
    // nextRequest() until the run commits, so arming first and then failing to
    // send left the cache waiting on a reply that was never asked for - and
    // because the send failure also skipped in_flight_, the timeout never ran
    // either. Both halves have to be armed together or not at all. Safe to
    // order this way: ingest() is only reached from service() on this task, so
    // no chunk can be filed between the send and the noteRequest().
    if (!send_(sample_id, req_columns, req_start, req_end)) {
        return Request::SendFailed;
    }
    cache_->noteRequest(sample_id, generation, req_start, req_end, req_columns);
    in_flight_ = true;
    sent_ms_ = now_ms;
    return Request::Sent;
}

void EnvelopeFetcher::onChunk(const WaveX::Protocol::EnvelopeChunkMessage& header,
                              const WaveX::Protocol::EnvelopeColumn* columns) {
    if (!columns || ready_.load(std::memory_order_acquire) || staging_.empty()) {
        return;  // nothing armed, or the last run is still waiting to be filed
    }
    const uint32_t epoch = epoch_.load(std::memory_order_acquire);
    if (header.sample_id != pending_sample_id_ || header.generation != pending_generation_ ||
        header.start_frame != pending_start_ || header.total_columns != pending_columns_) {
        return;  // a reply to a view the user has already left
    }
    if (header.channels == 0 || header.channels > 2 || header.columns == 0) {
        return;
    }
    const uint8_t seen_channels = channels_.load(std::memory_order_relaxed);
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
    if (header.first_column > received_.load(std::memory_order_relaxed)) {
        return;
    }

    std::copy(columns,
              columns + static_cast<size_t>(header.columns) * header.channels,
              staging_.begin() + static_cast<size_t>(header.first_column) * header.channels);

    // Re-check: request() may have re-armed while this was copying, in which
    // case what was just written belongs to neither run.
    if (epoch_.load(std::memory_order_acquire) != epoch) {
        return;
    }
    channels_.store(header.channels, std::memory_order_relaxed);
    if (end_column > received_.load(std::memory_order_relaxed)) {
        received_.store(static_cast<uint16_t>(end_column), std::memory_order_relaxed);
    }
    if (received_.load(std::memory_order_relaxed) >= pending_columns_) {
        // Release: everything above, including the column data, must be
        // visible to the UI task before it can observe this flag.
        ready_.store(true, std::memory_order_release);
    }
}

EnvelopeFetcher::Service EnvelopeFetcher::service(uint32_t now_ms) {
    if (!initialized()) {
        return Service::Idle;
    }

    // Hand a completed run to the cache. This is the only place the cache is
    // touched from, which is what lets it stay lock-free.
    if (ready_.load(std::memory_order_acquire)) {
        WaveX::Protocol::EnvelopeChunkMessage header;
        header.sample_id = pending_sample_id_;
        header.generation = pending_generation_;
        header.start_frame = pending_start_;
        header.end_frame = pending_end_;
        header.total_columns = pending_columns_;
        header.first_column = 0;
        header.columns = pending_columns_;
        const uint8_t channels = channels_.load(std::memory_order_relaxed);
        header.channels = channels ? channels : 1;

        cache_->ingest(header, staging_.data());
        ready_.store(false, std::memory_order_relaxed);
        in_flight_ = false;
        retries_ = 0;
        return Service::Committed;
    }

    if (!in_flight_ || (now_ms - sent_ms_) < config_.timeout_ms) {
        return Service::Idle;
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
    // Bump the epoch so a chunk still in flight on the RX task cannot file
    // itself against the run being abandoned.
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    ready_.store(false, std::memory_order_relaxed);
    received_.store(0, std::memory_order_relaxed);
    channels_.store(0, std::memory_order_relaxed);
    if (in_flight_) {
        in_flight_ = false;
        if (cache_) {
            cache_->abortPending();
        }
    }
}

}  // namespace wavex_ui
