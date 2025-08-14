#include "daisy_seed.h"
#include "daisysp.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include "spi_protocol/protocol.h"
#include "sampler.hpp"
#include "cv_bus.hpp"
#include "timebase.hpp"

using namespace daisy;
using namespace daisysp;
using namespace WaveX::Protocol;

// Hardware
DaisySeed hw;

// Audio processing parameters
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
} params;

// DSP objects
Svf filter;
Adsr envelope;
Oscillator lfo;
Oscillator oscillator;
Sampler g_sampler;
CvBus   g_cv;

static volatile float g_env_level = 0.0f;
static float g_last_in_block[Timebase::kBlockSize] = {0};
static size_t g_last_block_size = 0;

// Preview state
static std::vector<int16_t> g_preview;
static uint32_t g_prev_sent = 0;

// Outgoing packet ready flag
static volatile bool g_tx_ready = false;

// Envelope gate state
bool envelope_gate = false;

// SPI communication
SPI_HandleTypeDef hspi1;
uint8_t spi_rx_buffer[128];
uint8_t spi_tx_buffer[128];
uint8_t spi_dummy_rx[128];
volatile bool spi_data_ready = false;
static volatile bool g_tx_inflight = false;
static uint16_t g_tx_len = 0;

// Daisy → ESP32 IRQ pin remapped to a header pin for easy wiring.
// Wire this pin to ESP32-S3 GPIO16.
static GPIO s_irq_pin;

static inline void IRQ_Init()
{
    // Choose a free Daisy header pin for IRQ (was PB0). Using seed::D21 (PC4).
    s_irq_pin.Init(seed::D21, GPIO::Mode::OUTPUT);
    s_irq_pin.Write(false);
}

static inline void IRQ_Raise() { s_irq_pin.Write(true); }
static inline void IRQ_Lower() { s_irq_pin.Write(false); }

static void SendPacket(uint8_t type, const void* payload, size_t length)
{
    if(length > MAX_PAYLOAD_SIZE) length = MAX_PAYLOAD_SIZE;
    Packet* p = reinterpret_cast<Packet*>(spi_tx_buffer);
    p->header.sync = SYNC_BYTE;
    p->header.type = type;
    p->header.length = (uint8_t)length;
    if(payload && length)
        memcpy(p->payload, payload, length);
    p->header.checksum = ProtocolHandler::CalculateChecksum(p->payload, p->header.length);
    g_tx_ready = true;
    g_tx_len = (uint16_t)(sizeof(PacketHeader) + p->header.length);
    // Prime TXRX so next master clock will shift the packet out
    if(!g_tx_inflight) {
        if(HAL_SPI_TransmitReceive_IT(&hspi1, spi_tx_buffer, spi_dummy_rx, g_tx_len) == HAL_OK) {
            g_tx_inflight = true;
            IRQ_Raise();
        }
    }
}

// Audio callback
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    static float inMono[128];
    for (size_t i = 0; i < size; i++)
    {
        // Prepare mono input for sampler
        inMono[i] = 0.5f * (in[0][i] + in[1][i]);
        // Update LFO
        float lfo_value = lfo.Process();
        
        // Process envelope with gate
        float env_value = envelope.Process(envelope_gate);
        g_env_level = env_value;
        
        // Apply modulation to filter cutoff
        float modulated_cutoff = params.filter_cutoff + (lfo_value * params.lfo_depth * 1000.0f);
        filter.SetFreq(modulated_cutoff);
        
        // Process input through filter
        float input_sample = in[0][i] * params.volume;
        filter.Process(input_sample);
        float filtered_sample = filter.Low();
        
        // Apply envelope
        float output_sample = filtered_sample * env_value;
        // Mix in sampler playback
        float s = g_sampler.Next();
        output_sample = tanhf(output_sample + s);
        
        // Output processed audio
        out[0][i] = output_sample;
        out[1][i] = output_sample; // Mono for now
    }

    // Feed sampler with current input block
    g_sampler.FeedInputBlock(inMono, size);
    // Save last input block for metering
    g_last_block_size = size > Timebase::kBlockSize ? Timebase::kBlockSize : size;
    for(size_t i=0;i<g_last_block_size;i++) g_last_in_block[i] = inMono[i];

    // 1 kHz control tick (block size set to 48 samples)
    Timebase::Tick1kHz([]{
        // Map current params to CVs (normalized 0..1)
        auto clamp01 = [](float x){ return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
        float norm_cut = clamp01((params.filter_cutoff - 20.0f) / (8000.0f - 20.0f));
        float norm_res = clamp01(params.filter_resonance);
        float norm_vca = clamp01(g_env_level);
        g_cv.QueueVoice(0, norm_cut, norm_res, norm_vca);
        g_cv.Flush();

        // Meter push every 20 ms
        static uint32_t meter_ctr = 0;
        if(++meter_ctr >= 20) {
            meter_ctr = 0;
            float rms=0, peak=0;
            Sampler::BlockMeters(g_last_in_block, g_last_block_size, rms, peak);
            MeterPushMessage m{rms, peak};
            if(!g_tx_ready) SendPacket(MSG_METER_PUSH, &m, sizeof(m));
        }

        // Stream preview chunks if pending
        if(!g_tx_ready && g_prev_sent < g_preview.size()) {
            WaveChunkMessage hdr{};
            hdr.offset = g_prev_sent;
            uint16_t samples_avail = (uint16_t)(g_preview.size() - g_prev_sent);
            uint16_t max_samples = (uint16_t)((MAX_PAYLOAD_SIZE - sizeof(WaveChunkMessage)) / 2);
            uint16_t count = samples_avail < max_samples ? samples_avail : max_samples;
            uint8_t payload[MAX_PAYLOAD_SIZE];
            memcpy(payload, &hdr, sizeof(hdr));
            memcpy(payload + sizeof(hdr), &g_preview[g_prev_sent], count * sizeof(int16_t));
            hdr.count = count; // update count after computing
            // Re-write header with count
            memcpy(payload, &hdr, sizeof(hdr));
            SendPacket(MSG_WAVE_CHUNK, payload, sizeof(hdr) + count * sizeof(int16_t));
            g_prev_sent += count;
        }
    });
}

// SPI callback for receiving data from ESP32
void WaveX_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        spi_data_ready = true;
        if(g_tx_inflight){
            g_tx_inflight = false;
            g_tx_ready = false;
            IRQ_Lower();
        }
    }
}

// Process incoming SPI messages
void ProcessSPIMessage(const uint8_t* buffer, size_t length)
{
    if (!ProtocolHandler::ValidatePacket(buffer, length)) {
        return; // Invalid packet
    }
    
    MessageType msg_type = ProtocolHandler::GetMessageType(buffer);
    
    switch (msg_type) {
        case MSG_CONTROL_CHANGE: {
            ControlChangeMessage ctrl_msg;
            if (ProtocolHandler::ParseControlChange(buffer, ctrl_msg)) {
                // Update parameters based on control change
                switch (ctrl_msg.parameter) {
                    case PARAM_VOLUME:
                        params.volume = ctrl_msg.value / 65535.0f;
                        break;
                    case PARAM_FILTER_CUTOFF:
                        params.filter_cutoff = 20.0f + (ctrl_msg.value / 65535.0f) * 8000.0f;
                        filter.SetFreq(params.filter_cutoff);
                        break;
                    case PARAM_FILTER_RESONANCE:
                        params.filter_resonance = ctrl_msg.value / 65535.0f;
                        filter.SetRes(params.filter_resonance);
                        break;
                    case PARAM_ENVELOPE_ATTACK:
                        params.envelope_attack = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
                        envelope.SetAttackTime(params.envelope_attack);
                        break;
                    case PARAM_ENVELOPE_DECAY:
                        params.envelope_decay = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
                        envelope.SetDecayTime(params.envelope_decay);
                        break;
                    case PARAM_ENVELOPE_SUSTAIN:
                        params.envelope_sustain = ctrl_msg.value / 65535.0f;
                        envelope.SetSustainLevel(params.envelope_sustain);
                        break;
                    case PARAM_ENVELOPE_RELEASE:
                        params.envelope_release = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
                        envelope.SetReleaseTime(params.envelope_release);
                        break;
                    case PARAM_LFO_RATE:
                        params.lfo_rate = 0.1f + (ctrl_msg.value / 65535.0f) * 10.0f;
                        lfo.SetFreq(params.lfo_rate);
                        break;
                    case PARAM_LFO_DEPTH:
                        params.lfo_depth = ctrl_msg.value / 65535.0f;
                        break;
                }
            }
            break;
        }
        
        case MSG_NOTE_ON: {
            NoteMessage note_msg;
            if (ProtocolHandler::ParseNoteMessage(buffer, note_msg)) {
                // Trigger envelope and set oscillator frequency
                float freq = mtof(note_msg.note);
                oscillator.SetFreq(freq);
                envelope_gate = true;
                envelope.Retrigger(false);
            }
            break;
        }
        
        case MSG_NOTE_OFF: {
            NoteMessage note_msg;
            if (ProtocolHandler::ParseNoteMessage(buffer, note_msg)) {
                // Release envelope
                envelope_gate = false;
            }
            break;
        }
        
        case MSG_SAMPLE_CTRL: {
            SampleCtrlMessage sc{};
            if(ProtocolHandler::ParseSampleCtrl(buffer, sc)) {
                switch(sc.cmd){
                    case SAMPLE_REC_START: g_sampler.StartRec(); break;
                    case SAMPLE_REC_STOP:  g_sampler.StopRec();  break;
                    case SAMPLE_PLAY_START: g_sampler.StartPlay(sc.rate); break;
                    case SAMPLE_PLAY_STOP:  g_sampler.StopPlay(); break;
                }
            }
            break;
        }
        case MSG_PREVIEW_REQ: {
            PreviewReqMessage pr{};
            if(ProtocolHandler::ParsePreviewReq(buffer, pr)) {
                g_prev_sent = 0;
                g_sampler.MakePreview(pr.start, pr.end, pr.decim ? pr.decim : 1, g_preview);
                // Kick first chunk immediately if possible
                if(!g_tx_ready && !g_preview.empty()) {
                    WaveChunkMessage hdr{0,0};
                    uint16_t max_samples = (uint16_t)((MAX_PAYLOAD_SIZE - sizeof(WaveChunkMessage)) / 2);
                    uint16_t count = g_preview.size() < max_samples ? (uint16_t)g_preview.size() : max_samples;
                    uint8_t payload[MAX_PAYLOAD_SIZE];
                    hdr.offset = 0; hdr.count = count;
                    memcpy(payload, &hdr, sizeof(hdr));
                    memcpy(payload + sizeof(hdr), &g_preview[0], count * sizeof(int16_t));
                    SendPacket(MSG_WAVE_CHUNK, payload, sizeof(hdr) + count * sizeof(int16_t));
                    g_prev_sent = count;
                }
            }
            break;
        }
        default:
            // Handle other message types as needed
            break;
    }
}

// Initialize SPI communication
void InitSPI()
{
    // Configure SPI for slave mode
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_SLAVE;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_HARD_INPUT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 10;
    
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        // Handle error
    }
    
    // Start receiving data
    HAL_SPI_Receive_IT(&hspi1, spi_rx_buffer, sizeof(spi_rx_buffer));
}

// Initialize DSP objects
void InitDSP()
{
    // Initialize filter
    filter.Init(hw.AudioSampleRate());
    filter.SetFreq(params.filter_cutoff);
    filter.SetRes(params.filter_resonance);
    filter.SetDrive(0.5f);
    
    // Initialize envelope
    envelope.Init(hw.AudioSampleRate());
    envelope.SetAttackTime(params.envelope_attack);
    envelope.SetDecayTime(params.envelope_decay);
    envelope.SetSustainLevel(params.envelope_sustain);
    envelope.SetReleaseTime(params.envelope_release);
    
    // Initialize LFO
    lfo.Init(hw.AudioSampleRate());
    lfo.SetWaveform(Oscillator::WAVE_SIN);
    lfo.SetFreq(params.lfo_rate);
    lfo.SetAmp(params.lfo_depth);
    
    // Initialize oscillator (for future use)
    oscillator.Init(hw.AudioSampleRate());
    oscillator.SetWaveform(Oscillator::WAVE_SIN);
    oscillator.SetFreq(440.0f);
    oscillator.SetAmp(0.5f);
}

int main(void)
{
    // Initialize Daisy Seed hardware
    hw.Configure();
    hw.Init();
    
    // Initialize audio
    hw.SetAudioBlockSize(Timebase::kBlockSize); // 48-sample blocks → 1 kHz control tick
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // Initialize DSP objects
    InitDSP();

    // Init sampler and CV bus
    g_sampler.Init(hw.AudioSampleRate());
    g_cv.Init(0x60);
    
    // Initialize SPI communication with ESP32
    InitSPI();
    IRQ_Init();
    
    // Start audio processing
    hw.StartAudio(AudioCallback);
    
    // Main loop
    while(1)
    {
        // Process incoming SPI messages
        if (spi_data_ready) {
            ProcessSPIMessage(spi_rx_buffer, sizeof(spi_rx_buffer));
            spi_data_ready = false;
            
            // Restart SPI reception
            HAL_SPI_Receive_IT(&hspi1, spi_rx_buffer, sizeof(spi_rx_buffer));
        }
        
        // Update modulation sources (LFO, envelope)
        // These are updated in the audio callback for real-time performance
        
        // Sleep for a bit to prevent busy waiting
        System::Delay(1);
    }
} 