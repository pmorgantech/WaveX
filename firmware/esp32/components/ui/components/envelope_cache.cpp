#include "envelope_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wavex_ui {

using WaveX::Protocol::EnvelopeChunkMessage;
using WaveX::Protocol::EnvelopeColumn;

namespace {

// Largest power of two <= v, for v >= 1.
uint32_t FloorPow2(uint32_t v) {
    uint32_t p = 1;
    while ((p << 1) != 0 && (p << 1) <= v) {
        p <<= 1;
    }
    return p;
}

}  // namespace

EnvelopeCache::~EnvelopeCache() {
    reset();
}

void EnvelopeCache::init(size_t budget_bytes, const Allocator& allocator) {
    reset();
    if (!allocator.alloc || !allocator.release) {
        return;  // unusable; initialized() stays false and every call no-ops
    }
    alloc_ = allocator;
    budget_bytes_ = budget_bytes;
}

void EnvelopeCache::reset() {
    for (Entry& e: entries_) {
        releaseEntry(e);
    }
    if (pending_.staging && alloc_.release) {
        alloc_.release(pending_.staging);
    }
    pending_ = Pending{};
    bytes_used_ = 0;
}

uint32_t EnvelopeCache::tierFramesPerColumn(uint32_t span_frames, uint16_t display_columns) {
    if (display_columns == 0 || span_frames == 0) {
        return 1;
    }
    const uint32_t per_column = span_frames / display_columns;
    if (per_column < 2) {
        return 1;  // at or past 1:1 - a tier cannot be finer than a frame
    }
    return FloorPow2(per_column);
}

void EnvelopeCache::releaseEntry(Entry& e) {
    if (e.data && alloc_.release) {
        alloc_.release(e.data);
    }
    if (e.used) {
        const size_t bytes = static_cast<size_t>(e.columns) * e.channels * sizeof(EnvelopeColumn);
        bytes_used_ = (bytes_used_ > bytes) ? (bytes_used_ - bytes) : 0;
    }
    e = Entry{};
}

size_t EnvelopeCache::entryCount() const {
    size_t n = 0;
    for (const Entry& e: entries_) {
        if (e.used) {
            ++n;
        }
    }
    return n;
}

void EnvelopeCache::invalidateSample(uint16_t sample_id) {
    for (Entry& e: entries_) {
        if (e.used && e.sample_id == sample_id) {
            releaseEntry(e);
        }
    }
    if (pending_.active && pending_.sample_id == sample_id) {
        pending_.active = false;
        pending_.received = 0;
    }
}

// Evicts least-recently-used entries until `bytes_needed` fits the budget and
// a slot is free. LRU rather than "drop the coarsest": the coarse tiers are
// the cheap ones and the ones a zoom-out lands on, so evicting by size would
// systematically throw away what is about to be asked for again.
bool EnvelopeCache::makeRoom(size_t bytes_needed) {
    if (bytes_needed > budget_bytes_) {
        return false;
    }

    for (;;) {
        bool slot_free = false;
        for (const Entry& e: entries_) {
            if (!e.used) {
                slot_free = true;
                break;
            }
        }
        if (slot_free && bytes_used_ + bytes_needed <= budget_bytes_) {
            return true;
        }

        Entry* victim = nullptr;
        for (Entry& e: entries_) {
            if (!e.used) {
                continue;
            }
            if (!victim || e.last_used < victim->last_used) {
                victim = &e;
            }
        }
        if (!victim) {
            return false;  // nothing held and it still does not fit
        }
        releaseEntry(*victim);
    }
}

size_t EnvelopeCache::gatherTouching(uint16_t sample_id,
                                     uint16_t generation,
                                     uint32_t fpc,
                                     uint8_t channels,
                                     uint32_t first,
                                     uint32_t count,
                                     Entry** out,
                                     uint32_t& merged_first,
                                     uint32_t& merged_end) {
    merged_first = first;
    merged_end = first + count;
    size_t n = 0;
    for (Entry& e: entries_) {
        if (!e.used || e.sample_id != sample_id || e.generation != generation || e.fpc != fpc ||
            e.channels != channels) {
            continue;
        }
        const uint32_t e0 = e.first_column;
        const uint32_t e1 = e0 + e.columns;
        if (first <= e1 && e0 <= first + count) {
            out[n++] = &e;
            merged_first = std::min(merged_first, e0);
            merged_end = std::max(merged_end, e1);
        }
    }
    // One pass is enough: two held runs that touch each other would already
    // have been merged when the second of them landed.
    return n;
}

const EnvelopeCache::Entry* EnvelopeCache::findCovering(uint16_t sample_id,
                                                        uint16_t generation,
                                                        uint32_t view_start,
                                                        uint32_t view_end) const {
    const Entry* best = nullptr;
    uint32_t best_covered = 0;

    for (const Entry& e: entries_) {
        if (!e.used || e.sample_id != sample_id || e.generation != generation) {
            continue;
        }
        const uint32_t e_start = e.first_column * e.fpc;
        const uint32_t e_end = (e.first_column + e.columns) * e.fpc;
        const uint32_t lo = std::max(e_start, view_start);
        const uint32_t hi = std::min(e_end, view_end);
        if (hi <= lo) {
            continue;
        }
        const uint32_t covered = hi - lo;
        // Most coverage wins; on a tie the finer tier does, since it is the
        // one that can still resolve a transient at this zoom.
        if (covered > best_covered || (best && covered == best_covered && e.fpc < best->fpc)) {
            best = &e;
            best_covered = covered;
        }
    }
    return best;
}

bool EnvelopeCache::nextRequest(uint16_t sample_id,
                                uint16_t generation,
                                uint32_t view_start,
                                uint32_t view_end,
                                uint32_t total_frames,
                                uint16_t display_columns,
                                uint16_t max_columns_per_request,
                                uint32_t& req_start,
                                uint32_t& req_end,
                                uint16_t& req_columns) const {
    if (!initialized() || view_end <= view_start || display_columns == 0 ||
        max_columns_per_request == 0 || total_frames == 0) {
        return false;
    }
    if (pending_.active) {
        return false;  // one run in flight at a time
    }

    const uint32_t fpc = tierFramesPerColumn(view_end - view_start, display_columns);
    // A finer complete run already contains every extreme this view needs.
    // Do not scan and transmit the same PCM just to populate a coarser tier.
    for (const Entry& e: entries_) {
        if (e.used && e.sample_id == sample_id && e.generation == generation && e.fpc <= fpc &&
            static_cast<uint64_t>(e.first_column) * e.fpc <= view_start &&
            (static_cast<uint64_t>(e.first_column) + e.columns) * e.fpc >=
                std::min(view_end, total_frames)) {
            return false;
        }
    }
    const uint32_t last_column = (total_frames + fpc - 1) / fpc;  // exclusive
    uint32_t c0 = view_start / fpc;
    uint32_t c1 = std::min((view_end + fpc - 1) / fpc, last_column);
    if (c1 <= c0) {
        return false;
    }

    // Skip what this tier already holds. Each entry is one contiguous run and
    // runs that touch are merged on commit, so what is left is one range: the
    // whole view, a prefix, a suffix, or - with two separate runs either side
    // of it - the hole between them. A run that lands in that hole touches
    // both and the three become one.
    for (const Entry& e: entries_) {
        if (!e.used || e.sample_id != sample_id || e.generation != generation || e.fpc != fpc) {
            continue;
        }
        const uint32_t e0 = e.first_column;
        const uint32_t e1 = e.first_column + e.columns;
        if (e0 <= c0 && e1 >= c1) {
            return false;  // fully covered
        }
        if (e0 <= c0 && e1 > c0) {
            c0 = e1;  // covered prefix
        } else if (e1 >= c1 && e0 < c1) {
            c1 = e0;  // covered suffix
        }
    }
    if (c1 <= c0) {
        return false;
    }

    if (c1 - c0 > max_columns_per_request) {
        c1 = c0 + max_columns_per_request;
    }

    // The end is NOT clamped to the file. The backend clamps at end-of-file
    // itself, and noteRequest() reads the tier back out of (end - start) /
    // columns, so a clamped end would name the tier below the one asked for
    // whenever the file is not a whole number of tier columns long:
    // 12,312,576 frames over 752 columns at 16384 is 16373 per column, which
    // floors to 8192, and the run then claims half the file it covers.
    const uint64_t end64 = static_cast<uint64_t>(c1) * fpc;
    req_start = c0 * fpc;
    req_end = static_cast<uint32_t>(std::min<uint64_t>(end64, UINT32_MAX));
    req_columns = static_cast<uint16_t>(c1 - c0);
    return req_columns > 0 && req_end > req_start;
}

void EnvelopeCache::noteRequest(uint16_t sample_id,
                                uint16_t generation,
                                uint32_t req_start,
                                uint32_t req_end,
                                uint16_t req_columns) {
    if (!initialized() || req_columns == 0 || req_end <= req_start) {
        return;
    }
    pending_.active = true;
    pending_.sample_id = sample_id;
    pending_.generation = generation;
    pending_.start_frame = req_start;
    pending_.end_frame = req_end;
    pending_.columns = req_columns;
    pending_.received = 0;
    pending_.channels = 0;
    // The tier is the requester's, not the reply's: nextRequest() hands out
    // a span that is exactly columns * tier (it does not clamp at end-of-file;
    // the backend does), so this division is exact. The reply's own end_frame
    // is never consulted - the backend's clamp would make it come back short.
    pending_.fpc = std::max<uint32_t>(1u, (req_end - req_start) / req_columns);
    pending_.fpc = FloorPow2(pending_.fpc);
    pending_.first_column = req_start / pending_.fpc;
}

// Keeps the staging buffer: the next run is almost always the same size, and
// this is called from the UI task where a free/alloc pair costs more than the
// few kilobytes it would return.
void EnvelopeCache::abortPending() {
    pending_.active = false;
    pending_.received = 0;
    pending_.channels = 0;
}

bool EnvelopeCache::ingest(const EnvelopeChunkMessage& header, const EnvelopeColumn* columns) {
    if (!initialized() || !columns || !pending_.active) {
        return false;
    }
    if (header.sample_id != pending_.sample_id || header.generation != pending_.generation ||
        header.start_frame != pending_.start_frame || header.total_columns != pending_.columns) {
        return false;  // a reply to a request we have already moved past
    }
    if (header.channels == 0 || header.channels > 2 || header.columns == 0) {
        return false;
    }
    if (pending_.channels != 0 && header.channels != pending_.channels) {
        return false;  // channel_mode changed mid-run; the caller will re-ask
    }

    const size_t needed =
        static_cast<size_t>(pending_.columns) * header.channels * sizeof(EnvelopeColumn);
    if (pending_.staging_bytes < needed) {
        if (pending_.staging) {
            alloc_.release(pending_.staging);
        }
        pending_.staging = static_cast<EnvelopeColumn*>(alloc_.alloc(needed));
        pending_.staging_bytes = pending_.staging ? needed : 0;
        if (!pending_.staging) {
            pending_.active = false;
            return false;
        }
        std::fill(
            pending_.staging, pending_.staging + needed / sizeof(EnvelopeColumn), EnvelopeColumn());
    }
    pending_.channels = header.channels;

    const uint32_t end_column = static_cast<uint32_t>(header.first_column) + header.columns;
    if (end_column > pending_.columns) {
        return false;  // malformed run; the router already length-checked the payload
    }
    // A resend after a full TX queue repeats columns rather than reordering
    // them, so overlap is expected and a genuine gap is not: accept anything
    // that touches or extends the contiguous head, drop anything past it.
    if (header.first_column > pending_.received) {
        return false;
    }
    std::memcpy(pending_.staging + static_cast<size_t>(header.first_column) * header.channels,
                columns,
                static_cast<size_t>(header.columns) * header.channels * sizeof(EnvelopeColumn));
    if (end_column > pending_.received) {
        pending_.received = static_cast<uint16_t>(end_column);
    }

    if (pending_.received < pending_.columns) {
        return false;
    }
    return commitPending();
}

bool EnvelopeCache::commitPending() {
    const uint8_t channels = pending_.channels;
    const uint32_t fpc = pending_.fpc;
    const uint32_t first = pending_.first_column;
    const uint32_t count = pending_.columns;

    pending_.active = false;

    // A tier may hold several runs of one sample at once, and a run that
    // touches none of them takes its own slot. This is what lets the edit
    // page's two splice halves - the same tier, far apart in the file - both
    // stay resident: when the second one landing evicted the first, the
    // first was re-requested, evicted the second, and so on, one run per
    // round trip, for as long as the loop markers were in view.
    Entry* touching[kMaxEntries];
    uint32_t merged_first = 0;
    uint32_t merged_end = 0;
    size_t n_touching = gatherTouching(pending_.sample_id,
                                       pending_.generation,
                                       fpc,
                                       channels,
                                       first,
                                       count,
                                       touching,
                                       merged_first,
                                       merged_end);
    if (merged_end - merged_first > kMaxEntryColumns) {
        // A merge that would exceed the per-entry cap: keep the new run and
        // drop what it touched, since the new one is what the user is looking
        // at.
        for (size_t i = 0; i < n_touching; ++i) {
            releaseEntry(*touching[i]);
        }
        n_touching = 0;
        merged_first = first;
        merged_end = first + count;
    }
    uint32_t merged_count = std::min<uint32_t>(merged_end - merged_first, kMaxEntryColumns);

    const size_t bytes = static_cast<size_t>(merged_count) * channels * sizeof(EnvelopeColumn);
    if (!makeRoom(bytes)) {
        return false;
    }
    // makeRoom may have evicted some of what was being merged into.
    n_touching = gatherTouching(pending_.sample_id,
                                pending_.generation,
                                fpc,
                                channels,
                                first,
                                count,
                                touching,
                                merged_first,
                                merged_end);
    merged_count = std::min<uint32_t>(merged_end - merged_first, kMaxEntryColumns);

    auto* data = static_cast<EnvelopeColumn*>(
        alloc_.alloc(static_cast<size_t>(merged_count) * channels * sizeof(EnvelopeColumn)));
    if (!data) {
        return false;
    }
    std::fill(data, data + static_cast<size_t>(merged_count) * channels, EnvelopeColumn());

    for (size_t i = 0; i < n_touching; ++i) {
        const Entry& e = *touching[i];
        const size_t off = static_cast<size_t>(e.first_column - merged_first) * channels;
        std::memcpy(
            data + off, e.data, static_cast<size_t>(e.columns) * channels * sizeof(EnvelopeColumn));
    }
    // New data last: where the runs overlap, the fresher measurement wins.
    std::memcpy(data + static_cast<size_t>(first - merged_first) * channels,
                pending_.staging,
                static_cast<size_t>(count) * channels * sizeof(EnvelopeColumn));

    // The merged run takes the first slot it absorbed; a run that touched
    // nothing takes a free one, which makeRoom() guaranteed.
    Entry* slot = n_touching ? touching[0] : nullptr;
    for (size_t i = 0; i < n_touching; ++i) {
        releaseEntry(*touching[i]);
    }
    if (!slot) {
        for (Entry& e: entries_) {
            if (!e.used) {
                slot = &e;
                break;
            }
        }
    }
    if (!slot) {
        alloc_.release(data);
        return false;
    }

    slot->used = true;
    slot->sample_id = pending_.sample_id;
    slot->generation = pending_.generation;
    slot->fpc = fpc;
    slot->first_column = merged_first;
    slot->columns = merged_count;
    slot->channels = channels;
    slot->last_used = ++tick_;
    slot->data = data;
    bytes_used_ += static_cast<size_t>(merged_count) * channels * sizeof(EnvelopeColumn);
    return true;
}

uint16_t EnvelopeCache::render(uint16_t sample_id,
                               uint16_t generation,
                               uint32_t view_start,
                               uint32_t view_end,
                               uint16_t display_columns,
                               EnvelopeColumn* out,
                               size_t out_capacity,
                               uint8_t& out_channels,
                               uint32_t* out_fpc) const {
    out_channels = 1;
    if (out_fpc) {
        *out_fpc = 0;
    }
    if (!out || display_columns == 0 || view_end <= view_start) {
        return 0;
    }

    const Entry* e = findCovering(sample_id, generation, view_start, view_end);
    if (!e) {
        return 0;
    }
    out_channels = e->channels;
    if (out_capacity < static_cast<size_t>(display_columns) * e->channels) {
        return 0;
    }
    e->last_used = ++tick_;
    if (out_fpc) {
        *out_fpc = e->fpc;
    }

    const uint64_t span = view_end - view_start;
    uint16_t covered = 0;

    for (uint16_t d = 0; d < display_columns; ++d) {
        const uint32_t f0 = view_start + static_cast<uint32_t>((span * d) / display_columns);
        uint32_t f1 = view_start + static_cast<uint32_t>((span * (d + 1)) / display_columns);
        if (f1 <= f0) {
            f1 = f0 + 1;
        }

        // Tier columns overlapping this display column, clipped to the entry.
        uint32_t t0 = f0 / e->fpc;
        uint32_t t1 = (f1 + e->fpc - 1) / e->fpc;
        if (t1 <= t0) {
            t1 = t0 + 1;
        }
        t0 = std::max(t0, e->first_column);
        t1 = std::min(t1, e->first_column + e->columns);

        if (t1 <= t0) {
            for (uint8_t ch = 0; ch < e->channels; ++ch) {
                out[static_cast<size_t>(d) * e->channels + ch] = EnvelopeColumn(0, 0);
            }
            continue;
        }

        for (uint8_t ch = 0; ch < e->channels; ++ch) {
            int32_t lo = 32767;
            int32_t hi = -32768;
            for (uint32_t t = t0; t < t1; ++t) {
                const EnvelopeColumn& c =
                    e->data[static_cast<size_t>(t - e->first_column) * e->channels + ch];
                lo = std::min<int32_t>(lo, c.min_sample);
                hi = std::max<int32_t>(hi, c.max_sample);
            }
            out[static_cast<size_t>(d) * e->channels + ch] =
                EnvelopeColumn(static_cast<int16_t>(lo), static_cast<int16_t>(hi));
        }
        ++covered;
    }

    return covered;
}

EnvelopeCache& GetEnvelopeCache() {
    static EnvelopeCache cache;
    return cache;
}

}  // namespace wavex_ui
