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
// - The file model is **ahead of the engine** by design (osc 2, envs 2-3,
//   LFOs, trim, transpose, poly mode - stage 5). Those fields have no engine
//   home yet.
//
// That last point has a consequence worth stating plainly rather than
// discovering later: **a load-then-save through this build normalises an
// Instrument to what this build understands.** A file written by a later
// build, hand-edited, or carrying stage-5 fields loses them on the way
// through. That is the same rule as the container's "readers skip unknown
// chunks", applied to fields rather than chunks, and it is safe today only
// because nothing in this build can *set* those fields - so no user edit can
// be lost. It stops being safe the moment stage 5 ships a UI for them, which
// is why Save must not outlive that gap: see the roadmap entry.

#include "instrument.hpp"
#include "sfz_import.hpp"
#include "wxi/wxi.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace AudioEngine {
namespace InstrumentMap {

// Which oscillator a Sample Instrument's zones live on. Osc 2 arrives with
// stage 5; until then everything the engine plays is osc 1, and naming the
// index here keeps that assumption in one place instead of a bare 0.
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
    WaveX::ReconstructInPlace(out);  // ~8 KB of paths - never a stack temporary
    out.instrument.origin = InstrumentOrigin::Built;
    out.instrument.mode =
        doc.mode == Wxi::Mode::Drum ? InstrumentMode::Drum : InstrumentMode::Keyboard;
    std::memcpy(out.instrument.name, doc.name, kInstrumentNameBytes);
    out.instrument.name[kInstrumentNameBytes - 1] = '\0';

    out.instrument.filter.type = static_cast<uint8_t>(doc.filter.type);
    out.instrument.filter.cutoff_hz = doc.filter.cutoff_hz;
    out.instrument.filter.resonance = doc.filter.resonance;
    // filter.keytrack / env2_amount and envs 2-3 have no engine field yet.

    out.instrument.env.attack_s = doc.env[0].attack_s;
    out.instrument.env.decay_s = doc.env[0].decay_s;
    out.instrument.env.sustain = doc.env[0].sustain;
    out.instrument.env.release_s = doc.env[0].release_s;

    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        out.instrument.mod_slots[i].source = doc.mod_slots[i].source;
        out.instrument.mod_slots[i].dest = doc.mod_slots[i].dest;
        out.instrument.mod_slots[i].depth = doc.mod_slots[i].depth;
        out.instrument.mod_slots[i].curve = doc.mod_slots[i].curve;
        out.instrument.mod_slots[i].flags = doc.mod_slots[i].flags;
    }

    const Wxi::Oscillator& osc = doc.osc[kSampleOsc];
    uint8_t highest = 0;
    for (uint8_t i = 0; i < osc.zone_count && i < Wxi::kMaxZonesPerOsc; ++i) {
        const Wxi::Zone& src = osc.zones[i];
        // A stored index this build cannot hold is dropped rather than
        // wrapped onto another zone, which would overwrite a good one.
        if (src.index >= kMaxZones)
            continue;
        Zone& dst = out.instrument.zones[src.index];
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

        std::strncpy(out.sample_paths[src.index], src.path, Sfz::kMaxPath - 1);
        out.sample_paths[src.index][Sfz::kMaxPath - 1] = '\0';
        if (src.index >= highest)
            highest = static_cast<uint8_t>(src.index + 1);
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
 * Fields the engine does not model keep the document's defaults rather than
 * being cleared, so a caller that loaded a document and edits it in place
 * keeps what it could not represent. Callers that build a document from
 * scratch get the documented defaults, which is the same thing.
 */
inline bool ToFile(const Instrument& instrument,
                   const SamplePathResolver& resolver,
                   Wxi::InstrumentFile& doc) {
    std::memcpy(doc.name, instrument.name, kInstrumentNameBytes);
    doc.name[Wxi::kNameBytes - 1] = '\0';
    doc.mode = instrument.mode == InstrumentMode::Drum ? Wxi::Mode::Drum : Wxi::Mode::Keyboard;

    doc.filter.type = static_cast<Wxi::FilterType>(instrument.filter.type);
    doc.filter.cutoff_hz = instrument.filter.cutoff_hz;
    doc.filter.resonance = instrument.filter.resonance;

    doc.env[0].attack_s = instrument.env.attack_s;
    doc.env[0].decay_s = instrument.env.decay_s;
    doc.env[0].sustain = instrument.env.sustain;
    doc.env[0].release_s = instrument.env.release_s;

    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        doc.mod_slots[i].source = instrument.mod_slots[i].source;
        doc.mod_slots[i].dest = instrument.mod_slots[i].dest;
        doc.mod_slots[i].depth = instrument.mod_slots[i].depth;
        doc.mod_slots[i].curve = instrument.mod_slots[i].curve;
        doc.mod_slots[i].flags = instrument.mod_slots[i].flags;
    }

    Wxi::Oscillator& osc = doc.osc[kSampleOsc];
    osc.zone_count = 0;
    for (uint8_t z = 0; z < kMaxZones; ++z) {
        const Zone& src = instrument.zones[z];
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
    // A Sample Instrument with no zones is still a Sample Instrument - an
    // empty pad map the user is about to fill, not an Off oscillator.
    osc.type = Wxi::OscType::Sample;
    return true;
}

}  // namespace InstrumentMap
}  // namespace AudioEngine
}  // namespace WaveX
