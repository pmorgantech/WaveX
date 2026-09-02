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

/// Applies one mixer control change (MSG_MIX_OP). Main loop only: it writes
/// the track table the audio callback reads, and every field is a single
/// aligned word, so a change lands whole on the next block.
void OnMixOp(const WaveX::Protocol::MixOpMessage& m);
void OnNoteOn(const WaveX::Protocol::NoteMessage& m);
void OnNoteOff(const WaveX::Protocol::NoteMessage& m);
void OnSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m);
// OnPreviewReq only builds the preview buffer; PumpPreviewSend() (called
// every main-loop pass, like PumpWavIO/PumpEnvelopeJob) sends it one frame
// at a time so a full TX queue means "retry next pass", never "give up and
// truncate".
void OnPreviewReq(const WaveX::Protocol::PreviewReqMessage& m);
void PumpPreviewSend();

// Waveform envelope (roadmap 1.5.5 item 2). OnEnvelopeReq only accepts the
// request - measuring a min/max envelope touches every sample in the window,
// which is ~16 M reads for a three-minute stereo file and would stall the main
// loop past the ring's headroom if it ran here. PumpEnvelopeJob does the work
// in budgeted slices from the main loop and sends each chunk as it is
// measured; a new request replaces whatever was in flight.
void OnEnvelopeReq(const WaveX::Protocol::EnvelopeReqMessage& m);
void PumpEnvelopeJob();
void OnSampleLoad(const WaveX::Protocol::SampleLoadMessage& m);
void GetSampleMemStatus(WaveX::Protocol::SampleMemStatusMessage& out);

// SFZ v1 boot importer. Parses and loads one resident instrument into `slot`
// from the main loop before StartAudio(); false is non-fatal (missing/invalid
// file, unsupported WAV, or RAM budget refusal). Runtime rebinding and UI are
// deliberately outside the narrow first slice.
bool LoadSfzInstrument(const char* path, uint8_t slot);
void OnInstrumentOp(const WaveX::Protocol::InstOpMessage& request);
void PumpInstrumentLoad();

// Meter helpers
void GetMeters(BlockMeters& out);

// Non-destructive playback edit for the streaming audition (frames, absolute,
// at the file's own rate). The backend clamps: start <= loop_start <
// loop_end <= end, with 0 meaning "to the end" for end_frame and loop_end. A
// loop shorter than 256 frames is refused rather than allowed to re-seek every
// pass and starve the ring - the frontend cannot know that limit.
// fade_in_ms / fade_out_ms are the playback-time region fades (roadmap 1.5.6
// item 3): a region that starts mid-waveform starts on a step, and a step is a
// click. Clamped to the region length here, since only the backend knows what
// the region ended up being after its own clamping.
void SetEditParams(uint8_t slot,
                   bool loop_enabled,
                   int16_t gain_db_x10,
                   uint32_t start_frame,
                   uint32_t end_frame,
                   uint32_t loop_start_frame,
                   uint32_t loop_end_frame,
                   uint16_t fade_in_ms,
                   uint16_t fade_out_ms);

// Read position of the streaming audition within its region, in frames.
// Runs ahead of the audible position by the ring + filled SD slots (~42 ms):
// fine for a progress bar, not a playhead. False when nothing is open.
bool GetPlaybackPosition(uint32_t& frames_played, uint32_t& region_frames);

// Silence inserted between loop passes of the streaming audition, in ms.
// A property of the current audition rather than of the sample: the browser
// asks for a gap so a short file does not read as a drone, the editor asks
// for none so the loop seam is heard as it will actually play.
void SetLoopGapMs(uint16_t gap_ms);

// Which loaded sample MSG_NOTE_ON addresses. 0 restores the pre-selection
// behaviour: the most recently loaded playable sample.
void SelectSample(uint16_t sample_id);
uint16_t SelectedSample();

// Frees a loaded sample's RAM, stopping any voice sounding from it first.
// Returns false if the id is 0 or is not loaded.
bool UnloadSample(uint16_t sample_id);

// Re-sends the per-sample record. sample_id 0 means every loaded sample.
void PushAllSampleMeta(uint16_t sample_id);

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

// Lowest ring-buffer occupancy (frames) seen since the previous call, sampled
// per audio callback. Reading resets it. Ring capacity is 2048 frames / ~42 ms.
uint32_t TakeRingLowWater();

// True once if playback stopped for a storage reason (card removed, or reads
// failing past recovery) rather than a user request. Reading clears it. The
// frontend must be told, or it stays in audition mode over a dead card.
bool TakePlaybackAborted();
void MarkPlaybackAborted();

// Failed SD reads and the last FatFS result code. Counted separately from
// GetIOStats()'s count, which only tracks successful reads.
void GetIOErrors(uint32_t& errors, uint32_t& last_result);

// SD read throughput and latency for the interval since the previous call.
// Reading RESETS the accumulators, so each report describes its own interval
// rather than the whole run - which is what makes a degradation visible.
void TakeIOThroughput(
    uint32_t& bytes, uint32_t& reads, uint32_t& avg_us, uint32_t& min_us, uint32_t& max_us);

// Ring pushes, discarded passes and underrun episodes for the interval since
// the previous call. Reading RESETS them. Discards deserve their own counter:
// skip-without-consume is how two separate playback stalls began, and it is
// invisible in every other figure - the ring simply stops filling.
void TakeStreamCounters(uint32_t& pushes, uint32_t& discards, uint32_t& underruns);

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

}  // namespace AudioEngine
}  // namespace WaveX

#endif  // WAVEX_AUDIO_ENGINE_ENABLED
