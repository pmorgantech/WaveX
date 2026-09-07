#include "envelope_panel.h"

#include <algorithm>

namespace wavex_ui {

using WaveX::Protocol::EnvelopeChunkMessage;
using WaveX::Protocol::EnvelopeColumn;

void EnvelopePanel::attach(const Config& config,
                           const Link& link,
                           EnvelopeCache* cache,
                           EnvelopeSink* const* sinks,
                           uint8_t count) {
    detach();
    if (!cache || !link.send || !link.listen || !sinks) {
        return;
    }
    config_ = config;
    link_ = link;
    cache_ = cache;

    view_count_ = std::min<uint8_t>(count, kMaxViews);
    uint16_t widest = 0;
    for (uint8_t i = 0; i < view_count_; ++i) {
        views_[i] = View{};
        views_[i].sink = sinks[i];
        if (sinks[i]) {
            widest = std::max(widest, sinks[i]->columns());
        }
    }
    buffer_.assign(static_cast<size_t>(widest) * 2, EnvelopeColumn());

    EnvelopeFetcher::Config fc;
    fc.max_run_columns = WaveX::Protocol::MAX_ENVELOPE_COLUMNS;
    fc.timeout_ms = config.timeout_ms;
    fc.max_retries = config.max_retries;
    fetcher_.init(fc, link.send, cache);

    // The cache's one pending run may belong to a page that left without
    // releasing it. Nothing can be asked for until it is dropped, and no
    // reply to it can be filed now that the listener is about to move.
    cache_->abortPending();

    has_sample_ = false;
    changed_ = false;
    request_pending_ = false;
    immediate_ = false;
    dirty_ = false;
    gave_up_ = false;
    retries_ = 0;

    // Last: from here on chunks arrive on the RX task, and everything they
    // land in has to exist first.
    link_.listen(&EnvelopePanel::chunkTrampoline, this);
}

void EnvelopePanel::detach() {
    if (!attached()) {
        return;
    }
    // Listener FIRST, so no chunk can arrive on the RX task while the fetcher
    // is being torn down, THEN the fetcher, which releases the cache's run.
    link_.listen(nullptr, nullptr);
    fetcher_.abort();
    cache_ = nullptr;
    view_count_ = 0;
    has_sample_ = false;
}

void EnvelopePanel::chunkTrampoline(const EnvelopeChunkMessage& header,
                                    const EnvelopeColumn* columns,
                                    void* user) {
    auto* self = static_cast<EnvelopePanel*>(user);
    if (self) {
        self->fetcher_.onChunk(header, columns);  // RX task: stage only
    }
}

void EnvelopePanel::setSample(uint16_t sample_id, uint16_t generation, uint32_t total_frames) {
    if (!attached()) {
        return;
    }
    fetcher_.abort();
    sample_id_ = sample_id;
    generation_ = generation;
    total_frames_ = total_frames;
    has_sample_ = total_frames > 0;
    gave_up_ = false;
    retries_ = 0;
    dirty_ = false;
    changed_ = false;
    clearSinks();
    if (has_sample_) {
        request_pending_ = true;
        immediate_ = true;
    } else {
        request_pending_ = false;
    }
}

void EnvelopePanel::clearSample() {
    if (!attached()) {
        return;
    }
    fetcher_.abort();
    has_sample_ = false;
    total_frames_ = 0;
    gave_up_ = false;
    retries_ = 0;
    dirty_ = false;
    changed_ = false;
    request_pending_ = false;
    clearSinks();
}

void EnvelopePanel::setWindow(uint8_t index, uint32_t start, uint32_t end) {
    if (index >= view_count_) {
        return;
    }
    View& v = views_[index];
    if (v.start == start && v.end == end) {
        return;
    }
    v.start = start;
    v.end = end;
    dirty_ = dirty_ || has_sample_;  // Cached data draws now; only wire requests settle.
    invalidate(v);
    changed_ = true;
}

void EnvelopePanel::enableView(uint8_t index, bool enabled) {
    if (index >= view_count_) {
        return;
    }
    View& v = views_[index];
    if (v.enabled == enabled) {
        return;
    }
    v.enabled = enabled;
    if (enabled) {
        // Whatever the cache has, straight away; what it lacks, after the
        // settle. Disabling asks for nothing: a run in flight for the view
        // still lands in the cache, where it waits for the view to return.
        invalidate(v);
        changed_ = true;
        dirty_ = true;
    }
}

void EnvelopePanel::redraw() {
    for (uint8_t i = 0; i < view_count_; ++i) {
        invalidate(views_[i]);
    }
    dirty_ = true;
}

void EnvelopePanel::requestNow() {
    request_pending_ = true;
    immediate_ = true;
}

bool EnvelopePanel::drawn(uint8_t index) const {
    return index < view_count_ && views_[index].drawn;
}

void EnvelopePanel::scheduleRequest(uint32_t at_ms) {
    request_pending_ = true;
    if (!immediate_) {
        request_at_ = at_ms;
    }
}

bool EnvelopePanel::requestDue(uint32_t now_ms) const {
    if (!request_pending_) {
        return false;
    }
    // Signed difference, so a due time the clock has passed compares as due
    // across a wrap as well.
    return immediate_ || static_cast<int32_t>(now_ms - request_at_) >= 0;
}

EnvelopePanel::Event EnvelopePanel::service(uint32_t now_ms) {
    if (!attached()) {
        return Event::None;
    }
    Event event = Event::None;

    switch (fetcher_.service(now_ms)) {
        case EnvelopeFetcher::Service::Committed:
            // Draw what landed, then straight on to whatever is still
            // missing - the next view, or the next run of this one.
            dirty_ = true;
            retries_ = 0;
            request_pending_ = true;
            immediate_ = true;
            break;
        case EnvelopeFetcher::Service::Retrying:
            dirty_ = true;
            retries_ = fetcher_.retries();
            scheduleRequest(now_ms + config_.settle_ms);
            event = Event::Retrying;
            break;
        case EnvelopeFetcher::Service::GaveUp:
            dirty_ = true;
            gave_up_ = true;
            request_pending_ = false;
            event = Event::GaveUp;
            break;
        case EnvelopeFetcher::Service::Idle:
            break;
    }

    if (changed_) {
        changed_ = false;
        scheduleRequest(now_ms + config_.settle_ms);
    }

    if (has_sample_ && !gave_up_ && !fetcher_.busy() && requestDue(now_ms)) {
        request_pending_ = false;
        immediate_ = false;
        if (requestMissing(now_ms, event)) {
            dirty_ = true;
        }
    }

    if (dirty_) {
        dirty_ = false;
        render();
        if (event == Event::None) {
            event = Event::Drawn;
        }
    }
    return event;
}

bool EnvelopePanel::requestMissing(uint32_t now_ms, Event& event) {
    for (uint8_t i = 0; i < view_count_; ++i) {
        const View& v = views_[i];
        if (!v.enabled || !v.sink || v.end <= v.start) {
            continue;
        }
        switch (fetcher_.request(
            sample_id_, generation_, v.start, v.end, total_frames_, v.sink->columns(), now_ms)) {
            case EnvelopeFetcher::Request::AlreadyCached:
                continue;
            case EnvelopeFetcher::Request::Sent:
                return false;
            case EnvelopeFetcher::Request::SendFailed:
                // Nothing is armed. The link's TX queue being full is the
                // usual cause and it drains, so this is a retry, not an end -
                // but a bounded one, on the same budget as a timeout.
                if (retries_ < config_.max_retries) {
                    ++retries_;
                    scheduleRequest(now_ms + config_.settle_ms);
                    event = Event::SendFailed;
                } else {
                    gave_up_ = true;
                    event = Event::GaveUp;
                }
                return false;
            case EnvelopeFetcher::Request::Busy:
            case EnvelopeFetcher::Request::NotReady:
                return false;
        }
    }
    return true;
}

void EnvelopePanel::render() {
    for (uint8_t i = 0; i < view_count_; ++i) {
        View& v = views_[i];
        // A view already rendered complete for this window has nothing to
        // gain from another pass; on the edit page that is the 1240-column
        // continuous trace, which would otherwise be re-rendered and
        // re-drawn each time one of the splice halves lands.
        if (!v.enabled || !v.sink || v.drawn) {
            continue;
        }
        const uint16_t columns = v.sink->columns();
        if (!has_sample_ || v.end <= v.start || columns == 0 ||
            static_cast<size_t>(columns) * 2 > buffer_.size()) {
            v.sink->clear();
            invalidate(v);
            continue;
        }
        uint8_t channels = 0;
        uint32_t fpc = 0;
        const uint16_t covered = cache_->render(sample_id_,
                                                generation_,
                                                v.start,
                                                v.end,
                                                columns,
                                                buffer_.data(),
                                                buffer_.size(),
                                                channels,
                                                &fpc);
        if (covered == 0 || channels == 0) {
            // Nothing for this window yet. Leave what the sink shows alone:
            // it is either already clear (setSample) or the previous window's
            // trace, and a flash to empty on every scrub step is worse than a
            // trace one step stale for one settle.
            invalidate(v);
            continue;
        }
        if (fpc != v.shown_fpc || covered != v.shown_covered) {
            v.sink->setEnvelope(buffer_.data(), columns, channels);
            v.shown_fpc = fpc;
            v.shown_covered = covered;
        }
        // Done only when every column came from this view's own tier or a
        // finer one. The cache draws from the run that covers most of the
        // window whatever its tier, so a splice half is shown from the
        // whole-file scan the moment that lands - and must still be rendered
        // again when its own, finer run does.
        v.drawn = covered >= columns &&
                  fpc <= EnvelopeCache::tierFramesPerColumn(v.end - v.start, columns);
    }
}

void EnvelopePanel::clearSinks() {
    for (uint8_t i = 0; i < view_count_; ++i) {
        invalidate(views_[i]);
        if (views_[i].sink) {
            views_[i].sink->clear();
        }
    }
}

}  // namespace wavex_ui
