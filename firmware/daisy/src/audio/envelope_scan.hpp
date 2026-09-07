#pragma once

#include "spi_protocol/protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace WaveX::AudioEngine {

// Main-loop-owned scan cursor plus ONE retained packet. The reader resolves
// resident PCM each pass; no sample pointer survives a pass. No heap or I/O.
// A send failure preserves measured columns, including their wire identity.
class EnvelopeScan {
   public:
    static constexpr uint32_t kSampleReadsPerPass = 24576;
    // 286 bytes framed (20-byte header + 256 data + 10 framing): 1.43 ms
    // at the live 2 Mbaud link, versus 9.75 ms for the former 1920-byte data.
    static constexpr uint32_t kDataBytes =
        Protocol::MAX_ENVELOPE_CHUNK_VALUES * sizeof(Protocol::EnvelopeColumn8);
    struct Packet {
        Protocol::EnvelopeChunkMessage header;
        std::array<Protocol::EnvelopeColumn8, Protocol::MAX_ENVELOPE_CHUNK_VALUES> data;
    };
    static_assert(sizeof(Packet) == sizeof(Protocol::EnvelopeChunkMessage) + kDataBytes);

    bool Begin(const Protocol::EnvelopeChunkMessage& identity, uint32_t window_end = 0) {
        Cancel();
        if (identity.channels < 1 || identity.channels > 2 || identity.total_columns == 0 ||
            identity.total_columns > Protocol::MAX_ENVELOPE_COLUMNS ||
            identity.end_frame <= identity.start_frame ||
            identity.total_columns > identity.end_frame - identity.start_frame) {
            return false;
        }
        packet_.header = identity;
        packet_.header.first_column = 0;
        packet_.header.columns = 0;
        packet_.header.encoding = Protocol::ENVELOPE_ENCODING_S8;
        // Keep the requested grid when only its last bin extends past EOF.
        // Otherwise use the clamped window (arbitrary external requests).
        span_ = identity.end_frame - identity.start_frame;
        if (window_end > identity.end_frame) {
            const uint64_t requested_span = window_end - identity.start_frame;
            if (requested_span * (identity.total_columns - 1) / identity.total_columns < span_) {
                span_ = requested_span;
            }
        }
        frame_ = identity.start_frame;
        next_column_ = 0;
        ResetExtrema();
        active_ = true;
        return true;
    }

    void Cancel() { active_ = false; }
    bool Active() const { return active_; }
    const Protocol::EnvelopeChunkMessage& Identity() const { return packet_.header; }

    // read(frame, channel) returns a signed 16-bit display value.
    // reads_per_frame includes both PCM reads when the display sums stereo.
    // send(packet, bytes) takes a copy or returns false without taking ownership.
    // At most one packet per call; failed sends perform NO further PCM reads.
    template <typename Read, typename Send>
    uint32_t Pump(uint8_t reads_per_frame, Read read, Send send) {
        if (!active_ || reads_per_frame < packet_.header.channels || reads_per_frame > 2) {
            return 0;
        }
        auto& h = packet_.header;
        const uint32_t capacity = static_cast<uint32_t>(packet_.data.size()) / h.channels;
        const uint32_t budget = kSampleReadsPerPass / reads_per_frame;
        uint32_t frames = 0;
        while (next_column_ < h.total_columns && h.columns < capacity && frames < budget) {
            const uint32_t column_end = static_cast<uint32_t>(std::min<uint64_t>(
                h.end_frame, h.start_frame + span_ * (next_column_ + 1) / h.total_columns));
            const uint32_t stop = frame_ + std::min(column_end - frame_, budget - frames);
            for (; frame_ < stop; ++frame_, ++frames) {
                // Visit interleaved channels together, sequentially in SDRAM.
                for (uint8_t ch = 0; ch < h.channels; ++ch) {
                    const int16_t value = read(frame_, ch);
                    lo_[ch] = std::min(lo_[ch], value);
                    hi_[ch] = std::max(hi_[ch], value);
                }
            }
            if (frame_ == column_end) {
                for (uint8_t ch = 0; ch < h.channels; ++ch) {
                    packet_.data[h.columns * h.channels + ch] =
                        Protocol::EnvelopeColumn8::FromExtrema(lo_[ch], hi_[ch]);
                }
                ++h.columns;
                ++next_column_;
                ResetExtrema();
            }
        }
        if (h.columns && (h.columns == capacity || next_column_ == h.total_columns)) {
            const uint16_t bytes = static_cast<uint16_t>(
                sizeof(h) + h.columns * h.channels * sizeof(Protocol::EnvelopeColumn8));
            if (send(packet_, bytes)) {
                h.first_column = next_column_;
                h.columns = 0;
                active_ = next_column_ < h.total_columns;
            }
        }
        return frames * reads_per_frame;
    }

   private:
    void ResetExtrema() {
        lo_.fill(INT16_MAX);
        hi_.fill(INT16_MIN);
    }
    Packet packet_{};
    std::array<int16_t, 2> lo_{};
    std::array<int16_t, 2> hi_{};
    uint64_t span_ = 0;
    uint32_t frame_ = 0;
    uint16_t next_column_ = 0;
    bool active_ = false;
};

}  // namespace WaveX::AudioEngine
