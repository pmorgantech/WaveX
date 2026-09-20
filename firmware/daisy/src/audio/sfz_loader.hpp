#pragma once
#include "audio/sequencer_voice_map.hpp"

// Target-side SFZ resident loader. Runtime work is cooperative: Begin() only
// accepts a request and Pump() performs at most one parse/probe/allocation/read
// step. FatFs and allocation never run in the audio callback.

#include "spi_protocol/protocol.h"

#include "instrument.hpp"
#include "mod_matrix.hpp"
#include "sample_pool.hpp"
#include "voice_manager.hpp"
#include "wxi/wxi.hpp"
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
// Project restore: private empty Tracks plus a staged Pool with borrowed live
// PCM retained. Busy() covers the entire lease; normal Pump/Begin cannot touch
// it. Each Track is loaded once. Queries keep reporting the live bank.
// The caller freezes live Pool/Track edits, owns both scratch objects and
// drives only PumpProjectLoad until each Track finishes. No editor undo,
// revision or callback modulation state changes until FinishProjectLoad(true).
bool BeginProjectLoad(Tracks& candidate);
bool ProjectLoadActive();
bool BeginProjectTrack(uint8_t track, const char* path);
bool ProjectTrackBusy();
uint8_t ProjectTrackError();
void PumpProjectLoad(SamplePool&, SampleMemMgr&, uint8_t* io, uint32_t bytes);
void CancelProjectTrack(SamplePool&, SampleMemMgr&);
// Commit only after every dependency loads and the engine's audio stop fence.
// Keep notes/transport gated until Pool commit, Track commit and publication
// of all prepared voice maps finish. A failed/cancelled session cannot commit.
// Abort requires CancelProjectTrack first, then caller rolls back staged PCM.
// With only_track >= 0, copy that candidate Instrument to recall_targets
// (zero means only_track). All other Track fields remain live-owned.
bool FinishProjectLoad(bool commit, int only_track = -1, uint16_t recall_targets = 0);
// Bank recall shares the private Project loader lease, but installs only the
// selected Instrument; routing/mix and every other Track remain live.
bool BeginProjectDocument(uint8_t track, const Wxi::InstrumentFile& document);
// Under the same private lease, load/pin only dependencies, preserving all
// Track ownership. Candidate Tracks stay empty so documents can be repeated.
// End with FinishProjectLoad(false); the caller commits the additive Pool.
bool BeginProjectPreload(const Wxi::InstrumentFile& document);
// Preflight a Track snapshot without a temporary standalone WXI file. The
// caller serializes all edits until its borrowed document is no longer used.
bool BeginBankSnapshot(uint8_t track);
const Wxi::InstrumentFile& BankSnapshot();
// Project-owned WXI copy. Uses normal sample admission, but does not rename
// the live Instrument, apply its undo point, or emit an editor completion.
// Parent directory must already exist; destination is never overwritten.
bool BeginProjectSnapshot(uint8_t track, const char* destination);
uint8_t ProjectSnapshotError();  // meaningful after Busy() becomes false
// Returns true only for a newly applied edit; caller republishes that Track.
WaveX::Protocol::InstOscSyncMessage ReadOscState(uint8_t track, uint8_t oscillator);
WaveX::Protocol::InstEditSyncMessage ReadEditState(uint8_t track);
Allocation::Override TrackAllocation(uint8_t track);
WaveX::Protocol::AllocationSyncMessage ReadAllocationState(uint8_t track, uint8_t scope);
bool OnAllocationOp(const WaveX::Protocol::AllocationOpMessage& request);
bool OnEditOp(const WaveX::Protocol::InstEditOpMessage& request);
bool OnLfoOp(const WaveX::Protocol::InstLfoOpMessage& request);
WaveX::Protocol::InstLfoSyncMessage ReadLfoState(uint8_t track);
bool OnModOp(const WaveX::Protocol::InstModOpMessage& request);
WaveX::Protocol::InstModSyncMessage ReadModState(uint8_t track);
bool OnOscOp(const WaveX::Protocol::InstOscOpMessage& request);
bool OnKeyMapOp(const WaveX::Protocol::InstKeyMapOpMessage& request);
bool OnPadSoundOp(const WaveX::Protocol::InstPadSoundOpMessage& request);
void OnTrackStateRequest(const WaveX::Protocol::TrackStateRequest& request);
void PumpEditorReply();  // retained pad-map reply; main loop only
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
// Bare-sample id only for an unnamed, one-zone, full-range Quick Instrument;
// named, sparse or split keyboard maps and drum Instruments return 0.
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

// Filter/envelope come from the Track's Instrument (or a zone that
// overrides it), so this takes no engine state.
uint8_t ResolveNote(
    uint8_t track, uint8_t note, uint8_t velocity, VoiceTriggerParams* out, uint8_t max);

// Main-loop only: publish complete prepared zones, excluding loading Tracks.
void PrepareSequencerVoices(SequencerVoiceMap& map, uint16_t tracks = 0xFFFFu);

// Instrument-level filter and envelope (track-and-patch-model.md §3.2) -
// the defaults every zone follows unless it sets ZONE_FLAG_OWN_FILTER_ENV.
// Main-loop only, like every other Tracks mutation here. These are what the
// Instrument page's Filter and Env tabs edit, per Track, which is why they
// take a track rather than writing one engine-wide value.
bool SetInstrumentFilter(uint8_t track, const InstrumentFilter& filter);
bool SetInstrumentEnv(uint8_t track, const InstrumentEnv& env);
const InstrumentFilter* GetInstrumentFilter(uint8_t track);
const InstrumentEnv* GetInstrumentEnv(uint8_t track);
void PrepareLiveParams(uint8_t track, VoiceLiveParams& live);

// Modulation matrix (param-locks-and-modulation.md §9 stage 4). Main-loop
// context only (message dispatch) - mirrors every other Tracks
// mutation in this file. Out-of-range track/mod_slot_index is a no-op
// returning false.
//
// GetModSlots is callback-only: it acquires a complete table into private
// callback storage, independent of the main-loop Instrument record. A
// published edit never mutates a table currently being evaluated. Reset()
// initializes these mailboxes before audio starts.
bool SetModSlot(uint8_t track, uint8_t mod_slot_index, const ModSlot& value);
const ModSlot* GetModSlots(uint8_t track);

// Track settings (track-and-patch-model.md §2.1). Main-loop only, same as
// every other Tracks mutation here. Each setter rejects an out-of-range
// track or value and returns false rather than clamping, so a malformed
// MSG_TRACK_OP is visible instead of silently landing on Track 0.
//
// midi_in routes notes and enabled Program Changes; poly_limit/priority
// remain stored pending the measured allocation policy.
bool SetTrackMidiIn(uint8_t track, uint8_t midi_in);  // TrackMidiIn encoding
uint8_t TrackMidiIn(uint8_t track);
bool SetTrackPolyLimit(uint8_t track, uint8_t limit);  // legacy metadata only
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
