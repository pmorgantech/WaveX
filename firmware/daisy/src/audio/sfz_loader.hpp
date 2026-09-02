#pragma once

// Target-side SFZ resident loader. Runtime work is cooperative: Begin() only
// accepts a request and Pump() performs at most one parse/probe/allocation/read
// step. FatFs and allocation never run in the audio callback.

#include "spi_protocol/protocol.h"

#include "instrument.hpp"
#include "mod_matrix.hpp"
#include "voice_manager.hpp"
#include <cstdint>

class SampleMemMgr;

namespace WaveX {
namespace AudioEngine {
namespace SfzLoader {

void Reset();
bool Begin(const WaveX::Protocol::InstOpMessage& request);
void Pump(SampleMemMgr& memory, uint8_t* io_buffer, uint32_t io_buffer_bytes);
bool Busy();
bool SlotLoading(uint8_t slot);
bool NeedsVoiceStop();
void ConfirmVoicesStopped(SampleMemMgr& memory);

// Boot compatibility: runs the same cooperative state machine to completion
// before audio starts. request_id 0 suppresses frontend status messages.
bool Load(const char* path,
          uint8_t slot,
          SampleMemMgr& memory,
          uint8_t* io_buffer,
          uint32_t io_buffer_bytes);

// True when `slot` holds ANY instrument - imported from an .sfz or built
// on-device by BindSample() - i.e. when a note on it can resolve at all.
bool SlotLoaded(uint8_t slot);

// The bank is the one note-resolution path (roadmap Phase 2.5 item 1): a
// bare sample bound to a slot is a one-zone Instrument in it, not a second
// registry beside it. Its zone spans the whole keyboard, pitch-tracks from
// `root_note`, and takes filter/ADSR from the live params at trigger
// (ZONE_FLAG_LIVE_FILTER_ENV) - exactly what the retired bare-WAV note path
// did, now as editable zone state rather than constants in the note path.
//
// sample_id is a WAV-registry id (MSG_SAMPLE_LOAD's), resolved through the
// resolver the engine registers with SetLoadedSampleResolver(). sample_id 0
// unbinds. Refused (false) when the slot holds an SFZ import: its samples
// are owned by this loader and can only be released through the load
// handshake (NeedsVoiceStop/ConfirmVoicesStopped), which a plain bind must
// not trigger - load another .sfz elsewhere or reboot to free it. Keeps the
// slot's mod slots either way; a bind is not an edit of them.
bool BindSample(uint8_t slot, uint16_t sample_id, uint8_t root_note = 60);
// The registry id a Built slot's first zone plays, else 0.
uint16_t BoundSample(uint8_t slot);
// A WAV-registry sample is going away: drop every Built zone that names
// it (and the slot's binding if that empties it), so no note resolves to a
// freed block. The engine calls this before it releases the memory.
void ForgetLoadedSample(uint16_t sample_id);
void SetLoadedSampleResolver(const SampleResolver& resolver);

// `live` feeds ZONE_FLAG_LIVE_FILTER_ENV zones; nullptr is allowed.
uint8_t ResolveNote(uint8_t slot,
                    uint8_t note,
                    uint8_t velocity,
                    const VoiceLiveParams* live,
                    VoiceTriggerParams* out,
                    uint8_t max);

// Modulation matrix (param-locks-and-modulation.md §9 stage 4). Main-loop
// context only (message dispatch) - mirrors every other instrument-bank
// mutation in this file. Out-of-range slot/mod_slot_index is a no-op
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
bool SetModSlot(uint8_t slot, uint8_t mod_slot_index, const ModSlot& value);
const ModSlot* GetModSlots(uint8_t slot);

}  // namespace SfzLoader
}  // namespace AudioEngine
}  // namespace WaveX
