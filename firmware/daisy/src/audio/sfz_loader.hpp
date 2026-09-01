#pragma once

// Target-side SFZ resident loader. Runtime work is cooperative: Begin() only
// accepts a request and Pump() performs at most one parse/probe/allocation/read
// step. FatFs and allocation never run in the audio callback.

#include "spi_protocol/protocol.h"

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

bool SlotLoaded(uint8_t slot);
uint8_t ResolveNote(
    uint8_t slot, uint8_t note, uint8_t velocity, VoiceTriggerParams* out, uint8_t max);

}  // namespace SfzLoader
}  // namespace AudioEngine
}  // namespace WaveX
