#include "daisy_seed.h"
#include "daisysp.h"
#include "spi_protocol/protocol.h"

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

// Envelope gate state
bool envelope_gate = false;

// SPI communication
SPI_HandleTypeDef hspi1;
uint8_t spi_rx_buffer[128];
uint8_t spi_tx_buffer[128];
volatile bool spi_data_ready = false;

// Audio callback
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    for (size_t i = 0; i < size; i++)
    {
        // Update LFO
        float lfo_value = lfo.Process();
        
        // Process envelope with gate
        float env_value = envelope.Process(envelope_gate);
        
        // Apply modulation to filter cutoff
        float modulated_cutoff = params.filter_cutoff + (lfo_value * params.lfo_depth * 1000.0f);
        filter.SetFreq(modulated_cutoff);
        
        // Process input through filter
        float input_sample = in[0][i] * params.volume;
        filter.Process(input_sample);
        float filtered_sample = filter.Low();
        
        // Apply envelope
        float output_sample = filtered_sample * env_value;
        
        // Output processed audio
        out[0][i] = output_sample;
        out[1][i] = output_sample; // Mono for now
    }
}

// SPI callback for receiving data from ESP32
void WaveX_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        spi_data_ready = true;
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
    hw.SetAudioBlockSize(64); // 64-sample blocks for low latency
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // Initialize DSP objects
    InitDSP();
    
    // Initialize SPI communication with ESP32
    InitSPI();
    
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