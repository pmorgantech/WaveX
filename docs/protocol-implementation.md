# WaveX Protocol Implementation Guide

## Implementation Overview

This document provides practical implementation details for the WaveX inter-MCU communication protocol, including code examples, initialization procedures, and best practices.

## ESP32 Frontend Implementation

### SPI Master Configuration

```c
// spi_master_config.h
#define SPI_MASTER_HOST     SPI2_HOST
#define SPI_MASTER_FREQ_HZ  10000000    // 10 MHz
#define PIN_NUM_MISO        12
#define PIN_NUM_MOSI        13
#define PIN_NUM_CLK         14
#define PIN_NUM_CS          15
#define PIN_NUM_IRQ         16

// Protocol constants
#define PROTOCOL_SYNC_BYTE  0xAA
#define MAX_PAYLOAD_SIZE    64
#define SPI_TIMEOUT_MS      100
```

### SPI Master Initialization

```c
// spi_master.c
#include "driver/spi_master.h"
#include "driver/gpio.h"

static spi_device_handle_t spi_device;
static QueueHandle_t tx_queue;
static QueueHandle_t rx_queue;

esp_err_t spi_master_init(void) {
    esp_err_t ret;
    
    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_PAYLOAD_SIZE + 4  // Header + payload
    };
    
    ret = spi_bus_initialize(SPI_MASTER_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;
    
    // Configure SPI device
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_MASTER_FREQ_HZ,
        .mode = 0,                          // SPI mode 0
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX
    };
    
    ret = spi_bus_add_device(SPI_MASTER_HOST, &devcfg, &spi_device);
    if (ret != ESP_OK) return ret;
    
    // Configure IRQ pin
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_IRQ),
        .pull_down_en = 1,
    };
    gpio_config(&io_conf);
    
    // Create message queues
    tx_queue = xQueueCreate(32, sizeof(protocol_packet_t));
    rx_queue = xQueueCreate(32, sizeof(protocol_packet_t));
    
    return ESP_OK;
}
```

### Protocol Message Handling

```c
// protocol_handler.c
typedef struct {
    uint8_t sync;
    uint8_t type;
    uint8_t length;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint8_t crc;
} protocol_packet_t;

// Calculate simple checksum
static uint8_t calculate_crc(const protocol_packet_t* packet) {
    uint8_t crc = 0;
    crc ^= packet->sync;
    crc ^= packet->type;
    crc ^= packet->length;
    for (int i = 0; i < packet->length; i++) {
        crc ^= packet->payload[i];
    }
    return crc;
}

// Send a protocol message
esp_err_t protocol_send_message(uint8_t type, const void* payload, size_t length) {
    if (length > MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    protocol_packet_t packet = {
        .sync = PROTOCOL_SYNC_BYTE,
        .type = type,
        .length = length
    };
    
    if (payload && length > 0) {
        memcpy(packet.payload, payload, length);
    }
    
    packet.crc = calculate_crc(&packet);
    
    // Queue message for transmission
    if (xQueueSend(tx_queue, &packet, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

// Control change message
esp_err_t protocol_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    struct {
        uint8_t parameter;
        uint8_t channel;
        uint16_t value;
    } msg = { parameter, channel, value };
    
    return protocol_send_message(MSG_CONTROL_CHANGE, &msg, sizeof(msg));
}

// Note on message
esp_err_t protocol_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    struct {
        uint8_t channel;
        uint8_t note;
        uint8_t velocity;
        uint8_t voice_id;
    } msg = { channel, note, velocity, 0 };  // voice_id assigned by backend
    
    return protocol_send_message(MSG_NOTE_ON, &msg, sizeof(msg));
}
```

### SPI Communication Task

```c
// spi_task.c
static void spi_communication_task(void* pvParameters) {
    protocol_packet_t tx_packet, rx_packet;
    spi_transaction_t trans;
    
    while (1) {
        // Check for outgoing messages
        if (xQueueReceive(tx_queue, &tx_packet, pdMS_TO_TICKS(10)) == pdTRUE) {
            memset(&trans, 0, sizeof(trans));
            trans.length = (3 + tx_packet.length + 1) * 8;  // bits
            trans.tx_buffer = &tx_packet;
            trans.rx_buffer = &rx_packet;
            
            esp_err_t ret = spi_device_transmit(spi_device, &trans);
            if (ret == ESP_OK) {
                // Check if we received a response
                if (rx_packet.sync == PROTOCOL_SYNC_BYTE) {
                    if (calculate_crc(&rx_packet) == rx_packet.crc) {
                        xQueueSend(rx_queue, &rx_packet, 0);
                    }
                }
            }
        }
        
        // Send heartbeat if no activity
        static uint32_t last_heartbeat = 0;
        uint32_t now = xTaskGetTickCount();
        if ((now - last_heartbeat) > pdMS_TO_TICKS(100)) {
            protocol_send_message(MSG_SYNC, NULL, 0);
            last_heartbeat = now;
        }
    }
}
```

## STM32 Backend Implementation

### SPI Slave Configuration

```c
// spi_slave_config.h (STM32/Daisy)
#define SPI_INSTANCE        SPI1
#define SPI_IRQ_PRIORITY    5
#define SPI_DMA_PRIORITY    4

// GPIO Configuration
#define SPI_SCK_PIN         GPIO_PIN_5
#define SPI_SCK_PORT        GPIOA
#define SPI_MISO_PIN        GPIO_PIN_6
#define SPI_MISO_PORT       GPIOA
#define SPI_MOSI_PIN        GPIO_PIN_7
#define SPI_MOSI_PORT       GPIOA
#define SPI_NSS_PIN         GPIO_PIN_4
#define SPI_NSS_PORT        GPIOA
#define IRQ_PIN             GPIO_PIN_0
#define IRQ_PORT            GPIOB
```

### SPI Slave Initialization

```c
// spi_slave.c (STM32/Daisy)
#include "stm32h7xx_hal.h"

static SPI_HandleTypeDef hspi1;
static DMA_HandleTypeDef hdma_spi1_tx;
static DMA_HandleTypeDef hdma_spi1_rx;

// Circular buffers for message handling
static protocol_packet_t rx_buffer[16];
static protocol_packet_t tx_buffer[16];
static volatile uint32_t rx_head = 0, rx_tail = 0;
static volatile uint32_t tx_head = 0, tx_tail = 0;

HAL_StatusTypeDef spi_slave_init(void) {
    // Enable clocks
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    
    // Configure GPIO pins
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    // SPI pins
    GPIO_InitStruct.Pin = SPI_SCK_PIN | SPI_MISO_PIN | SPI_MOSI_PIN | SPI_NSS_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    
    // IRQ output pin
    GPIO_InitStruct.Pin = IRQ_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(IRQ_PORT, &GPIO_InitStruct);
    
    // Configure SPI
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_SLAVE;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_HARD_INPUT;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    
    return HAL_SPI_Init(&hspi1);
}
```

### Message Processing

```c
// protocol_processor.c (STM32/Daisy)
#include "audio_engine.h"

void process_protocol_message(const protocol_packet_t* packet) {
    switch (packet->type) {
        case MSG_CONTROL_CHANGE: {
            const struct {
                uint8_t parameter;
                uint8_t channel;
                uint16_t value;
            }* msg = (const void*)packet->payload;
            
            audio_engine_set_parameter(msg->parameter, msg->channel, msg->value);
            
            // Send confirmation
            protocol_send_parameter_update(msg->parameter, msg->channel, msg->value);
            break;
        }
        
        case MSG_NOTE_ON: {
            const struct {
                uint8_t channel;
                uint8_t note;
                uint8_t velocity;
                uint8_t voice_id;
            }* msg = (const void*)packet->payload;
            
            uint8_t voice = audio_engine_note_on(msg->channel, msg->note, msg->velocity);
            
            // Send back assigned voice ID
            struct {
                uint8_t channel;
                uint8_t note;
                uint8_t velocity;
                uint8_t voice_id;
            } response = *msg;
            response.voice_id = voice;
            
            protocol_send_message(MSG_NOTE_ON, &response, sizeof(response));
            break;
        }
        
        case MSG_NOTE_OFF: {
            const struct {
                uint8_t channel;
                uint8_t note;
                uint8_t velocity;
                uint8_t voice_id;
            }* msg = (const void*)packet->payload;
            
            audio_engine_note_off(msg->voice_id);
            break;
        }
        
        case MSG_SAMPLE_LOAD: {
            const struct {
                uint32_t sample_id;
                uint32_t size;
                uint16_t sample_rate;
                uint8_t channels;
                uint8_t bit_depth;
            }* msg = (const void*)packet->payload;
            
            if (audio_engine_prepare_sample_load(msg->sample_id, msg->size, 
                                                msg->sample_rate, msg->channels, 
                                                msg->bit_depth)) {
                // Send ACK
                protocol_send_message(MSG_SYNC, NULL, 0);
            } else {
                // Send error
                struct {
                    uint8_t error_code;
                    uint8_t source;
                    uint16_t context;
                } error = { 0x04, 0x01, msg->sample_id };
                protocol_send_message(MSG_ERROR, &error, sizeof(error));
            }
            break;
        }
        
        case MSG_SAMPLE_DATA: {
            const struct {
                uint32_t sample_id;
                uint32_t offset;
                uint16_t chunk_size;
                uint8_t data[];
            }* msg = (const void*)packet->payload;
            
            if (audio_engine_write_sample_data(msg->sample_id, msg->offset, 
                                              msg->data, msg->chunk_size)) {
                // Send ACK
                protocol_send_message(MSG_SYNC, NULL, 0);
            }
            break;
        }
        
        case MSG_SYNC:
            // Heartbeat received - update watchdog
            protocol_update_watchdog();
            break;
            
        default:
            // Unknown message type
            struct {
                uint8_t error_code;
                uint8_t source;
                uint16_t context;
            } error = { 0x01, 0x02, packet->type };
            protocol_send_message(MSG_ERROR, &error, sizeof(error));
            break;
    }
}
```

### Audio Engine Integration

```c
// audio_engine.c (STM32/Daisy)
#include "daisy_seed.h"
#include "daisysp.h"

#define MAX_VOICES      16
#define SAMPLE_RATE     48000
#define BLOCK_SIZE      64

typedef struct {
    bool active;
    uint8_t note;
    uint8_t velocity;
    float frequency;
    daisysp::Adsr envelope;
    // Add more voice parameters
} voice_t;

static voice_t voices[MAX_VOICES];
static float parameters[256];  // Parameter storage
static daisy::DaisySeed hw;

void audio_engine_init(void) {
    hw.Configure();
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // Initialize voices
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].active = false;
        voices[i].envelope.Init(SAMPLE_RATE);
    }
    
    hw.StartAudio(audio_callback);
}

uint8_t audio_engine_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Find free voice
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) {
            voices[i].active = true;
            voices[i].note = note;
            voices[i].velocity = velocity;
            voices[i].frequency = mtof(note);
            voices[i].envelope.Retrigger(false);
            return i;
        }
    }
    return 0xFF;  // No free voices
}

void audio_engine_note_off(uint8_t voice_id) {
    if (voice_id < MAX_VOICES) {
        voices[voice_id].envelope.Retrigger(true);
    }
}

void audio_engine_set_parameter(uint8_t param_id, uint8_t channel, uint16_t value) {
    float normalized_value = value / 65535.0f;
    parameters[param_id] = normalized_value;
    
    // Apply parameter changes immediately
    switch (param_id) {
        case PARAM_ENVELOPE_ATTACK:
            for (int i = 0; i < MAX_VOICES; i++) {
                voices[i].envelope.SetTime(daisysp::ADSR_SEG_ATTACK, normalized_value * 2.0f);
            }
            break;
        case PARAM_ENVELOPE_DECAY:
            for (int i = 0; i < MAX_VOICES; i++) {
                voices[i].envelope.SetTime(daisysp::ADSR_SEG_DECAY, normalized_value * 2.0f);
            }
            break;
        // Add more parameter handling
    }
}

void audio_callback(daisy::AudioHandle::InputBuffer in, 
                   daisy::AudioHandle::OutputBuffer out, 
                   size_t size) {
    // Process protocol messages (non-blocking)
    process_pending_messages();
    
    for (size_t i = 0; i < size; i++) {
        float left = 0.0f, right = 0.0f;
        
        // Process all active voices
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].active) {
                float env = voices[v].envelope.Process();
                if (env <= 0.0f && voices[v].envelope.IsRunning() == false) {
                    voices[v].active = false;
                    continue;
                }
                
                // Generate audio (simplified oscillator)
                float sample = sinf(voices[v].frequency * 2.0f * M_PI * i / SAMPLE_RATE) * env * 0.1f;
                left += sample;
                right += sample;
            }
        }
        
        out[0][i] = left;
        out[1][i] = right;
    }
}
```

## Performance Optimization

### Message Batching

```c
// message_batcher.c
typedef struct {
    protocol_packet_t packets[8];
    uint8_t count;
    uint32_t timestamp;
} message_batch_t;

static message_batch_t current_batch = {0};

void batch_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    if (current_batch.count >= 8 || 
        (xTaskGetTickCount() - current_batch.timestamp) > pdMS_TO_TICKS(5)) {
        // Send current batch and start new one
        flush_message_batch();
    }
    
    // Add to current batch
    protocol_packet_t* packet = &current_batch.packets[current_batch.count++];
    packet->sync = PROTOCOL_SYNC_BYTE;
    packet->type = MSG_CONTROL_CHANGE;
    packet->length = 4;
    
    struct {
        uint8_t parameter;
        uint8_t channel;
        uint16_t value;
    }* msg = (void*)packet->payload;
    
    msg->parameter = parameter;
    msg->channel = channel;
    msg->value = value;
    
    packet->crc = calculate_crc(packet);
    
    if (current_batch.count == 1) {
        current_batch.timestamp = xTaskGetTickCount();
    }
}
```

### DMA-based Transfer

```c
// dma_spi.c (STM32)
static uint8_t dma_tx_buffer[128];
static uint8_t dma_rx_buffer[128];

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) {
    if (hspi->Instance == SPI1) {
        // Process received data
        protocol_packet_t* rx_packet = (protocol_packet_t*)dma_rx_buffer;
        if (rx_packet->sync == PROTOCOL_SYNC_BYTE) {
            uint32_t next_head = (rx_head + 1) % 16;
            if (next_head != rx_tail) {
                memcpy(&rx_buffer[rx_head], rx_packet, sizeof(protocol_packet_t));
                rx_head = next_head;
            }
        }
        
        // Prepare next transmission
        setup_next_spi_transfer();
    }
}
```

## Debug and Testing

### Protocol Analyzer

```c
// protocol_debug.c
#ifdef DEBUG_PROTOCOL
static void log_protocol_message(const protocol_packet_t* packet, bool tx) {
    printf("%s: Type=0x%02X, Len=%d, CRC=0x%02X\n", 
           tx ? "TX" : "RX", packet->type, packet->length, packet->crc);
    
    if (packet->length > 0) {
        printf("Payload: ");
        for (int i = 0; i < packet->length; i++) {
            printf("%02X ", packet->payload[i]);
        }
        printf("\n");
    }
}
#endif
```

### Performance Monitoring

```c
// performance_monitor.c
typedef struct {
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t errors;
    uint32_t max_latency_us;
    uint32_t avg_latency_us;
} protocol_stats_t;

static protocol_stats_t stats = {0};

void update_latency_stats(uint32_t latency_us) {
    if (latency_us > stats.max_latency_us) {
        stats.max_latency_us = latency_us;
    }
    
    // Simple moving average
    stats.avg_latency_us = (stats.avg_latency_us * 7 + latency_us) / 8;
}
```

This implementation provides a solid foundation for the WaveX inter-MCU communication protocol, with emphasis on real-time performance, reliability, and ease of debugging. 