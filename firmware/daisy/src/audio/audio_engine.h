#pragma once

#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include "daisy_seed.h"
#include "daisysp.h"
#include "spi_protocol/protocol.h"

namespace WaveX {
namespace AudioEngine {

struct BlockMeters {
    float rmsL;
    float rmsR;
    float peakL;
    float peakR;
};

void Init(daisy::DaisySeed& hw, float sample_rate);
void Callback(daisy::AudioHandle::InputBuffer in,
              daisy::AudioHandle::OutputBuffer out,
              size_t size);

// Control/message hook APIs
void OnControlChange(const WaveX::Protocol::ControlChangeMessage& m);
void OnNoteOn(const WaveX::Protocol::NoteMessage& m);
void OnNoteOff(const WaveX::Protocol::NoteMessage& m);
void OnSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m);
void OnPreviewReq(const WaveX::Protocol::PreviewReqMessage& m);

// Meter helpers
void GetInputMeters(float& rms, float& peak);
void GetMeters(BlockMeters& out);

// WAV playback control
bool OpenWav(const char* path);
void CloseWav();
void PumpWavIO();
bool IsWavPlaying();
bool ShouldPumpWavIO();  // Adaptive polling helper
bool IsPrebufferReady(); // Check if pre-buffering is complete

// Performance monitoring
void GetIOStats(uint32_t& count, uint32_t& max_duration, uint32_t& last_duration);

// CPU load monitoring (audio processing performance)
float GetAvgCpuLoad();     // 0.0-1.0 smoothed average
float GetMinCpuLoad();     // 0.0-1.0 minimum observed
float GetMaxCpuLoad();     // 0.0-1.0 maximum observed
float GetBlockPeriodMs();  // Block period in milliseconds

// Sample audition control (for Sample Load/Save page)
bool AuditionSample(const char* path);
void StopAudition();

} // namespace AudioEngine
} // namespace WaveX


#endif // WAVEX_AUDIO_ENGINE_ENABLED

