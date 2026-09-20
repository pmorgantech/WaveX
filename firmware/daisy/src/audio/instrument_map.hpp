#pragma once

// Between the `.wxi` document (`shared/wxi/wxi.hpp`) and the engine's
// Instrument (`instrument.hpp`) - the two directions a Load and a Save need.
//
// They are deliberately different models and this is the seam:
//
// - A stored Zone names its Sample by **card path**; an engine Zone names it
//   by **Pool id**, which is a slot in this boot's registry and means nothing
//   across a power cycle. So loading produces paths for the loader to admit
//   to the Pool, and saving asks a resolver to name each id.
// - A stored Zone carries an explicit `index`; the engine's zone array is
//   sparse. Preserving the index is what keeps "pad 5" pad 5 across a save
//   that wrote only three zones.
// - Both maps and all defined Instrument parameter fields have an engine
//   home, even while their render/edit adapters are being completed. Editing
//   one supported field must not discard another field on save.

#include "instrument.hpp"
#include "sfz_import.hpp"
#include "wxi/wxi.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace AudioEngine {

// The file format and the wire each state the path bound for their own
// layering reasons (wxi.hpp explains why it does not include protocol.h).
// This is the one place that sees both, so it is where they are held
// together: a sample the wire can load must be one the file can name.
static_assert(Wxi::kPathBytes == Protocol::BROWSE_PATH_MAX,
              "the .wxi path bound and the wire path bound must agree");
static_assert(Sfz::kMaxPath == Protocol::BROWSE_PATH_MAX,
              "the SFZ importer's resolved-path bound must agree too");

namespace InstrumentMap {

// Primary oscillator used by the existing single-map editor/test fixtures.
// The mapper and loader handle both oscillator maps.
static constexpr uint8_t kSampleOsc = 0;

/**
 * Which parser a path gets.
 *
 * An `.sfz` is an IMPORT FORMAT for an Instrument, not a different kind of
 * thing (track-and-patch-model.md §3.3): both parsers produce the same
 * MappedInstrument and hand it to the same loader back half. Anything that is
 * not `.sfz` is treated as `.wxi`, so a file the browser offered is attempted
 * rather than refused over a spelling - the reader rejects it by content
 * (magic, file type, version) if it really is something else, which is a
 * better answer than "unknown extension".
 *
 * Case-insensitive: cards written on other machines carry `.SFZ`.
 */
inline bool PathIsSfz(const char* path) {
    if (!path)
        return false;
    const size_t n = std::strlen(path);
    if (n < 4)
        return false;
    const char* ext = path + n - 4;
    return ext[0] == '.' && (ext[1] == 's' || ext[1] == 'S') && (ext[2] == 'f' || ext[2] == 'F') &&
           (ext[3] == 'z' || ext[3] == 'Z');
}

/// Names a zone's Sample by card path for a Save. Returning false fails the
/// save: a zone whose Sample cannot be named would be silently unloadable,
/// which is worse than not saving at all.
struct SamplePathResolver {
    const void* ctx = nullptr;
    bool (*resolve)(const void* ctx, uint16_t sample_id, char* out, size_t out_len) = nullptr;
};

/**
 * `.wxi` document -> engine Instrument + the sample paths to admit.
 *
 * Zones land at their stored `index`, so a sparse map survives a round trip.
 * `sample_id` is left 0: the loader sets it when the path is admitted to the
 * Pool, and inventing one here would be a number that resolves to whatever
 * happens to occupy that slot.
 */
inline void FromFile(const Wxi::InstrumentFile& doc, Sfz::MappedInstrument& out) {
    WaveX::ReconstructInPlace(out);  // both maps/paths: never a stack temporary
    out.instrument.origin = InstrumentOrigin::Built;
    out.instrument.mode =
        doc.mode == Wxi::Mode::Drum ? InstrumentMode::Drum : InstrumentMode::Keyboard;
    std::memcpy(out.instrument.name, doc.name, kInstrumentNameBytes);
    out.instrument.name[kInstrumentNameBytes - 1] = '\0';

    out.instrument.filter.type = static_cast<uint8_t>(doc.filter.type);
    out.instrument.filter.topology = static_cast<uint8_t>(doc.filter.topology);
    out.instrument.filter.slope = static_cast<uint8_t>(doc.filter.slope);
    out.instrument.filter.drive = doc.filter.drive;
    out.instrument.filter.cutoff_hz = doc.filter.cutoff_hz;
    out.instrument.filter.resonance = doc.filter.resonance;
    out.instrument.filter.keytrack = doc.filter.keytrack;
    out.instrument.filter.env2_amount = doc.filter.env2_amount;
    out.instrument.tags = doc.tags;
    out.instrument.transpose = doc.transpose;
    out.instrument.fine_tune = doc.fine_tune;
    out.instrument.trim_gain = doc.trim_gain;
    out.instrument.trim_pan = doc.trim_pan;
    out.instrument.output = doc.output;
    out.instrument.poly_mode = doc.poly_mode;
    out.instrument.allocation = doc.allocation;
    out.instrument.osc_mix = doc.osc_mix;
    out.instrument.velocity_curve = doc.amp.velocity_curve;
    for (uint8_t e = 0; e < 3; ++e) {
        out.instrument.env[e].attack_s = doc.env[e].attack_s;
        out.instrument.env[e].decay_s = doc.env[e].decay_s;
        out.instrument.env[e].sustain = doc.env[e].sustain;
        out.instrument.env[e].release_s = doc.env[e].release_s;
    }
    for (uint8_t i = 0; i < 2; ++i) {
        out.instrument.lfo[i].wave = doc.lfo[i].wave;
        out.instrument.lfo[i].rate_hz = doc.lfo[i].rate_hz;
        out.instrument.lfo[i].sync_div = doc.lfo[i].sync_div;
        out.instrument.lfo[i].delay_s = doc.lfo[i].delay_s;
        out.instrument.lfo[i].fade_s = doc.lfo[i].fade_s;
        out.instrument.lfo[i].retrigger = doc.lfo[i].retrigger;
        out.instrument.lfo[i].pitch_follow = doc.lfo[i].pitch_follow;
    }

    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        out.instrument.mod_slots[i].source = doc.mod_slots[i].source;
        out.instrument.mod_slots[i].dest = doc.mod_slots[i].dest;
        out.instrument.mod_slots[i].depth = doc.mod_slots[i].depth;
        out.instrument.mod_slots[i].curve = doc.mod_slots[i].curve;
        out.instrument.mod_slots[i].flags = doc.mod_slots[i].flags;
    }

    uint8_t highest = 0;
    for (uint8_t o = 0; o < kNumOscillators; ++o) {
        const Wxi::Oscillator& osc = doc.osc[o];
        auto& target = out.instrument.osc[o];
        target.type = static_cast<OscType>(osc.type);
        target.level = osc.level;
        target.pan = osc.pan;
        target.coarse_tune = osc.coarse_tune;
        target.fine_tune = osc.fine_tune;
        target.keytrack = osc.keytrack;
        target.mono = osc.mono;

        if (target.type != OscType::Sample)
            continue;
        for (uint8_t i = 0; i < osc.zone_count && i < Wxi::kMaxZonesPerOsc; ++i) {
            const Wxi::Zone& src = osc.zones[i];
            // A stored index this build cannot hold is dropped rather than
            // wrapped onto another zone, which would overwrite a good one.
            if (src.index >= kMaxZones)
                continue;
            Zone& dst = target.zones[src.index];
            const uint8_t flat = o * kMaxZones + src.index;
            dst.sample_id = 0;
            dst.key_lo = src.key_lo;
            dst.key_hi = src.key_hi;
            dst.vel_lo = src.vel_lo;
            dst.vel_hi = src.vel_hi;
            dst.root_note = src.root_note;
            dst.coarse_tune = src.coarse_tune;
            dst.fine_tune = src.fine_tune;
            dst.gain = src.gain;
            dst.pan = src.pan;
            dst.start_frame = src.start_frame;
            dst.end_frame = src.end_frame;
            dst.loop_start = src.loop_start;
            dst.loop_end = src.loop_end;
            dst.loop_mode = src.loop_mode;
            dst.choke_group = src.choke_group;
            dst.output_bus = src.output_bus;
            dst.flags = src.flags;
            dst.cutoff_hz = src.cutoff_hz;
            dst.attack_s = src.attack_s;
            dst.decay_s = src.decay_s;
            dst.sustain = src.sustain;
            dst.release_s = src.release_s;
            dst.in_use = true;

            std::strncpy(out.sample_paths[flat], src.path, Sfz::kMaxPath - 1);
            out.sample_paths[flat][Sfz::kMaxPath - 1] = '\0';
            if (flat >= highest)
                highest = static_cast<uint8_t>(flat + 1);
        }
    }
    // zone_count is "how far into the sparse array the live zones reach", the
    // same meaning the SFZ importer gives it, not "how many are in use".
    out.zone_count = highest;
}

/**
 * Engine Instrument -> `.wxi` document, ready to write.
 *
 * Returns false if a zone's Sample cannot be named; the caller must not
 * write a partial document (write to a temporary and rename - see
 * offline-sample-editing.md §2).
 *
 */
inline bool ToFile(const Instrument& instrument,
                   const SamplePathResolver& resolver,
                   Wxi::InstrumentFile& doc) {
    std::memcpy(doc.name, instrument.name, kInstrumentNameBytes);
    doc.name[Wxi::kNameBytes - 1] = '\0';
    doc.mode = instrument.mode == InstrumentMode::Drum ? Wxi::Mode::Drum : Wxi::Mode::Keyboard;

    doc.filter.type = static_cast<Wxi::FilterType>(instrument.filter.type);
    static_assert(
        static_cast<uint8_t>(Wxi::FilterTopology::Ladder) == Protocol::INST_FILTER_TOPOLOGY_LADDER,
        "the file byte is the wire byte");
    doc.filter.topology = static_cast<Wxi::FilterTopology>(instrument.filter.topology);
    static_assert(static_cast<uint8_t>(Wxi::FilterSlope::Db24) == Protocol::INST_FILTER_SLOPE_24,
                  "the file slope byte is the wire byte");
    doc.filter.slope = static_cast<Wxi::FilterSlope>(instrument.filter.slope);
    doc.filter.drive = instrument.filter.drive;
    doc.filter.cutoff_hz = instrument.filter.cutoff_hz;
    doc.filter.resonance = instrument.filter.resonance;

    doc.filter.keytrack = instrument.filter.keytrack;
    doc.filter.env2_amount = instrument.filter.env2_amount;
    doc.tags = instrument.tags;
    doc.transpose = instrument.transpose;
    doc.fine_tune = instrument.fine_tune;
    doc.trim_gain = instrument.trim_gain;
    doc.trim_pan = instrument.trim_pan;
    doc.output = instrument.output;
    doc.poly_mode = instrument.poly_mode;
    doc.allocation = instrument.allocation;
    doc.osc_mix = instrument.osc_mix;
    doc.amp.velocity_curve = instrument.velocity_curve;
    for (uint8_t e = 0; e < 3; ++e) {
        doc.env[e].attack_s = instrument.env[e].attack_s;
        doc.env[e].decay_s = instrument.env[e].decay_s;
        doc.env[e].sustain = instrument.env[e].sustain;
        doc.env[e].release_s = instrument.env[e].release_s;
    }
    for (uint8_t i = 0; i < 2; ++i) {
        doc.lfo[i].wave = instrument.lfo[i].wave;
        doc.lfo[i].rate_hz = instrument.lfo[i].rate_hz;
        doc.lfo[i].sync_div = instrument.lfo[i].sync_div;
        doc.lfo[i].delay_s = instrument.lfo[i].delay_s;
        doc.lfo[i].fade_s = instrument.lfo[i].fade_s;
        doc.lfo[i].retrigger = instrument.lfo[i].retrigger;
        doc.lfo[i].pitch_follow = instrument.lfo[i].pitch_follow;
    }

    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        doc.mod_slots[i].source = instrument.mod_slots[i].source;
        doc.mod_slots[i].dest = instrument.mod_slots[i].dest;
        doc.mod_slots[i].depth = instrument.mod_slots[i].depth;
        doc.mod_slots[i].curve = instrument.mod_slots[i].curve;
        doc.mod_slots[i].flags = instrument.mod_slots[i].flags;
    }

    for (uint8_t o = 0; o < kNumOscillators; ++o) {
        Wxi::Oscillator& osc = doc.osc[o];
        const auto& source = instrument.osc[o];
        osc.type = static_cast<Wxi::OscType>(source.type);
        osc.level = source.level;
        osc.pan = source.pan;
        osc.coarse_tune = source.coarse_tune;
        osc.fine_tune = source.fine_tune;
        osc.keytrack = source.keytrack;
        osc.mono = source.mono;

        osc.zone_count = 0;
        for (uint8_t z = 0; z < kMaxZones; ++z) {
            const Zone& src = source.zones[z];
            if (!src.in_use)
                continue;
            if (osc.zone_count >= Wxi::kMaxZonesPerOsc)
                break;
            Wxi::Zone& dst = osc.zones[osc.zone_count];
            dst = Wxi::Zone{};
            dst.index = z;  // the whole point: pad 5 stays pad 5
            if (!resolver.resolve ||
                !resolver.resolve(resolver.ctx, src.sample_id, dst.path, sizeof(dst.path)))
                return false;
            dst.key_lo = src.key_lo;
            dst.key_hi = src.key_hi;
            dst.vel_lo = src.vel_lo;
            dst.vel_hi = src.vel_hi;
            dst.root_note = src.root_note;
            dst.coarse_tune = src.coarse_tune;
            dst.fine_tune = src.fine_tune;
            dst.gain = src.gain;
            dst.pan = src.pan;
            dst.start_frame = src.start_frame;
            dst.end_frame = src.end_frame;
            dst.loop_start = src.loop_start;
            dst.loop_end = src.loop_end;
            dst.loop_mode = src.loop_mode;
            dst.choke_group = src.choke_group;
            dst.output_bus = src.output_bus;
            dst.flags = src.flags;
            dst.cutoff_hz = src.cutoff_hz;
            dst.attack_s = src.attack_s;
            dst.decay_s = src.decay_s;
            dst.sustain = src.sustain;
            dst.release_s = src.release_s;
            ++osc.zone_count;
        }
    }
    return true;
}

}  // namespace InstrumentMap
}  // namespace AudioEngine
}  // namespace WaveX
