#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include "audio_engine.h"
#include "../sampler.hpp"
#include "../cv_bus.hpp"
#include "../timebase.hpp"
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

// Simple single-producer (main loop) / single-consumer (audio IRQ) ring buffer of stereo int16 frames
static const uint32_t RB_CAP_FRAMES = 8192; // ~170ms at 48k (increased for better buffering)
static volatile uint32_t s_rb_head = 0; // write counter (frames)
static volatile uint32_t s_rb_tail = 0; // read counter (frames)
static int16_t s_rb[RB_CAP_FRAMES * 2]; // interleaved L,R

static inline uint32_t rb_count_frames()
{
    uint32_t head = s_rb_head;
    uint32_t tail = s_rb_tail;
    return (head - tail) & (RB_CAP_FRAMES - 1u);
}

static inline uint32_t rb_free_frames()
{
    return (RB_CAP_FRAMES - 1u) - rb_count_frames();
}

static inline void rb_push_stereo(int16_t l, int16_t r)
{
    uint32_t head = s_rb_head;
    uint32_t idx = (head & (RB_CAP_FRAMES - 1u)) * 2u;
    s_rb[idx + 0] = l;
    s_rb[idx + 1] = r;
    s_rb_head = head + 1u;
}

static inline bool rb_pop_stereo(int16_t &l, int16_t &r)
{
    uint32_t tail = s_rb_tail;
    if (tail == s_rb_head)
        return false;
    uint32_t idx = (tail & (RB_CAP_FRAMES - 1u)) * 2u;
    l = s_rb[idx + 0];
    r = s_rb[idx + 1];
    s_rb_tail = tail + 1u;
    return true;
}

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
    s_oscillator.SetFreq(440.0f);
    s_oscillator.SetAmp(0.5f);

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
            // Fallback to oscillator if no file, otherwise silence
            if (!s_wav.open)
            {
                float sample = s_oscillator.Process();
                out[0][i] = sample;
                out[1][i] = sample;
                continue;
            }
        }

        out[0][i] = (float)l16 / 32768.0f;
        out[1][i] = (float)r16 / 32768.0f;
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

    // Reset ring buffer
    s_rb_head = 0;
    s_rb_tail = 0;
    #if WAVEX_DAISY_SD_DEBUG
    if (s_hw) s_hw->PrintLine("WAV open ok: %s ch=%u sr=%lu bits=%u size=%lu", path, (unsigned)num_ch, (unsigned long)sample_rate, (unsigned)bits, (unsigned long)data_size);
    #endif
    return true;
}

void CloseWav()
{
    if (s_wav.open) {
        f_close(&s_wav.file);
        s_wav = {};
    }
}

bool IsWavPlaying()
{
    return s_wav.open;
}

bool ShouldPumpWavIO()
{
    if (!s_wav.open) return false;
    
    // Only pump I/O when ring buffer is less than 25% full
    // This reduces unnecessary file I/O operations
    uint32_t count = rb_count_frames();
    uint32_t threshold = RB_CAP_FRAMES / 4;  // 25% of buffer capacity
    
    return count < threshold;
}

void PumpWavIO()
{
    if (!s_wav.open)
        return;

    // Fill ring buffer with as many frames as we can
    uint32_t free_frames = rb_free_frames();
    if (free_frames == 0)
        return;

    // Read in chunks; align to frames (optimized for performance)
    uint32_t frames_per_read = 2048;  // Increased to 2048 frames for better performance
    if (frames_per_read > free_frames) frames_per_read = free_frames;
    // Bytes per frame in file
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * 2u;
    uint32_t req_bytes = frames_per_read * file_bpf;
    if (req_bytes > s_wav.bytes_remaining) req_bytes = s_wav.bytes_remaining;
    if (req_bytes == 0) {
        // Loop: seek back to data start
        f_lseek(&s_wav.file, s_wav.data_start);
        s_wav.bytes_remaining = s_wav.data_size;
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("WAV loop: rewinding to data start");
        #endif
        return;
    }

    // Temp buffer on stack for simplicity (monophonic doubles to stereo during push)
    // MOVED to static to ensure it's in DMA-accessible RAM (increased for larger reads)
    static uint8_t tmp[8192];  // Increased from 2048 to 8192 bytes for larger reads
    if (req_bytes > sizeof(tmp)) req_bytes = sizeof(tmp);
    UINT br = 0;

    // #if WAVEX_DAISY_SD_DEBUG
    // if (s_hw) s_hw->PrintLine("WAV read: req=%u, offset=%lu", (unsigned)req_bytes, (unsigned long)f_tell(&s_wav.file));
    // #endif

    FRESULT fr = f_read(&s_wav.file, tmp, req_bytes, &br);

    // #if WAVEX_DAISY_SD_DEBUG
    // if (s_hw) s_hw->PrintLine("WAV read: result fr=%d, br=%u, new_offset=%lu", (int)fr, (unsigned)br, (unsigned long)f_tell(&s_wav.file));
    // #endif

    if (fr != FR_OK || br == 0) {
        // On read error, stop playback
        #if WAVEX_DAISY_SD_DEBUG
        if (s_hw) s_hw->PrintLine("WAV read error: fr=%d br=%u", (int)fr, (unsigned)br);
        #endif
        CloseWav();
        return;
    }
    s_wav.bytes_remaining -= br;

    // Convert and push
    if (s_wav.num_channels == 2) {
        const int16_t* s = (const int16_t*)tmp;
        uint32_t frames = br / 4u; // 4 bytes per stereo frame
        for (uint32_t i = 0; i < frames; ++i) {
            if (rb_free_frames() == 0) break;
            int16_t L = s[i * 2 + 0];
            int16_t R = s[i * 2 + 1];
            rb_push_stereo(L, R);
        }
    } else {
        const int16_t* s = (const int16_t*)tmp;
        uint32_t frames = br / 2u; // 2 bytes per mono frame
        for (uint32_t i = 0; i < frames; ++i) {
            if (rb_free_frames() == 0) break;
            int16_t M = s[i];
            rb_push_stereo(M, M);
        }
    }
}

} // namespace AudioEngine
} // namespace WaveX



#endif // WAVEX_AUDIO_ENGINE_ENABLED
