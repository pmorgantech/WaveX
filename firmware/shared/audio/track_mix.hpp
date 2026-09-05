#pragma once

// Per-track mixer state (roadmap Phase 2.5 item 2; design:
// docs/features/output-routing-and-mixer.md §1-2). A "track" is an instrument
// slot, so this is the control layer over whatever those slots are playing.
//
// Shared rather than Daisy-local because the two sides own different halves of
// it and must agree on the arithmetic:
//
//   - The Daisy holds the gain/pan/mute table and applies it in the voice
//     render sum.
//   - The ESP32 owns SOLO. The engine deliberately has no solo concept: a
//     solo set is expanded to a mute set on the frontend and sent as mutes, so
//     a dropped link cannot strand a hidden solo on the backend with no way to
//     clear it. ExpandSoloToMutes() is that expansion.
//   - Both sides convert between dB and linear, and between a peak level and
//     the meter byte the wire carries. Two implementations of those would
//     disagree in the last bit and show a fader that does not match what is
//     heard.
//
// HAL-free and allocation-free: fixed arrays, no I/O, safe to call from the
// audio callback at block rate.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace Mix {

/// One per Track (AudioEngine::kNumTracks). Kept as its own
/// constant so this header does not pull in the instrument model; a static
/// assert where they meet is cheaper than the coupling.
static constexpr uint8_t kNumTracks = 16;

/// Fader range. -60 dB is the floor the UI shows as -inf and the mapping
/// treats as silence; +6 dB of headroom matches the design's fader.
static constexpr float kMinGainDb = -60.0f;
static constexpr float kMaxGainDb = 6.0f;

/// Soft-mute ramp. A hard cut on a sounding voice is a step discontinuity,
/// which is a click; 5 ms is long enough to remove it and short enough that
/// punching a mute still feels immediate.
static constexpr float kMuteRampSeconds = 0.005f;

/// Linear gain for a dB value, with the floor mapped to true silence rather
/// than to a very small number - a fader pulled to the bottom must be silent,
/// not -60 dB of audible hiss on a quiet passage.
inline float DbToLinear(float db) {
    if (db <= kMinGainDb) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

/// Inverse of DbToLinear. Zero and negative amplitudes report the floor rather
/// than -inf, so a caller formatting this into a label cannot print "nan".
inline float LinearToDb(float linear) {
    if (linear <= 0.0f) {
        return kMinGainDb;
    }
    const float db = 20.0f * std::log10(linear);
    return db < kMinGainDb ? kMinGainDb : db;
}

/**
 * @brief Expands a solo set into the mute set the engine is actually sent.
 *
 * With nothing soloed the explicit mutes stand as they are. With anything
 * soloed, every track that is NOT soloed is muted regardless of its own mute
 * flag, and a soloed track plays even if it was explicitly muted - which is
 * what makes solo useful for checking a track you had muted.
 *
 * Pure so the frontend can compute it and the result can be host-tested; the
 * engine never sees a solo bit.
 *
 * @param solo_mask      bit per track, 1 = soloed
 * @param explicit_mutes bit per track, 1 = muted by the user
 * @return               bit per track, 1 = the engine should mute it
 */
inline uint16_t ExpandSoloToMutes(uint16_t solo_mask, uint16_t explicit_mutes) {
    if (solo_mask == 0) {
        return explicit_mutes;
    }
    return static_cast<uint16_t>(~solo_mask);
}

/**
 * @brief Peak amplitude to the byte MSG_MIX_METERS carries.
 *
 * Logarithmic, because a linear meter spends most of its travel on the top
 * 6 dB and shows nothing useful about a quiet track. Maps [kMinGainDb, 0 dB]
 * onto [1, 255] and reserves 0 for true silence, so "no signal" is
 * distinguishable from "very quiet" on the display.
 *
 * Monotonic by construction: a louder peak never produces a smaller byte.
 */
inline uint8_t PeakToMeterByte(float peak) {
    if (peak <= 0.0f) {
        return 0;
    }
    const float db = LinearToDb(peak);
    if (db <= kMinGainDb) {
        return 0;
    }
    const float clamped = db > 0.0f ? 0.0f : db;
    // (db - floor) / -floor maps the range onto 0..1.
    const float unit = (clamped - kMinGainDb) / (-kMinGainDb);
    const float scaled = 1.0f + unit * 254.0f;
    return static_cast<uint8_t>(scaled > 255.0f ? 255.0f : scaled);
}

/// Inverse of PeakToMeterByte, for a frontend drawing a bar in dB.
inline float MeterByteToDb(uint8_t value) {
    if (value == 0) {
        return kMinGainDb;
    }
    const float unit = (static_cast<float>(value) - 1.0f) / 254.0f;
    return kMinGainDb + unit * (-kMinGainDb);
}

// ---------------------------------------------------------------------------
// Wire encoding for MSG_MIX_OP. Lives here rather than in protocol.h so the
// protocol header stays free of <cmath>, and so these convert through exactly
// the same dB mapping as everything above - the failure this file exists to
// prevent is the two ends disagreeing about what a fader position means.
// ---------------------------------------------------------------------------

/// Centi-dB above the floor, so 0 is silence and the whole unsigned range is
/// meaningful. -60 dB -> 0, 0 dB -> 6000, +6 dB -> 6600.
inline uint16_t GainDbToWire(float db) {
    const float clamped = db < kMinGainDb ? kMinGainDb : (db > kMaxGainDb ? kMaxGainDb : db);
    return static_cast<uint16_t>((clamped - kMinGainDb) * 100.0f + 0.5f);
}

inline float WireToGainDb(uint16_t value) {
    const float db = kMinGainDb + static_cast<float>(value) / 100.0f;
    return db > kMaxGainDb ? kMaxGainDb : db;
}

/// Matches PARAM_PAN's existing convention: 0 hard left, 32768 centre, 65535
/// hard right. Reused rather than reinvented - a second pan encoding on the
/// same wire is how two ends end up disagreeing about where centre is.
inline uint16_t PanToWire(float offset) {
    const float clamped = offset < -1.0f ? -1.0f : (offset > 1.0f ? 1.0f : offset);
    const float scaled = (clamped + 1.0f) * 32767.5f;
    return static_cast<uint16_t>(scaled < 0.0f ? 0.0f : (scaled > 65535.0f ? 65535.0f : scaled));
}

inline float WireToPan(uint16_t value) {
    return (static_cast<float>(value) / 32767.5f) - 1.0f;
}

/// Per-track control state. Solo is absent on purpose - see the file header.
struct TrackMix {
    float gain = 1.0f;        ///< linear, post-voice and pre-master
    float pan_offset = 0.0f;  ///< -1..+1, added onto each voice's own pan
    bool mute = false;        ///< target state; the ramp below follows it
};

/**
 * @brief The 16-track table, plus the mute ramps that keep punches click-free.
 *
 * Setters run on the main loop; Tick() and the accessors run at block rate in
 * the audio callback. Nothing here blocks or allocates, and a setter racing a
 * Tick() can at worst change a ramp target one block early.
 */
class TrackMixer {
   public:
    /// Block-rate ramping needs to know how long a block is in seconds.
    void SetSampleRate(float hz) { sample_rate_hz_ = hz > 0.0f ? hz : 48000.0f; }

    void SetGain(uint8_t track, float linear) {
        if (track < kNumTracks) {
            tracks_[track].gain = linear < 0.0f ? 0.0f : linear;
        }
    }

    void SetPanOffset(uint8_t track, float offset) {
        if (track < kNumTracks) {
            const float clamped = offset < -1.0f ? -1.0f : (offset > 1.0f ? 1.0f : offset);
            tracks_[track].pan_offset = clamped;
        }
    }

    void SetMute(uint8_t track, bool mute) {
        if (track < kNumTracks) {
            tracks_[track].mute = mute;
        }
    }

    /// Applies a whole mute set at once, which is how the frontend sends solo.
    void SetMuteMask(uint16_t mask) {
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            tracks_[t].mute = ((mask >> t) & 1u) != 0u;
        }
    }

    void SetMasterGain(float linear) { master_gain_ = linear < 0.0f ? 0.0f : linear; }
    float MasterGain() const { return master_gain_; }

    const TrackMix& Track(uint8_t track) const { return tracks_[track < kNumTracks ? track : 0]; }

    /**
     * @brief Advances the mute ramps by one block.
     *
     * Call once per audio block, before reading GainFor(). Ramping here rather
     * than per sample keeps the cost at 16 adds per block no matter how many
     * voices are sounding.
     */
    void Tick(uint32_t block_frames) {
        if (block_frames == 0) {
            return;
        }
        const float ramp_frames = kMuteRampSeconds * sample_rate_hz_;
        // A ramp shorter than one block still has to make progress, or a small
        // block size would leave the envelope stuck part-way.
        const float step =
            ramp_frames > 1.0f ? (static_cast<float>(block_frames) / ramp_frames) : 1.0f;

        for (uint8_t t = 0; t < kNumTracks; ++t) {
            const float target = tracks_[t].mute ? 0.0f : 1.0f;
            float& level = mute_level_[t];
            if (level < target) {
                level += step;
                if (level > target) {
                    level = target;
                }
            } else if (level > target) {
                level -= step;
                if (level < target) {
                    level = target;
                }
            }
        }
    }

    /// Track gain including the mute ramp, excluding master. Multiply onto a
    /// voice's own gain at block boundaries.
    float GainFor(uint8_t track) const {
        if (track >= kNumTracks) {
            return 0.0f;
        }
        return tracks_[track].gain * mute_level_[track];
    }

    float PanOffsetFor(uint8_t track) const {
        return track < kNumTracks ? tracks_[track].pan_offset : 0.0f;
    }

    /// True once every ramp has reached its target - i.e. nothing is mid-punch.
    bool Settled() const {
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            const float target = tracks_[t].mute ? 0.0f : 1.0f;
            if (mute_level_[t] != target) {
                return false;
            }
        }
        return true;
    }

    /// Drops every track to defaults. Ramps are set to their targets rather
    /// than to 1.0, so a reset does not fade every track in from silence.
    void Reset() {
        for (uint8_t t = 0; t < kNumTracks; ++t) {
            tracks_[t] = TrackMix{};
            mute_level_[t] = 1.0f;
        }
        master_gain_ = 1.0f;
    }

   private:
    TrackMix tracks_[kNumTracks];
    /// 0 = fully muted, 1 = fully open. Starts open: a fresh mixer must not
    /// fade in.
    float mute_level_[kNumTracks] = {1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f,
                                     1.0f};
    float master_gain_ = 1.0f;
    float sample_rate_hz_ = 48000.0f;
};

}  // namespace Mix
}  // namespace WaveX
