#pragma once

#include "../config.hpp"
#include <cstdint>
#if WAVEX_AUDIO_ENGINE_ENABLED

#include "daisy_seed.h"
#include "spi_protocol/protocol.h"

namespace WaveX {
namespace AudioEngine {

struct BlockMeters {
    float rmsL;
    float rmsR;
    float peakL;
    float peakR;
};

enum class AudioOutputMode : uint8_t {
    StereoSAI1 = 2,
    VoiceSAI2 = 8,
};

void Init(daisy::DaisySeed& hw, float sample_rate, bool sdram_available);
void Callback(daisy::AudioHandle::InputBuffer in,
              daisy::AudioHandle::OutputBuffer out,
              size_t size);

// Control/message hook APIs
void OnControlChange(const WaveX::Protocol::ControlChangeMessage& m);
void OnNoteOn(const WaveX::Protocol::NoteMessage& m);
void OnNoteOff(const WaveX::Protocol::NoteMessage& m);
void OnSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m);
void OnPreviewReq(const WaveX::Protocol::PreviewReqMessage& m);
void OnSampleLoad(const WaveX::Protocol::SampleLoadMessage& m);
void GetSampleMemStatus(WaveX::Protocol::SampleMemStatusMessage& out);

// Meter helpers
void GetMeters(BlockMeters& out);

// WAV playback control
bool OpenWav(const char* path);
void CloseWav();
void PumpWavIO();
bool IsWavPlaying();
bool ShouldPumpWavIO();   // Adaptive polling helper
bool IsPrebufferReady();  // Check if pre-buffering is complete

// Performance monitoring
void GetIOStats(uint32_t& count, uint32_t& max_duration, uint32_t& last_duration);

// Audio callbacks executed since boot. Advances at sample_rate/block_size
// (1 kHz here) whenever the SAI DMA is running, independent of the main loop.
uint32_t GetCallbackBlocks();

// Failed SD reads and the last FatFS result code. Counted separately from
// GetIOStats()'s count, which only tracks successful reads.
void GetIOErrors(uint32_t& errors, uint32_t& last_result);

// Streaming telemetry (see WAVEX_DAISY_STREAM_DEBUG in hardware_config.h).
void GetStreamDebug(uint32_t& prebuf_filled,
                    uint32_t& prebuf_target,
                    uint32_t& wav_sample_rate,
                    uint8_t& wav_channels,
                    uint8_t& wav_bits);
void GetStreamDiscardDebug(uint32_t& free_frames,
                           uint32_t& want_frames,
                           uint32_t& resampled,
                           uint32_t& pushes);
void SetOutputMode(AudioOutputMode mode);
AudioOutputMode GetOutputMode();
uint32_t GetOutputChannelCount();
void GetDwtStats(uint32_t& callback_cycles, uint32_t& io_cycles);

// CPU load monitoring (audio processing performance)
float GetAvgCpuLoad();     // 0.0-1.0 smoothed average
float GetMinCpuLoad();     // 0.0-1.0 minimum observed
float GetMaxCpuLoad();     // 0.0-1.0 maximum observed
float GetBlockPeriodMs();  // Block period in milliseconds

// Underrun monitoring (called from main loop to avoid blocking audio)
void CheckAndLogUnderruns();

// Stage A CV flush (roadmap item 5): performs the blocking MCP4728 I2C
// write for CV values staged at the control tick. Main-loop only.
void FlushCv();

// CV calibration workflow (item 5 stage 4) - main-loop message handlers.
void OnCvCalSet(const WaveX::Protocol::CvCalMessage& m);
void OnCvCalGet(const WaveX::Protocol::CvCalGetMessage& m);
void OnCvTest(const WaveX::Protocol::CvTestMessage& m);
// Load the persisted calibration table (call once at boot, after SD mount).
void LoadCvCalFromSd();

// Sequencer / transport / MIDI-clock hooks (Phase 2). These forward to the
// engine-owned SequencerTransport (firmware/daisy/src/sequencer/), which
// drives the step scheduler from the control tick. Main-loop dispatch.
void OnSeqTransport(const WaveX::Protocol::SeqTransportMessage& m);
void OnSeqPatternOp(const WaveX::Protocol::SeqPatternOpMessage& m);
void OnMidiClockEvent(const WaveX::Protocol::MidiClockEventMessage& m);
void OnMidiCc(const WaveX::Protocol::MidiCcMessage& m);

// Sample audition control (for Sample Load/Save page)
bool AuditionSample(const char* path);
void StopAudition();

}  // namespace AudioEngine
}  // namespace WaveX

#endif  // WAVEX_AUDIO_ENGINE_ENABLED
