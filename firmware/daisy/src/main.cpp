#include "daisy_seed.h"
#include "daisysp.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cstdio>
#include "spi_protocol/protocol.h"

#define INTER_MCU_UART_BAUD_RATE 460800 

// Enable to run a minimal audio path for hang isolation
// #define WAVEX_AUDIO_DEBUG_MINIMAL 1
// Set to 1 to completely disable UART init and polling for isolation testing
#define WAVEX_DEBUG_DISABLE_UART 0
// Set to 1 to enable UART RX via IRQ; leave 0 for polling-only mode
// NOTE: When IRQ mode is enabled, the main loop will NOT poll for UART data
//       Instead, bytes are received via libDaisy's interrupt callback system
//       When polling mode is enabled, uses libDaisy's UartHandler::PollReceive()
#define WAVEX_UART_RX_IRQ_MODE 1
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

// UART communication using libDaisy's UartHandler
static UartHandler uart;
static uint8_t uart_tx_buffer[256];

// Minimal RX ring buffer for ISR-safe byte capture
static volatile uint8_t  rx_ring[256];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;
static volatile uint32_t rx_total = 0;

// Forward declarations
void ProcessUARTMessage(const uint8_t* buffer, size_t length);
void InitUART(void);
void TestUARTLoopback(void);

// UART receive callback for interrupt-driven reception (DMA listen mode)
void OnUartDataReceived(uint8_t* data, size_t size, void* context, UartHandler::Result result);

// Data queue for outgoing messages (Daisy → ESP32)
struct QueuedMessage {
    uint8_t type;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint8_t length;
    bool valid;
};
static QueuedMessage g_message_queue[4]; // Small queue for pending messages
static volatile uint8_t g_queue_head = 0;
static volatile uint8_t g_queue_tail = 0;
static volatile bool g_has_pending_data = false;

// UART debugging counters
static volatile uint32_t g_uart_rx_count = 0;
static volatile uint32_t g_uart_rx_errors = 0;
static volatile uint32_t g_uart_tx_count = 0;
static volatile uint32_t g_uart_tx_errors = 0;
static volatile uint32_t g_last_uart_activity = 0;
static volatile uint32_t g_uart_rx_msgs = 0; // Decoded RX messages from ESP32

// Simple RX parser state for ProtocolHandler packets (header then payload)
enum class RxParseState : uint8_t { FindSync = 0, ReadHeader = 1, ReadPayload = 2 };
static RxParseState g_rx_state = RxParseState::FindSync;
static uint8_t g_rx_header[sizeof(PacketHeader)];
static uint8_t g_rx_packet[sizeof(PacketHeader) + MAX_PAYLOAD_SIZE];
static uint8_t g_rx_header_pos = 0;
static uint8_t g_rx_payload_pos = 0;
static uint8_t g_expected_payload_len = 0;

static inline bool rx_ring_pop(uint8_t& out)
{
    if(rx_head == rx_tail) return false;
    out = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1) & 0xFF);
    return true;
}

static void ProcessRxRing()
{
    uint8_t byte = 0;
    while(rx_head != rx_tail)
    {
        if(!rx_ring_pop(byte)) {
            break;
        }
        switch(g_rx_state)
        {
            case RxParseState::FindSync:
                if(byte == SYNC_BYTE)
                {
                    g_rx_header_pos = 0;
                    g_rx_payload_pos = 0;
                    g_expected_payload_len = 0;
                    g_rx_state = RxParseState::ReadHeader;
                    // First header byte is sync
                    g_rx_header[g_rx_header_pos++] = byte;
                }
                break;

            case RxParseState::ReadHeader:
                g_rx_header[g_rx_header_pos++] = byte;
                if(g_rx_header_pos >= sizeof(PacketHeader))
                {
                    // Copy header into packet buffer
                    memcpy(g_rx_packet, g_rx_header, sizeof(PacketHeader));
                    // Extract expected payload length (1 byte length field)
                    const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(g_rx_header);
                    g_expected_payload_len = hdr->length;
                    if(g_expected_payload_len > MAX_PAYLOAD_SIZE)
                    {
                        // Invalid length; reset state
                        g_rx_state = RxParseState::FindSync;
                        break;
                    }
                    if(g_expected_payload_len == 0)
                    {
                        // No payload; validate and process immediately
                        size_t total_len = sizeof(PacketHeader);
                        if(ProtocolHandler::ValidatePacket(g_rx_packet, total_len))
                        {
                            ProcessUARTMessage(g_rx_packet, total_len);
                        }
                        g_rx_state = RxParseState::FindSync;
                    }
                    else
                    {
                        g_rx_payload_pos = 0;
                        g_rx_state = RxParseState::ReadPayload;
                    }
                }
                break;

            case RxParseState::ReadPayload:
                g_rx_packet[sizeof(PacketHeader) + g_rx_payload_pos] = byte;
                if(++g_rx_payload_pos >= g_expected_payload_len)
                {
                    size_t total_len = sizeof(PacketHeader) + g_expected_payload_len;
                    if(ProtocolHandler::ValidatePacket(g_rx_packet, total_len))
                    {
                        ProcessUARTMessage(g_rx_packet, total_len);
                    }
                    g_rx_state = RxParseState::FindSync;
                }
                break;
        }
    }
}


static inline void UART_Send(const uint8_t* data, size_t length) {
    if (uart.BlockingTransmit(const_cast<uint8_t*>(data), length, 1000) == UartHandler::Result::OK) {
        g_has_pending_data = false;
        g_uart_tx_count++;
        g_last_uart_activity = System::GetNow();
        
        // Extract type, payload length from packet if possible
        if (length >= sizeof(PacketHeader) && data[0] == SYNC_BYTE) {
            const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(data);
            uint8_t msg_type = hdr->type;
            uint8_t payload_len = hdr->length;

            switch (msg_type) {
                case MSG_HEARTBEAT:
                    hw.PrintLine("UART TX: HEARTBEAT (%u payload + header + CRC = %u bytes), total=%lu",
                                payload_len, (unsigned)length, (unsigned long)g_uart_tx_count);
                    break;

                case MSG_METER_PUSH:
                case MSG_WAVE_CHUNK:
                    // Suppress frequent messages to avoid flooding
                    break;

                default:
                    hw.PrintLine("UART TX: type=0x%02X (%u payload + header + CRC = %u bytes), total=%lu",
                                msg_type, payload_len, (unsigned)length, (unsigned long)g_uart_tx_count);
                    break;
            }
        }
    } else {
        g_uart_tx_errors++;
        hw.PrintLine("UART TX failed! Total errors: %lu", g_uart_tx_errors);
    }
}

// Queue a message for transmission (Daisy → ESP32)
static void QueueMessage(uint8_t type, const void* payload, size_t length)
{
    if(length > MAX_PAYLOAD_SIZE) length = MAX_PAYLOAD_SIZE;
    
    // Check if queue has space
    uint8_t next_head = (g_queue_head + 1) % 4;
    if(next_head == g_queue_tail) {
        // Queue full, drop oldest message
        g_queue_tail = (g_queue_tail + 1) % 4;
    }
    
    // Add message to queue
    QueuedMessage& msg = g_message_queue[g_queue_head];
    msg.type = type;
    msg.length = (uint8_t)length;
    if(payload && length) {
        memcpy(msg.payload, payload, length);
    }
    msg.valid = true;
    g_queue_head = next_head;
    
    // Mark that we have data to send
    g_has_pending_data = true;
}

// Get next queued message for transmission
static bool GetNextQueuedMessage(QueuedMessage& msg)
{
    if(g_queue_head == g_queue_tail || !g_has_pending_data) {
        return false;
    }
    
    msg = g_message_queue[g_queue_tail];
    g_message_queue[g_queue_tail].valid = false;
    g_queue_tail = (g_queue_tail + 1) % 4;
    
    // If queue is empty, mark no pending data
    if(g_queue_head == g_queue_tail) {
        g_has_pending_data = false;
    }
    
    return true;
}

// Prepare response packet for ESP32 to read
void PrepareResponsePacket(const QueuedMessage& msg) {
    if (msg.valid) {
        // Create the response packet based on message type
        size_t packet_len = 0;
        
        switch (msg.type) {
            case MSG_METER_PUSH: {
                // Create meter push packet
                MeterPushMessage meter_msg;
                memcpy(&meter_msg, msg.payload, sizeof(meter_msg));
                packet_len = ProtocolHandler::CreateMeterPushPacket(
                    uart_tx_buffer, 
                    sizeof(uart_tx_buffer), 
                    meter_msg
                );
                break;
            }
            case MSG_WAVE_CHUNK: {
                // Create wave chunk packet
                WaveChunkMessage wave_msg;
                memcpy(&wave_msg, msg.payload, sizeof(wave_msg));
                packet_len = ProtocolHandler::CreateWaveChunkPacket(
                    uart_tx_buffer, 
                    sizeof(uart_tx_buffer), 
                    wave_msg,
                    msg.payload + sizeof(wave_msg),
                    msg.length - sizeof(wave_msg)
                );
                break;
            }
            default:
                // For other message types, create a generic packet
                packet_len = ProtocolHandler::CreateGenericPacket(
                    uart_tx_buffer, 
                    sizeof(uart_tx_buffer), 
                    msg.type, 
                    msg.payload, 
                    msg.length
                );
                break;
        }
        
        if (packet_len > 0) {
            // Packet is ready in uart_tx_buffer
            // Send it immediately via UART
            UART_Send(uart_tx_buffer, packet_len);
            // g_tx_buffer_valid = true;  // Mark buffer as containing valid data - REMOVED
        } else {
            // Failed to create packet - mark buffer as invalid
            // g_tx_buffer_valid = false; // REMOVED
        }
    } else {
        // Send empty packet
        memset(uart_tx_buffer, 0, sizeof(uart_tx_buffer));
    }
}

// Audio callback
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    #if WAVEX_AUDIO_DEBUG_MINIMAL
    for (size_t i = 0; i < size; i++) {
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }
    return;
    #endif
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
        // Temporarily disabled: blocking I2C in audio callback can stall audio; move to lower-priority context
        // g_cv.Flush();

        // Meter push every 20 ms
        static uint32_t meter_ctr = 0;
        if(++meter_ctr >= 20) {
            meter_ctr = 0;
            float rms=0, peak=0;
            Sampler::BlockMeters(g_last_in_block, g_last_block_size, rms, peak);
            MeterPushMessage m{rms, peak};
            // Queue the message instead of trying to send directly
            QueueMessage(MSG_METER_PUSH, &m, sizeof(m));
        }

        // Stream preview chunks if pending
        if(g_prev_sent < g_preview.size()) {
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
            // Queue the message instead of trying to send directly
            QueueMessage(MSG_WAVE_CHUNK, payload, sizeof(hdr) + count * sizeof(int16_t));
            g_prev_sent += count;
        }
    });
}

// Process incoming UART messages
void ProcessUARTMessage(const uint8_t* buffer, size_t length)
{
    if (!ProtocolHandler::ValidatePacket(buffer, length)) {
        hw.PrintLine("UART RX: invalid packet (len=%u)", (unsigned)length);
        return; // Invalid packet
    }
    
    MessageType msg_type = ProtocolHandler::GetMessageType(buffer);
    const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(buffer);
    const uint8_t payload_len = hdr ? hdr->length : 0;
    g_uart_rx_msgs++;
    // Generic per-packet summary before type-specific handling
    hw.PrintLine("UART RX: type=0x%02X len=%u (msg#=%lu)", (unsigned)msg_type, (unsigned)payload_len, (unsigned long)g_uart_rx_msgs);
    
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
                hw.PrintLine("RX CONTROL_CHANGE: param=0x%02X ch=%u val=%u", (unsigned)ctrl_msg.parameter, (unsigned)ctrl_msg.channel, (unsigned)ctrl_msg.value);
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
                hw.PrintLine("RX NOTE_ON: note=%u vel=%u ch=%u freq=%.2f", (unsigned)note_msg.note, (unsigned)note_msg.velocity, (unsigned)note_msg.channel, (double)freq);
            }
            break;
        }
        
        case MSG_NOTE_OFF: {
            NoteMessage note_msg;
            if (ProtocolHandler::ParseNoteMessage(buffer, note_msg)) {
                // Release envelope
                envelope_gate = false;
                hw.PrintLine("RX NOTE_OFF: note=%u ch=%u", (unsigned)note_msg.note, (unsigned)note_msg.channel);
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
                hw.PrintLine("RX SAMPLE_CTRL: slot=%u cmd=%u rate=%.3f", (unsigned)sc.slot, (unsigned)sc.cmd, (double)sc.rate);
            }
            break;
        }
        case MSG_PREVIEW_REQ: {
            PreviewReqMessage pr{};
            if(ProtocolHandler::ParsePreviewReq(buffer, pr)) {
                g_prev_sent = 0;
                g_sampler.MakePreview(pr.start, pr.end, pr.decim ? pr.decim : 1, g_preview);
                hw.PrintLine("RX PREVIEW_REQ: slot=%u start=%lu end=%lu decim=%u", (unsigned)pr.slot, (unsigned long)pr.start, (unsigned long)pr.end, (unsigned)pr.decim);
                // Queue first chunk immediately if possible
                if(!g_preview.empty()) {
                    WaveChunkMessage hdr{0,0};
                    uint16_t max_samples = (uint16_t)((MAX_PAYLOAD_SIZE - sizeof(WaveChunkMessage)) / 2);
                    uint16_t count = g_preview.size() < max_samples ? (uint16_t)g_preview.size() : max_samples;
                    uint8_t payload[MAX_PAYLOAD_SIZE];
                    hdr.offset = 0; hdr.count = count;
                    memcpy(payload, &hdr, sizeof(hdr));
                    memcpy(payload + sizeof(hdr), &g_preview[0], count * sizeof(int16_t));
                    QueueMessage(MSG_WAVE_CHUNK, payload, sizeof(hdr) + count * sizeof(int16_t));
                    g_prev_sent = count;
                }
            }
            break;
        }
        
        case MSG_DATA_REQUEST: {
            DataRequestMessage dr{};
            if(ProtocolHandler::ParseDataRequest(buffer, dr)) {
                hw.PrintLine("RX DATA_REQUEST: type=%u", (unsigned)dr.request_type);
                // ESP32 is requesting queued data - prepare response
                QueuedMessage msg;
                if(GetNextQueuedMessage(msg)) {
                    // We have data to send - prepare the response packet
                    PrepareResponsePacket(msg);
                    // The ESP32 will read this data on the next SPI transaction
                } else {
                    // No data to send - send minimal sync/heartbeat back
                    uint8_t payload[] = {0};
                    size_t packet_len = ProtocolHandler::CreateGenericPacket(
                        uart_tx_buffer,
                        sizeof(uart_tx_buffer),
                        MSG_SYNC,
                        payload,
                        0);
                    if(packet_len > 0) {
                        UART_Send(uart_tx_buffer, packet_len);
                    }
                }
            }
            break;
        }
        case MSG_SYNC: {
            hw.PrintLine("RX SYNC");
            break;
        }
        
        default:
            // Handle other message types as needed
            hw.PrintLine("RX UNKNOWN: type=0x%02X len=%u", (unsigned)msg_type, (unsigned)payload_len);
            break;
    }
}



// UART receive callback for interrupt-driven reception (DMA listen mode)
void OnUartDataReceived(uint8_t* data, size_t size, void* context, UartHandler::Result result)
{
    if (result == UartHandler::Result::OK) {
        // Process each received byte
        for (size_t i = 0; i < size; i++) {
            uint16_t next = (rx_head + 1) & 0xFF;
            if (next != rx_tail) {
                rx_ring[rx_head] = data[i];
                rx_head = next;
                rx_total++;
            }
        }
    }
}

// Initialize UART communication using libDaisy's UartHandler
void InitUART()
{
    // Configure UART using libDaisy's UartHandler
    UartHandler::Config cfg;
    cfg.periph = UartHandler::Config::Peripheral::USART_1;
    cfg.baudrate = INTER_MCU_UART_BAUD_RATE;
    cfg.stopbits = UartHandler::Config::StopBits::BITS_1;
    cfg.parity = UartHandler::Config::Parity::NONE;
    cfg.mode = UartHandler::Config::Mode::TX_RX;
    cfg.wordlength = UartHandler::Config::WordLength::BITS_8;
    cfg.pin_config.rx = Pin(PORTB, 7); // D14 = PB7 = RX
    cfg.pin_config.tx = Pin(PORTB, 6); // D13 = PB6 = TX
    
    if (uart.Init(cfg) != UartHandler::Result::OK) {
        hw.PrintLine("UartHandler init failed!");
        return;
    }
    hw.PrintLine("UART initialized: TX=D13 (PB6), RX=D14 (PB7), Baud=%lu", (unsigned long)cfg.baudrate);
    
    // Clear TX buffer
    memset(uart_tx_buffer, 0, sizeof(uart_tx_buffer));
    
    // Initialize TX buffer with a valid "no data" packet
    uint8_t no_data_payload[] = {0}; // Empty payload
    size_t packet_len = ProtocolHandler::CreateGenericPacket(
        uart_tx_buffer, 
        sizeof(uart_tx_buffer), 
        MSG_SYNC, 
        no_data_payload, 
        0
    );
    if (packet_len > 0) {
        hw.PrintLine("TX buffer initialized with sync packet");
    } else {
        hw.PrintLine("Failed to create TX buffer sync packet");
    }
    
    #if WAVEX_UART_RX_IRQ_MODE
    // Set up interrupt-driven reception using DMA listen mode
    
    // Create a buffer for DMA reception
    static uint8_t dma_rx_buffer[256];
    
    // Start DMA listening mode with callback
    if (uart.DmaListenStart(dma_rx_buffer, sizeof(dma_rx_buffer), OnUartDataReceived, nullptr) == UartHandler::Result::OK) {
        hw.PrintLine("UART DMA listen mode started");
    } else {
        hw.PrintLine("UART DMA listen mode failed to start");
    }
    #else
    hw.PrintLine("UART polling mode enabled");
    #endif
}



// UART interrupt handling is now managed by libDaisy's UartHandler
// No custom interrupt handlers needed - the callback system handles everything

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
    static bool sync_sent = false;
    // IMMEDIATE DEBUG - This should appear first
    // Note: We can't use hw.PrintLine yet as hardware isn't initialized
    // Initialize Daisy Seed hardware
    hw.Configure();

    hw.Init();

    
    // Initialize USB CDC for debugging IMMEDIATELY after hw.Init() to capture ALL output
    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);
    
    // Start USB CDC interface - this makes it appear as a serial device
    hw.StartLog(true);
    hw.PrintLine("Hardware initialized successfully");
    hw.PrintLine("Revision: 0x%lX", DBGMCU->IDCODE);
    
    // Initialize audio
    hw.SetAudioBlockSize(Timebase::kBlockSize); // 48-sample blocks → 1 kHz control tick
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // Initialize DSP objects
    InitDSP();
    hw.PrintLine("DSP objects initialized");

    // Init sampler and CV bus
    g_sampler.Init(hw.AudioSampleRate());
    g_cv.Init(0x60);
    hw.PrintLine("Sampler and CV bus initialized");
    
    // Initialize UART communication with ESP32 (disabled for isolation if macro set)
    #if !WAVEX_DEBUG_DISABLE_UART
    InitUART();
    System::Delay(100);
    #else
    hw.PrintLine("UART init disabled (WAVEX_DEBUG_DISABLE_UART)");
    #endif
    
    // Start audio processing (disabled temporarily to isolate interrupt/timing)
    hw.StartAudio(AudioCallback);
    
    // Startup message
    hw.PrintLine("WaveX Daisy firmware starting");
    hw.PrintLine("USART1: TX=D13 (PB6), RX=D14 (PB7), Baud=%d", INTER_MCU_UART_BAUD_RATE);
    #if WAVEX_UART_RX_IRQ_MODE
    hw.PrintLine("UART RX Mode: Interrupt-driven");
    #else
    hw.PrintLine("UART RX Mode: Polling");
    #endif
    hw.PrintLine("Ready for inter-MCU communication");

    // Periodic liveness beacon: respond proactively every ~1s with basic health
    uint32_t last_beacon = System::GetNow();
    uint32_t last_tx_pump = System::GetNow();
    
    // Main loop starting

    // Main loop
    while(1)
    {
        static uint32_t loop_counter = 0;
        loop_counter++;
        

        
        // Handle UART reception based on mode
        #if !WAVEX_DEBUG_DISABLE_UART
        #if WAVEX_UART_RX_IRQ_MODE
        // In IRQ mode, just process any bytes that have been received via interrupts
        // The interrupt handler populates the ring buffer automatically
        // No polling needed - interrupts handle all UART reception

        #else
        // Polling mode: use libDaisy's UartHandler to poll for received bytes
        uint8_t ch;
        size_t bytes_received = 0;
        
        // Poll for any available bytes using libDaisy's method
        while (uart.BlockingReceive(&ch, 1, 1) == UartHandler::Result::OK) {
            uint16_t next = (rx_head + 1) & 0xFF;
            if (next != rx_tail) {
                rx_ring[rx_head] = ch;
                rx_head = next;
                rx_total++;
                bytes_received++;
            } else {
                break; // Ring buffer full
            }
        }
        

        #endif

        // Parse any bytes accumulated in the RX ring into protocol messages
        ProcessRxRing();

        // Proactively send one queued message every few ms (push model)
        if(g_has_pending_data && (System::GetNow() - last_tx_pump) >= 5) {
            last_tx_pump = System::GetNow();
            QueuedMessage msg;
            if(GetNextQueuedMessage(msg)) {
                PrepareResponsePacket(msg);
            }
        }

        // Debug output moved to the conditional blocks above
        #endif

        // Send SYNC packet every 2 seconds
        if(System::GetNow() > 2000 && !sync_sent) {
            uint8_t p[] = {0};
            size_t len = ProtocolHandler::CreateGenericPacket(uart_tx_buffer, sizeof(uart_tx_buffer), MSG_SYNC, p, 0);
            if(len > 0) UART_Send(uart_tx_buffer, len);
            sync_sent = true;
            hw.PrintLine("Manually sent SYNC packet");
        }

        // Periodic liveness beacon (approx 1s)
        #if !WAVEX_DEBUG_DISABLE_UART
        if(System::GetNow() - last_beacon >= 1000) {
            last_beacon = System::GetNow();
            HeartbeatMessage hb{};
            hb.uptime_ms = last_beacon;
            hb.rx_total = rx_total;
            hb.loop_counter = loop_counter;
            size_t packet_len = ProtocolHandler::CreateHeartbeatPacket(
                uart_tx_buffer,
                sizeof(uart_tx_buffer),
                hb);
            if(packet_len > 0) {
                UART_Send(uart_tx_buffer, packet_len);
                hw.PrintLine("Heartbeat sent: uptime=%lu rx_total=%lu", (unsigned long)hb.uptime_ms, (unsigned long)hb.rx_total);
            }
        }
        #endif

        // 5ms delay
        System::Delay(5);

    }
}

// NOTE: UART interrupt handling is now properly managed by libDaisy's UartHandler
// No custom interrupt handlers needed - the callback system handles everything safely