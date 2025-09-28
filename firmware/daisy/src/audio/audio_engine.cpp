#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include "audio_engine.h"
#include "../sampler.hpp"
#include "../cv_bus.hpp"
#include "../timebase.hpp"
#include "daisy_core.h"  // For memory sections
#include "sys/dma.h"     // For cache management
#include "stm32h7xx_ll_cortex.h" // For ARM atomic operations
#include <cmath>
#include <vector>
#include <cstdint>
#include <cstring>
#include "ff.h"

using namespace daisy;
using namespace daisysp;
using namespace WaveX::Protocol;

namespace WaveX {
namespace AudioEngine {

static DaisySeed* s_hw = nullptr;

// Parameters
struct AudioParameters {
    float volume = 1.0f;
    float filter_cutoff = 2000.0f;
    float filter_resonance = 0.5f;
    float envelope_attack = 0.01f;
    float envelope_decay = 0.1f;
    float envelope_sustain = 0.7f;
    float envelope_release = 0.5f;
    float lfo_rate = 1.0f;
    float lfo_depth = 0.1f;
};
static AudioParameters s_params;

// DSP objects
static Svf s_filter;
static Adsr s_envelope;
static Oscillator s_lfo;
static Oscillator s_oscillator;
static Sampler s_sampler;
static CvBus   s_cv;

static volatile float s_env_level = 0.0f;
static float s_last_in_block[Timebase::kBlockSize] = {0};
static size_t s_last_block_size = 0;
static BlockMeters s_last_block_meters = {0, 0, 0, 0};

// Preview state
static std::vector<int16_t> s_preview;
static uint32_t s_prev_sent = 0;

// Envelope gate state
static bool s_envelope_gate = false;

// ============================
// WAV playback state
// ============================
struct WavState {
    bool        open;
    FIL         file;
    uint32_t    data_start;
    uint32_t    data_size;
    uint32_t    bytes_remaining;
    uint16_t    num_channels;
    uint16_t    bits_per_sample;
    uint32_t    sample_rate;
};
static WavState s_wav = {};

// ============================
// Sample audition state (separate from main WAV playback)
// ============================
struct AuditionState {
    bool        active;
    FIL         file;
    uint32_t    data_start;
    uint32_t    data_size;
    uint32_t    bytes_remaining;
    uint16_t    num_channels;
    uint16_t    bits_per_sample;
    uint32_t    sample_rate;
    char        current_path[96];
};
static AuditionState s_audition = {};

// Thread-safe single-producer (main loop) / single-consumer (audio IRQ) ring buffer of stereo int16 frames
// Uses ARM Cortex-M7 atomic operations and memory barriers for race-condition-free operation
// This eliminates warbling caused by timing variations between main loop and audio callback
// Using regular memory for larger buffers - DMA memory was too constrained
static const uint32_t RB_CAP_FRAMES = 2048; // ~46ms at 44.1k (better latency) (excellent buffering for smooth playback)
static volatile uint32_t s_rb_head = 0; // write counter (frames) - atomic access with memory barriers
static volatile uint32_t s_rb_tail = 0; // read counter (frames) - atomic access with memory barriers
static int16_t s_rb[RB_CAP_FRAMES * 2] DMA_BUFFER_MEM_SECTION; // interleaved L,R - DMA memory (8KB, fits comfortably)

// Pre-buffering system for smooth playback start
static const uint32_t PREBUFFER_FRAMES = 1024; // ~23ms at 44.1kHz (much more responsive for auditioning)
static int16_t s_prebuffer[PREBUFFER_FRAMES * 2]; // ~23ms of stereo audio
static uint32_t s_prebuffer_filled = 0; // Number of frames pre-buffered
static bool s_prebuffer_ready = false; // Whether pre-buffer is ready for playback
static bool s_prebuffering = false; // Whether we're currently pre-buffering

// Background SD I/O system with larger buffers
static const uint32_t SD_BUFFER_SIZE = 8192; // 8KB SD read buffer for better performance
static uint8_t s_sd_buffer[SD_BUFFER_SIZE]; // SD read buffer in regular memory
static uint32_t s_sd_buffer_pos = 0; // Current position in SD buffer
static uint32_t s_sd_buffer_frames = 0; // Number of frames available in SD buffer
static bool s_sd_buffer_valid = false; // Whether SD buffer contains valid data
// Audio performance instrumentation
static uint32_t s_io_start_time = 0;
static uint32_t s_io_duration = 0;
static uint32_t s_max_io_duration = 0;
static uint32_t s_io_count = 0;
static uint32_t s_last_io_log = 0;
static uint32_t s_last_io_time = 0; // Last time we did SD I/O (for rate limiting)

// Resampling temporarily disabled

// Pre-buffering functions
static bool prebuffer_audio()
{
    if (!s_wav.open || s_prebuffer_ready) return true;
    
    s_prebuffering = true;
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * 2u;
    uint32_t frames_to_read = PREBUFFER_FRAMES - s_prebuffer_filled;
    
    if (frames_to_read == 0) {
        s_prebuffer_ready = true;
        s_prebuffering = false;
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("Pre-buffer complete: %u frames ready", (unsigned)PREBUFFER_FRAMES);
        #endif
        return true;
    }
    
    // Calculate how much to read
    uint32_t max_frames = SD_BUFFER_SIZE / file_bpf;
    uint32_t req_bytes = (frames_to_read < max_frames ? frames_to_read : max_frames) * file_bpf;
    
    if (req_bytes > s_wav.bytes_remaining) {
        req_bytes = s_wav.bytes_remaining;
    }
    
    if (req_bytes == 0) {
        // End of file reached during pre-buffering
        s_prebuffer_ready = true;
        s_prebuffering = false;
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("Pre-buffer complete: %u frames (end of file)", (unsigned)s_prebuffer_filled);
        #endif
        return true;
    }
    
    // Read from SD card
    UINT br = 0;
    s_io_start_time = System::GetTick(); // Start timing
    FRESULT fr = f_read(&s_wav.file, s_sd_buffer, req_bytes, &br);
    s_io_duration = System::GetTick() - s_io_start_time; // End timing
    
    // Track I/O performance
    s_io_count++;
    if (s_io_duration > s_max_io_duration) {
        s_max_io_duration = s_io_duration;
    }
    
    // Log I/O performance every 100 operations
    if (s_io_count % 100 == 0) {
        if (s_hw) s_hw->PrintLine("SD I/O Stats: count=%u, max_duration=%u ms, last_duration=%u ms", 
                                   (unsigned)s_io_count, (unsigned)s_max_io_duration, (unsigned)s_io_duration);
    }
    
    if (fr != FR_OK || br == 0) {
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("Pre-buffer read error: fr=%d, br=%u", (int)fr, (unsigned)br);
        #endif
        s_prebuffering = false;
        return false;
    }
    
    s_wav.bytes_remaining -= br;
    
    // Copy to pre-buffer
    uint32_t frames_read = br / file_bpf;
    const int16_t* src = (const int16_t*)s_sd_buffer;
    int16_t* dst = &s_prebuffer[s_prebuffer_filled * 2];
    
    if (s_wav.num_channels == 2) {
        // Stereo: copy L,R pairs
        for (uint32_t i = 0; i < frames_read; ++i) {
            dst[i * 2 + 0] = src[i * 2 + 0];
            dst[i * 2 + 1] = src[i * 2 + 1];
        }
    } else {
        // Mono: duplicate to both channels
        for (uint32_t i = 0; i < frames_read; ++i) {
            dst[i * 2 + 0] = src[i];
            dst[i * 2 + 1] = src[i];
        }
    }
    
    s_prebuffer_filled += frames_read;
    
    #if WAVEX_DAISY_SD_DEBUG
    if (s_hw) s_hw->PrintLine("Pre-buffer progress: %u/%u frames", (unsigned)s_prebuffer_filled, (unsigned)PREBUFFER_FRAMES);
    #endif
    
    return true;
}

// Background SD I/O functions
static bool refill_sd_buffer()
{
    if (!s_wav.open) return false;
    
    // Check if we need to refill
    if (s_sd_buffer_valid && s_sd_buffer_pos < s_sd_buffer_frames) {
        return true; // Still have data
    }
    
    // Calculate how much to read
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * 2u;
    uint32_t max_frames = SD_BUFFER_SIZE / file_bpf;
    uint32_t req_bytes = max_frames * file_bpf;
    
    if (req_bytes > s_wav.bytes_remaining) {
        req_bytes = s_wav.bytes_remaining;
    }
    
    if (req_bytes == 0) {
        // Loop: seek back to data start
        f_lseek(&s_wav.file, s_wav.data_start);
        s_wav.bytes_remaining = s_wav.data_size;
        req_bytes = max_frames * file_bpf;
        if (req_bytes > s_wav.bytes_remaining) {
            req_bytes = s_wav.bytes_remaining;
        }
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("WAV loop: rewinding to data start");
        #endif
    }
    
    if (req_bytes == 0) return false;
    
    // Read from SD card
    UINT br = 0;
    FRESULT fr = f_read(&s_wav.file, s_sd_buffer, req_bytes, &br);
    
    if (fr != FR_OK || br == 0) {
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("WAV read error: fr=%d, br=%u", (int)fr, (unsigned)br);
        #endif
        return false;
    }
    
    // Update state
    s_sd_buffer_frames = br / file_bpf;
    s_sd_buffer_pos = 0;
    s_sd_buffer_valid = true;
    s_wav.bytes_remaining -= br;
    
    #if WAVEX_DAISY_SD_DEBUG
    if (s_hw) s_hw->PrintLine("SD buffer refilled: %u frames, %u bytes", (unsigned)s_sd_buffer_frames, (unsigned)br);
    #endif
    
    return true;
}

// Thread-safe ring buffer operations with atomic access and memory barriers
static inline uint32_t rb_count_frames()
{
    // Atomic read with memory barrier to ensure consistency
    __DMB(); // Data Memory Barrier - ensure all previous memory operations complete
    uint32_t head = s_rb_head;
    uint32_t tail = s_rb_tail;
    __DMB(); // Ensure reads are completed before calculation
    return (head - tail) & (RB_CAP_FRAMES - 1u);
}

static inline uint32_t rb_free_frames()
{
    return (RB_CAP_FRAMES - 1u) - rb_count_frames();
}

static inline void rb_push_stereo(int16_t l, int16_t r)
{
    // Atomic write with memory barriers for thread safety
    uint32_t head = s_rb_head;
    uint32_t idx = (head & (RB_CAP_FRAMES - 1u)) * 2u;
    
    // Write data first
    s_rb[idx + 0] = l;
    s_rb[idx + 1] = r;
    
    // Memory barrier to ensure data is written before updating head
    __DMB(); // Data Memory Barrier - ensure data writes complete
    
    // Atomic update of head pointer
    s_rb_head = head + 1u;
    
    // Final memory barrier to ensure head update is visible
    __DMB(); // Ensure head update is committed to memory
}

static inline bool rb_pop_stereo(int16_t &l, int16_t &r)
{
    // Atomic read with memory barriers for thread safety
    __DMB(); // Data Memory Barrier - ensure all previous operations complete
    uint32_t tail = s_rb_tail;
    uint32_t head = s_rb_head;
    
    // Check if buffer is empty (atomic comparison)
    if (tail == head) {
        __DMB(); // Ensure comparison is complete
        return false;
    }
    
    // Read data
    uint32_t idx = (tail & (RB_CAP_FRAMES - 1u)) * 2u;
    l = s_rb[idx + 0];
    r = s_rb[idx + 1];
    
    // Memory barrier to ensure data is read before updating tail
    __DMB(); // Data Memory Barrier - ensure data reads complete
    
    // Atomic update of tail pointer
    s_rb_tail = tail + 1u;
    
    // Final memory barrier to ensure tail update is visible
    __DMB(); // Ensure tail update is committed to memory
    
    return true;
}

// Resampling temporarily disabled - using direct playback

void Init(DaisySeed& hw, float sample_rate)
{
    s_hw = &hw;

    s_filter.Init(sample_rate);
    s_filter.SetFreq(s_params.filter_cutoff);
    s_filter.SetRes(s_params.filter_resonance);
    s_filter.SetDrive(0.5f);

    s_envelope.Init(sample_rate);
    s_envelope.SetAttackTime(s_params.envelope_attack);
    s_envelope.SetDecayTime(s_params.envelope_decay);
    s_envelope.SetSustainLevel(s_params.envelope_sustain);
    s_envelope.SetReleaseTime(s_params.envelope_release);

    s_lfo.Init(sample_rate);
    s_lfo.SetWaveform(Oscillator::WAVE_SIN);
    s_lfo.SetFreq(s_params.lfo_rate);
    s_lfo.SetAmp(s_params.lfo_depth);

    s_oscillator.Init(sample_rate);
    s_oscillator.SetWaveform(Oscillator::WAVE_SIN);
    s_oscillator.SetFreq(1000.0f);  // Use 1kHz - simple test frequency
    s_oscillator.SetAmp(0.3f);      // Reduce amplitude slightly

    s_sampler.Init(sample_rate);
    s_cv.Init(0x60);
}

void Callback(AudioHandle::InputBuffer in,
              AudioHandle::OutputBuffer out,
              size_t size)
{
    (void)in;
    for (size_t i = 0; i < size; i++)
    {
        int16_t l16 = 0, r16 = 0;
        if (!rb_pop_stereo(l16, r16))
        {
            // Check if we have audition playback active
            if (s_audition.active && s_wav.open)
            {
                // Use pre-buffering system for audition to avoid blocking audio callback
                // The main loop will handle file I/O via PumpWavIO()
                // For now, output silence and let the pre-buffering system handle the data
                l16 = 0;
                r16 = 0;
                
                // Check if audition is complete (this will be handled by the main loop)
                // The audio callback should never do file I/O
            }
            else if (!s_wav.open)
            {
                // No audio should play on startup - output silence until audition commands
                // Requirement: "When daisy starts, no audio plays (no oscillator, no .wavs)"
                out[0][i] = 0.0f;
                out[1][i] = 0.0f;
                continue;
            }
            else
            {
                // WAV is playing but buffer is empty - output silence to prevent glitches
                out[0][i] = 0.0f;
                out[1][i] = 0.0f;
                // Log underrun only once per block to avoid spam
                static bool underrun_logged = false;
                if (!underrun_logged) {
                    if (s_hw) s_hw->PrintLine("AUDIO: Ring buffer underrun - outputting silence");
                    underrun_logged = true;
                }
                continue;
            }
        }

        out[0][i] = (float)l16 / 32768.0f;
        out[1][i] = (float)r16 / 32768.0f;
        
        // Reset underrun logging flag when we have data again
        static bool underrun_logged = false;
        if (underrun_logged) {
            underrun_logged = false;
        }
    }

    // Compute per-block meters
    float sumL = 0.f, sumR = 0.f;
    float pkL = 0.f, pkR = 0.f;
    for (size_t i = 0; i < size; ++i) {
        float l = out[0][i];
        float r = out[1][i];
        sumL += l * l;
        sumR += r * r;
        float al = fabsf(l);
        float ar = fabsf(r);
        if (al > pkL) pkL = al;
        if (ar > pkR) pkR = ar;
    }
    s_last_block_meters.rmsL = sqrtf(sumL / (float)size);
    s_last_block_meters.rmsR = sqrtf(sumR / (float)size);
    s_last_block_meters.peakL = pkL;
    s_last_block_meters.peakR = pkR;

    Timebase::Tick1kHz([]{
        // Future control logic
    });
}

void OnControlChange(const ControlChangeMessage& ctrl_msg)
{
    switch (ctrl_msg.parameter) {
        case PARAM_VOLUME:
            s_params.volume = ctrl_msg.value / 65535.0f;
            break;
        case PARAM_FILTER_CUTOFF:
            s_params.filter_cutoff = 20.0f + (ctrl_msg.value / 65535.0f) * 8000.0f;
            s_filter.SetFreq(s_params.filter_cutoff);
            break;
        case PARAM_FILTER_RESONANCE:
            s_params.filter_resonance = ctrl_msg.value / 65535.0f;
            s_filter.SetRes(s_params.filter_resonance);
            break;
        case PARAM_ENVELOPE_ATTACK:
            s_params.envelope_attack = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
            s_envelope.SetAttackTime(s_params.envelope_attack);
            break;
        case PARAM_ENVELOPE_DECAY:
            s_params.envelope_decay = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
            s_envelope.SetDecayTime(s_params.envelope_decay);
            break;
        case PARAM_ENVELOPE_SUSTAIN:
            s_params.envelope_sustain = ctrl_msg.value / 65535.0f;
            s_envelope.SetSustainLevel(s_params.envelope_sustain);
            break;
        case PARAM_ENVELOPE_RELEASE:
            s_params.envelope_release = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
            s_envelope.SetReleaseTime(s_params.envelope_release);
            break;
        case PARAM_LFO_RATE:
            s_params.lfo_rate = 0.1f + (ctrl_msg.value / 65535.0f) * 10.0f;
            s_lfo.SetFreq(s_params.lfo_rate);
            break;
        case PARAM_LFO_DEPTH:
            s_params.lfo_depth = ctrl_msg.value / 65535.0f;
            break;
    }
}

void OnNoteOn(const NoteMessage& note_msg)
{
    float freq = mtof(note_msg.note);
    s_oscillator.SetFreq(freq);
    s_envelope_gate = true;
    s_envelope.Retrigger(false);
    if (s_hw) s_hw->PrintLine("RX NOTE_ON: note=%u vel=%u ch=%u freq=%.2f", (unsigned)note_msg.note, (unsigned)note_msg.velocity, (unsigned)note_msg.channel, (double)freq);
}

void OnNoteOff(const NoteMessage& note_msg)
{
    (void)note_msg;
    s_envelope_gate = false;
    if (s_hw) s_hw->PrintLine("RX NOTE_OFF: note=%u ch=%u", (unsigned)note_msg.note, (unsigned)note_msg.channel);
}

void OnSampleCtrl(const SampleCtrlMessage& sc)
{
    switch(sc.cmd){
        case SAMPLE_REC_START: s_sampler.StartRec(); break;
        case SAMPLE_REC_STOP:  s_sampler.StopRec();  break;
        case SAMPLE_PLAY_START: s_sampler.StartPlay(sc.rate); break;
        case SAMPLE_PLAY_STOP:  s_sampler.StopPlay(); break;
    }
}

void OnPreviewReq(const PreviewReqMessage& pr)
{
    s_prev_sent = 0;
    s_sampler.MakePreview(pr.start, pr.end, pr.decim ? pr.decim : 1, s_preview);
}

void GetInputMeters(float& rms, float& peak)
{
    Sampler::BlockMeters(s_last_in_block, s_last_block_size, rms, peak);
}

void GetMeters(BlockMeters& out)
{
    out = s_last_block_meters;
}

// ============================
// WAV playback implementation
// ============================

static uint16_t read_le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t read_le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool OpenWav(const char* path)
{
    CloseWav();

    FRESULT fr = f_open(&s_wav.file, path, FA_READ);
    if (fr != FR_OK)
    {
        if (s_hw) s_hw->PrintLine("WAV open failed: f_open error %d for path %s", (int)fr, path);
        return false;
    }

    uint8_t hdr[44];
    UINT br = 0;
    fr = f_read(&s_wav.file, hdr, sizeof(hdr), &br);
    if (fr != FR_OK || br < 44) {
        if (s_hw) s_hw->PrintLine("WAV open failed: header read error %d, bytes read %u", (int)fr, (unsigned)br);
        f_close(&s_wav.file);
        return false;
    }
    // Validate RIFF/WAVE
    if (memcmp(hdr + 0, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        if (s_hw) s_hw->PrintLine("WAV open failed: not a RIFF/WAVE file");
        f_close(&s_wav.file);
        return false;
    }

    // Parse chunks to find fmt and data
    uint16_t audio_fmt = 0;
    uint16_t num_ch = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_off = 0;
    uint32_t data_size = 0;

    // Rewind and iterate using f_lseek for generality
    f_lseek(&s_wav.file, 12);
    while (true)
    {
        uint8_t chdr[8];
        fr = f_read(&s_wav.file, chdr, 8, &br);
        if (fr != FR_OK || br < 8) {
            if (s_hw) s_hw->PrintLine("WAV open failed: chunk header read error %d, bytes read %u", (int)fr, (unsigned)br);
            f_close(&s_wav.file);
            return false;
        }
        uint32_t cid = read_le32(chdr);
        uint32_t csz = read_le32(chdr + 4);
        if (cid == 0x20746d66) { // 'fmt '
            uint8_t fmt[16];
            if (csz < 16) {
                if (s_hw) s_hw->PrintLine("WAV open failed: fmt chunk too small");
                f_close(&s_wav.file);
                return false;
            }
            fr = f_read(&s_wav.file, fmt, 16, &br);
            if (fr != FR_OK || br < 16) {
                if (s_hw) s_hw->PrintLine("WAV open failed: fmt chunk read error");
                f_close(&s_wav.file);
                return false;
            }
            audio_fmt = read_le16(fmt + 0);
            num_ch = read_le16(fmt + 2);
            sample_rate = read_le32(fmt + 4);
            bits = read_le16(fmt + 14);
            // Skip any extra fmt bytes
            if (csz > 16) f_lseek(&s_wav.file, f_tell(&s_wav.file) + (csz - 16));
        } else if (cid == 0x61746164) { // 'data'
            data_off = f_tell(&s_wav.file);
            data_size = csz;
            // Position after header for reading
            break;
        } else {
            // skip unknown chunk
            f_lseek(&s_wav.file, f_tell(&s_wav.file) + csz);
        }
    }

    if (audio_fmt != 1 || (bits != 16) || (num_ch != 1 && num_ch != 2)) {
        if (s_hw) s_hw->PrintLine("WAV open failed: unsupported format fmt=%u bits=%u ch=%u", (unsigned)audio_fmt, (unsigned)bits, (unsigned)num_ch);
        f_close(&s_wav.file);
        return false; // only PCM16 mono/stereo supported
    }

    s_wav.open = true;
    s_wav.data_start = data_off;
    s_wav.data_size = data_size;
    s_wav.bytes_remaining = data_size;
    s_wav.num_channels = num_ch;
    s_wav.bits_per_sample = bits;
    s_wav.sample_rate = sample_rate;
    
    // Reset buffers
    s_rb_head = 0;
    s_rb_tail = 0;
    
    #if WAVEX_DAISY_SD_DEBUG
    if (s_hw) s_hw->PrintLine("WAV open ok: %s ch=%u sr=%lu bits=%u size=%lu", path, (unsigned)num_ch, (unsigned long)sample_rate, (unsigned)bits, (unsigned long)data_size);
    #endif
    
    // Reset pre-buffer state and start pre-buffering
    s_prebuffer_filled = 0;
    s_prebuffer_ready = false;
    s_prebuffering = false;
    
    return true;
}

void CloseWav()
{
    if (s_wav.open) {
        f_close(&s_wav.file);
        s_wav = {};
    }
    
    // Reset SD buffer state
    s_sd_buffer_pos = 0;
    s_sd_buffer_frames = 0;
    s_sd_buffer_valid = false;
    s_last_io_time = 0;
    
    // Reset pre-buffer state
    s_prebuffer_filled = 0;
    s_prebuffer_ready = false;
    s_prebuffering = false;
}

bool IsWavPlaying()
{
    return s_wav.open;
}

bool IsPrebufferReady()
{
    return s_prebuffer_ready;
}

void GetIOStats(uint32_t& count, uint32_t& max_duration, uint32_t& last_duration)
{
    count = s_io_count;
    max_duration = s_max_io_duration;
    last_duration = s_io_duration;
}

bool ShouldPumpWavIO()
{
    if (!s_wav.open) return false;
    
    // If we're pre-buffering, always pump I/O
    if (s_prebuffering) {
        return true;
    }
    
    // If pre-buffer is ready, use normal I/O pumping logic
    if (s_prebuffer_ready) {
        // Rate limit SD I/O to avoid blocking the main loop too frequently
        uint32_t now = System::GetNow();
        if (now - s_last_io_time < 10) { // Minimum 10ms between I/O operations
            return false;
        }
        
        // Pump I/O when ring buffer is less than 50% full
        // Good balance between performance and buffering
        uint32_t count = rb_count_frames();
        uint32_t threshold = RB_CAP_FRAMES / 2;  // 50% of buffer capacity
        
        return count < threshold;
    }
    
    // If pre-buffer is not ready, start pre-buffering
    return true;
}

void PumpWavIO()
{
    if (!s_wav.open)
        return;

    // Update I/O timing
    s_last_io_time = System::GetNow();

    // If we're pre-buffering, do that first
    if (s_prebuffering || !s_prebuffer_ready) {
        if (!prebuffer_audio()) {
            return; // Pre-buffering failed
        }
        return; // Continue pre-buffering next time
    }

    // If pre-buffer is ready, transfer data from pre-buffer to ring buffer
    if (s_prebuffer_ready && s_prebuffer_filled > 0) {
        uint32_t free_frames = rb_free_frames();
        if (free_frames == 0) return;

        // Calculate how many frames we can transfer from pre-buffer
        uint32_t frames_to_transfer = (s_prebuffer_filled < free_frames) ? s_prebuffer_filled : free_frames;
        
        if (frames_to_transfer == 0) return;

        // Transfer frames from pre-buffer to ring buffer
        for (uint32_t i = 0; i < frames_to_transfer; ++i) {
            int16_t L = s_prebuffer[i * 2 + 0];
            int16_t R = s_prebuffer[i * 2 + 1];
            rb_push_stereo(L, R);
        }
        
        // Remove transferred frames from pre-buffer
        s_prebuffer_filled -= frames_to_transfer;
        if (s_prebuffer_filled > 0) {
            // Shift remaining data to beginning of pre-buffer
            memmove(s_prebuffer, &s_prebuffer[frames_to_transfer * 2], s_prebuffer_filled * 2 * sizeof(int16_t));
        }
        
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("Transferred %u frames from pre-buffer to ring buffer", (unsigned)frames_to_transfer);
        #endif
        
        return;
    }

    // Normal I/O pumping (when pre-buffer is empty)
    // Refill SD buffer if needed (this does the actual SD I/O)
    if (!refill_sd_buffer()) {
        return; // Failed to refill, or no data available
    }

    // Transfer data from SD buffer to ring buffer
    uint32_t free_frames = rb_free_frames();
    if (free_frames == 0) return;

    // Calculate how many frames we can transfer
    uint32_t available_frames = s_sd_buffer_frames - s_sd_buffer_pos;
    uint32_t frames_to_transfer = (available_frames < free_frames) ? available_frames : free_frames;
    
    if (frames_to_transfer == 0) return;

    // Transfer frames from SD buffer to ring buffer
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * 2u;
    const int16_t* src = (const int16_t*)(s_sd_buffer + (s_sd_buffer_pos * file_bpf));
    
    if (s_wav.num_channels == 2) {
        // Stereo: copy L,R pairs
        for (uint32_t i = 0; i < frames_to_transfer; ++i) {
            int16_t L = src[i * 2 + 0];
            int16_t R = src[i * 2 + 1];
            rb_push_stereo(L, R);
        }
    } else {
        // Mono: duplicate to both channels
        for (uint32_t i = 0; i < frames_to_transfer; ++i) {
            int16_t M = src[i];
            rb_push_stereo(M, M);
        }
    }
    
    // Update SD buffer position
    s_sd_buffer_pos += frames_to_transfer;
    
    #if WAVEX_DAISY_SD_DEBUG
    if (s_hw) s_hw->PrintLine("Transferred %u frames from SD buffer to ring buffer", (unsigned)frames_to_transfer);
    #endif
}

// ============================================================================
// Sample Audition Functions (for Sample Load/Save page)
// ============================================================================

bool AuditionSample(const char* path)
{
    // Stop any current audition first
    StopAudition();

    // Use the existing WAV playback system for audition
    // This integrates with the pre-buffering system and avoids blocking I/O
    if (!OpenWav(path)) {
        if (s_hw) s_hw->PrintLine("AuditionSample: Failed to open WAV file for %s", path);
        return false;
    }

    // Mark audition as active for tracking
    s_audition.active = true;
    std::strncpy(s_audition.current_path, path, sizeof(s_audition.current_path) - 1);
    s_audition.current_path[sizeof(s_audition.current_path) - 1] = '\0';
    
    if (s_hw) {
        s_hw->PrintLine("AuditionSample: Started audition of %s using WAV playback system", path);
    }
    
    return true;
}

void StopAudition()
{
    if (s_audition.active) {
        // Stop the WAV playback system
        CloseWav();
        s_audition.active = false;
        if (s_hw) s_hw->PrintLine("AuditionSample: Stopped audition");
    }
}

} // namespace AudioEngine
} // namespace WaveX



#endif // WAVEX_AUDIO_ENGINE_ENABLED
