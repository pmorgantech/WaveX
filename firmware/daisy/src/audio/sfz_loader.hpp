#pragma once

// Target-side SFZ resident loader. Runtime work is cooperative: Begin() only
// accepts a request and Pump() performs at most one parse/probe/allocation/read
// step. FatFs and allocation never run in the audio callback.

#include "spi_protocol/protocol.h"

#include "instrument.hpp"
#include "mod_matrix.hpp"
#include "sample_pool.hpp"
#include "voice_manager.hpp"
#include <cstdint>

class SampleMemMgr;

namespace WaveX {
namespace AudioEngine {
namespace SfzLoader {

// Every sample an import loads is a Sample Pool entry
// (track-and-patch-model.md §4, audio/sample_pool.hpp): shared by path with
// other imports and with samples the user loaded, listable, editable, and
// released per Track - a Track that holds an import can take a bare sample
// (a deref), and two imports on two Tracks share what they have in common.
// The engine owns the Pool and the allocator and hands both in; this file
// owns the Tracks and the load state machine. Main-loop only.

void Reset();
bool Begin(const WaveX::Protocol::InstOpMessage& request);
void Pump(SamplePool& pool, SampleMemMgr& memory, uint8_t* io_buffer, uint32_t io_buffer_bytes);
bool Busy();
bool TrackLoading(uint8_t track);
// The Track whose voices must be hard-stopped before the load can release
// what that Track held, or 0xFF when no stop is pending. The engine asks the
// callback to StopTrack() it and calls ConfirmVoicesStopped() on the ack.
uint8_t VoiceStopTrack();
void ConfirmVoicesStopped(SamplePool& pool, SampleMemMgr& memory);

// Boot compatibility: runs the same cooperative state machine to completion
// before audio starts. request_id 0 suppresses frontend status messages.
bool Load(const char* path,
          uint8_t track,
          SamplePool& pool,
          SampleMemMgr& memory,
          uint8_t* io_buffer,
          uint32_t io_buffer_bytes);

// True when Track `track` holds ANY Instrument - imported from an .sfz or built
// on-device by BindSample() - i.e. when a note on it can resolve at all.
bool TrackLoaded(uint8_t track);

// The Tracks are the one note-resolution path (roadmap Phase 2.5 item 1): a
// bare sample bound to a Track is a one-zone Instrument in it, not a second
// registry beside it. Its zone spans the whole keyboard, pitch-tracks from
// `root_note`, and takes filter/ADSR from the live params at trigger
// (ZONE_FLAG_LIVE_FILTER_ENV) - exactly what the retired bare-WAV note path
// did, now as editable zone state rather than constants in the note path.
//
// sample_id is a Pool id. sample_id 0 unbinds. Whatever the Track held -
// a bare sample or an import - is released: its Pool refs are dropped and
// any sample nobody else holds is freed, which is why the CALLER must have
// stopped the Track's voices first (the engine's per-track barrier). Keeps
// the Track's mod slots either way; a bind is not an edit of them.
bool BindSample(SamplePool& pool,
                SampleMemMgr& memory,
                uint8_t track,
                uint16_t sample_id,
                uint8_t root_note = 60);
// Bytes the Pool would free if `track` released what it holds: samples only
// this Track references and the user did not pin.
uint32_t ReclaimableBytes(SamplePool& pool, uint8_t track);
// The registry id a Built Track's first zone plays, else 0.
uint16_t BoundSample(uint8_t track);

// Display name for what Track `track` holds: an import's .sfz basename, or - while
// that import is still loading - the basename of the request in flight, so
// the UI can name an Instrument before Commit stamps it. Never null; empty when
// the Track holds nothing, or holds a Built instrument whose name is the
// bound sample's own metadata and already known to the frontend.
const char* TrackName(uint8_t track);
// A Pool sample is going away (the user unloaded it): drop every zone on
// every Track that names it (and the Track's binding if that empties it), so
// no note resolves to a freed block. The engine calls this before it
// releases the memory.
void ForgetLoadedSample(uint16_t sample_id);
// The one resolver, over the Pool, registered by the engine because the
// allocator that turns a handle into a pointer lives there.
void SetLoadedSampleResolver(const SampleResolver& resolver);

// `live` feeds ZONE_FLAG_LIVE_FILTER_ENV zones; nullptr is allowed.
uint8_t ResolveNote(uint8_t track,
                    uint8_t note,
                    uint8_t velocity,
                    const VoiceLiveParams* live,
                    VoiceTriggerParams* out,
                    uint8_t max);

// Modulation matrix (param-locks-and-modulation.md §9 stage 4). Main-loop
// context only (message dispatch) - mirrors every other Tracks
// mutation in this file. Out-of-range track/mod_slot_index is a no-op
// returning false.
//
// Read directly from the audio callback's control tick with no mailbox: a
// ModSlot is 6 bytes, smaller than this architecture's atomic word, so a
// write landing mid-read could in principle be torn - but every field a
// torn read could produce is still bounds-checked downstream
// (ModSources::Get()'s default case, EvaluateModMatrix()'s default case), so
// the worst case is one control tick (1ms) of a harmless or odd-but-bounded
// modulation amount, self-correcting the next tick. That is a different risk
// class from VoiceLiveParams/ParaphonicParams, where a torn multi-field read
// could produce a sustained audibly-wrong combination or reach hardware CV -
// promote this to a mailbox like s_voice_live_pending if a bench session
// ever finds it audible.
bool SetModSlot(uint8_t track, uint8_t mod_slot_index, const ModSlot& value);
const ModSlot* GetModSlots(uint8_t track);

// Track settings (track-and-patch-model.md §2.1). Main-loop only, same as
// every other Tracks mutation here. Each setter rejects an out-of-range
// track or value and returns false rather than clamping, so a malformed
// MSG_TRACK_OP is visible instead of silently landing on Track 0.
//
// Only midi_in has behaviour today: poly_limit/priority are stored for stage
// 8 (which measures before it implements a steal policy) and program_change
// for stage 6 (Bank recall).
bool SetTrackMidiIn(uint8_t track, uint8_t midi_in);  // TrackMidiIn encoding
uint8_t TrackMidiIn(uint8_t track);
bool SetTrackPolyLimit(uint8_t track, uint8_t limit);  // 0 = none, else <= WAVEX_NUM_VOICES
uint8_t TrackPolyLimit(uint8_t track);
bool SetTrackPriority(uint8_t track, uint8_t priority);
uint8_t TrackPriority(uint8_t track);
bool SetTrackProgramChange(uint8_t track, bool enabled);
bool TrackProgramChange(uint8_t track);

// Which Tracks hear a note-on that arrived on MIDI `channel` (0-based)?
// Fan-out: several Tracks listening on one channel is a layer (§2.2).
// Writes up to `max` indices, returns the count.
uint8_t TracksForMidiChannel(uint8_t channel, uint8_t* out, uint8_t max);

}  // namespace SfzLoader
}  // namespace AudioEngine
}  // namespace WaveX
