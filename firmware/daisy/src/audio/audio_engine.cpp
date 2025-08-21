#include "audio_engine.h"
#include "../sampler.hpp"
#include "../cv_bus.hpp"
#include "../timebase.hpp"

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

// Preview state
static std::vector<int16_t> s_preview;
static uint32_t s_prev_sent = 0;

// Envelope gate state
static bool s_envelope_gate = false;

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
    for (size_t i = 0; i < size; i++)
    {
        float sample = s_oscillator.Process();
        out[0][i] = sample;
        out[1][i] = sample;
    }

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

} // namespace AudioEngine
} // namespace WaveX


