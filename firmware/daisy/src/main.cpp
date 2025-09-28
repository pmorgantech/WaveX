#include "daisy_seed.h"
#include "daisysp.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cmath>
#include "stm32h7xx_hal.h"
#include "../shared/spi_protocol/spi_protocol.h"
#include "../shared/config/pin_config.h"
#include "config.hpp"
#include "comm/daisy_spi_link.h"
#include "config/link_config.h"
#include "metrics/metrics.h"
#include "ff.h"
#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 0)
#include "storage/sd_spi.h"
#endif
#include "per/gpio.h"

// Feature macros moved to config.hpp
#include "timebase.hpp"
#include "audio/audio_engine.h"

using namespace daisy;
using namespace daisysp;
using namespace WaveX::Protocol;

// Hardware
DaisySeed hw;
static FATFS s_fs; // FatFs object
static daisy::SpiHandle spi_handle;

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
#include "storage/sd_sdio.h"
#endif

// Helper function to process SPI packets - NO LONGER NEEDED
/*
void process_spi_packet(::pkt_t* pkt) {
    // Process incoming SPI messages from ESP32 using the proper message processor
    if (pkt && pkt->h.len <= 26) {
        // Call the actual message processing function from daisy_spi_link.cpp
        // pkt_t has payload directly, not pkt->h.payload
        WaveX::Comm::ProcessEsp32Message(pkt->h.type, 0, pkt->payload, pkt->h.len);
    }
}
*/

// Process incoming SPI messages from ESP32
void process_incoming_spi_messages() {
    #if WAVEX_SPI_LINK_ENABLED
    // The new, correct approach is to call a function that handles polling,
    // dequeuing, and processing in one step, avoiding the legacy conversion.
    WaveX::Comm::ProcessQueuedSpiMessage();
    #endif
}

// Optional loop/CPU probe GPIO
// CPU usage measurement state - improved accuracy
static uint32_t s_cpu_window_start_ticks = 0;
static uint32_t s_cpu_busy_ticks_accum = 0;
static uint32_t s_cpu_total_ticks_accum = 0;
static uint32_t s_cpu_last_log_ms = 0;
static uint32_t s_cpu_baseline_ticks_per_second = 0;
static bool s_cpu_baseline_measured = false;
static uint32_t s_cpu_measurement_count = 0;
static float s_cpu_usage_percent = 0.0f; // Current CPU usage percentage

// QueuedMessage removed - using SPI only

/**
 * @brief Measure CPU baseline to determine maximum possible CPU usage
 * This runs a tight loop for 1 second to measure maximum ticks per second
 */
static void measure_cpu_baseline()
{
    if (s_cpu_baseline_measured) return;
    
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "CPU: Measuring baseline performance...");
    
    uint32_t start_time = System::GetNow();
    uint32_t start_ticks = System::GetTick();
    uint32_t end_time = start_time + 100; // Run for 100ms (much shorter)
    
    // Tight loop to measure maximum CPU capability
    volatile uint32_t counter = 0;
    while (System::GetNow() < end_time) {
        counter++;
        // Prevent compiler optimization
        __asm__ __volatile__("" : "+r" (counter));
    }
    
    uint32_t end_ticks = System::GetTick();
    uint32_t elapsed_ticks = end_ticks - start_ticks;
    s_cpu_baseline_ticks_per_second = elapsed_ticks * 10; // Scale up from 100ms to 1 second
    s_cpu_baseline_measured = true;
    
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "CPU: Baseline = %lu ticks/second (counter=%lu)", 
                    (unsigned long)s_cpu_baseline_ticks_per_second, (unsigned long)counter);
}

// Initialize DSP objects via AudioEngine
void InitDSP()
{
    WaveX::AudioEngine::Init(hw, hw.AudioSampleRate());
}

int main(void)
{
    // Initialize Daisy Seed hardware
    hw.Configure();
    hw.Init();

    // Initialize USB CDC for debugging
    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);
    
    // Start USB CDC interface - this makes it appear as a serial device
    hw.StartLog(true);
    
    // Add delay to ensure USB CDC is ready before logging
    System::Delay(100);
    
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== WAVEX DAISY BOOT START ===");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Hardware initialized successfully");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Revision: 0x%lX", 0x20036450); // STM32H7B3
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "USB CDC logging active");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI Link Enabled: %d", WAVEX_SPI_LINK_ENABLED);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI DMA Enabled: %d", WAVEX_SPI_DMA_ENABLED);
    
    // Initialize CPU usage measurement window
    s_cpu_window_start_ticks = System::GetTick();
    s_cpu_last_log_ms = System::GetNow();
    s_cpu_baseline_measured = false;
    s_cpu_measurement_count = 0;
    
    // Measure CPU baseline early to avoid interfering with audio
    measure_cpu_baseline();
    
    // Initialize SD card (SDMMC + FatFS) if enabled
    bool sd_available = false;
    #if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: Starting SD card initialization...");
    uint32_t sd_start_time = System::GetNow();
    
    #if WAVEX_DAISY_SD_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: InitAndMount start");
    #endif
    sd_available = WaveX::Storage::SdSdio::InitAndMount(hw, true);
    
    uint32_t sd_init_time = System::GetNow() - sd_start_time;
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: Initialization completed in %lu ms", (unsigned long)sd_init_time);
    if(sd_available)
    {
        #if WAVEX_DAISY_SD_DEBUG
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: InitAndMount SUCCESS");
        #endif
        // Give SD card time to stabilize after mount
        System::Delay(200);
        #if WAVEX_DAISY_SD_DEBUG
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: post-mount delay complete");
        #endif
    }
    else
    {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: InitAndMount FAILED - no card or mount error");
    }
    #endif

    // Initialize audio (if enabled)
    
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.SetAudioBlockSize(Timebase::kBlockSize); // 48-sample blocks → 1 kHz control tick
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_44KHZ);
    
    // Initialize DSP objects
    InitDSP();
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "DSP objects initialized");
    #else
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio engine disabled (WAVEX_AUDIO_ENGINE_ENABLED = 0)");
    #endif

    // AudioEngine already initializes sampler and CV bus internally
    
    // Configure interrupt priorities to prevent audio starvation
    // Audio DMA should have higher priority (lower number) than SPI
    #if WAVEX_AUDIO_ENGINE_ENABLED
    // Set audio DMA to highest priority
    HAL_NVIC_SetPriority(SAI1_IRQn, 5, 0);
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 5, 0);  // SAI1 DMA A
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);  // SAI1 DMA B
    HAL_NVIC_SetPriority(SAI2_IRQn, 6, 0);
    HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 6, 0);  // SAI2 DMA A
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 6, 0);  // SAI2 DMA B
    
    // Set SPI to lower priority
    HAL_NVIC_SetPriority(SPI1_IRQn, 12, 0);
    HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 10, 0); // SPI1 DMA RX
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 10, 0); // SPI1 DMA TX
    
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Interrupt priorities configured: Audio DMA=5/6, SPI DMA=10");
    #endif
    
    // Initialize communication with ESP32
    #if WAVEX_SPI_LINK_ENABLED
    // Add debug to confirm SPI init is reached
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Entering SPI init in main.");
    #endif
    
    // Ensure SPI peripheral system is initialized
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "About to call dsy_spi_global_init");
    #endif
    dsy_spi_global_init();
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "dsy_spi_global_init completed");
    #endif
    
    // Use libDaisy SPI1 master path (Daisy is MASTER, ESP32 is SLAVE)
    daisy::SpiHandle::Config spi_conf;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Created SPI config object");
    #endif
    
    // Use configuration macros from pin_config.h and link_config.h
    spi_conf.periph = (daisy::SpiHandle::Config::Peripheral::SPI_1);  // WAVEX_DAISY_SPI_PERIPH
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI peripheral to SPI_1");
    #endif
    
    spi_conf.mode = daisy::SpiHandle::Config::Mode::MASTER;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI mode to MASTER");
    #endif
    
    spi_conf.direction = daisy::SpiHandle::Config::Direction::TWO_LINES;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI direction to TWO_LINES");
    #endif
    
    spi_conf.datasize = 8;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI datasize to 8");
    #endif
    
    spi_conf.clock_polarity = daisy::SpiHandle::Config::ClockPolarity::LOW;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI clock polarity to LOW");
    #endif
    
    spi_conf.clock_phase = daisy::SpiHandle::Config::ClockPhase::ONE_EDGE;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI clock phase to ONE_EDGE");
    #endif
    
    spi_conf.nss = daisy::SpiHandle::Config::NSS::SOFT;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI NSS to SOFT");
    #endif
    
    spi_conf.baud_prescaler = daisy::SpiHandle::Config::BaudPrescaler::PS_8; // Unused in slave mode
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI baud prescaler to PS_8");
    #endif
    
    spi_conf.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI SCLK pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_SCK);
    #endif
    
    spi_conf.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI MOSI pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_MOSI);
    #endif
    
    spi_conf.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI MISO pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_MISO);
    #endif
    
    spi_conf.pin_config.nss = hw.GetPin(WAVEX_DAISY_SPI_CS);
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI NSS pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_CS);
    #endif
    
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "About to call spi_handle.Init");
    #endif
    
    // Debug: Print SPI configuration values
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI Config Debug:");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  periph: %d", (int)spi_conf.periph);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  mode: %d", (int)spi_conf.mode);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  direction: %d", (int)spi_conf.direction);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  datasize: %lu", (unsigned long)spi_conf.datasize);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  clock_polarity: %d", (int)spi_conf.clock_polarity);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  clock_phase: %d", (int)spi_conf.clock_phase);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  nss: %d", (int)spi_conf.nss);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  baud_prescaler: %d", (int)spi_conf.baud_prescaler);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  pin_config.sclk: configured");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  pin_config.mosi: configured");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  pin_config.miso: configured");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "  pin_config.nss: configured");
    #endif
    
    // Add error handling and timeout for SPI init
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: About to call spi_handle.Init...");
    daisy::SpiHandle::Result init_result = spi_handle.Init(spi_conf);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: spi_handle.Init returned: %d", (int)init_result);
    if (init_result != daisy::SpiHandle::Result::OK) {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: SPI init FAILED with result: %d", (int)init_result);
        // Continue without SPI for now
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: SPI init SUCCESS");
    }
    
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Exited spi_handle.Init (before Spi_Init)");
    #endif

    // Initialize SPI link only if SPI init succeeded
    if (init_result == daisy::SpiHandle::Result::OK) {
        hw.PrintLine("DAISY: SPI Init SUCCESS - About to call WaveX::Comm::Spi_Init");
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Calling WaveX::Comm::Spi_Init");
        WaveX::Comm::Spi_Init(hw, &spi_handle);
        hw.PrintLine("DAISY: WaveX::Comm::Spi_Init completed");
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Returned from Spi_Init");
        System::Delay(100);
        
        // SPI test to verify communication works
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Testing SPI with immediate test packet");
    WaveX::Protocol::SyncMessage sync_msg = {System::GetTick(), {0xAA, 0xBB, 0xCC, 0xDD}};
    uint8_t test_buffer[64];
    size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateSyncPacket(test_buffer, sizeof(test_buffer), sync_msg);
    int test_result = WaveX::Comm::Spi_SendPreCreatedPacket(test_buffer, pkt_size);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: SPI test result: %d (1=success, 0=fail)", test_result);
    if (test_result == 1) {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SUCCESS: SPI is working! You should see SPI1 CLK on scope now!");
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "FAILURE: SPI test failed - this explains the missing SPI1 CLK!");
    }
    WaveX::Comm::Spi_DebugState();
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "ERROR: Skipping SPI link init due to SPI init failure - NO SPI CLOCK WILL BE GENERATED");
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "ERROR: This explains why you see no SPI1 CLK on the scope!");
    }
    #endif

    // Start audio callback system
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.StartAudio(WaveX::AudioEngine::Callback);
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio engine started - callback system active");
    #else
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio engine disabled (WAVEX_AUDIO_ENGINE_ENABLED = 0)");
    #endif
    


    // Main loop
    // Periodic liveness beacon: respond proactively every ~1s with basic health
    uint32_t last_beacon = System::GetNow();
    uint32_t last_sync = System::GetNow();
    uint32_t last_tx_pump = System::GetNow();
    uint32_t last_meter_send = System::GetNow();
    char wav_path[64] = {0};
    bool wav_started = false;
    static uint32_t loop_counter = 0;
    static uint32_t last_heartbeat = 0;

    // Main loop
    while(1)
    {
        
        #if WAVEX_DAISY_LOOP_PROBE_ENABLED
        s_loop_probe.Write(true);
        #endif

        #if WAVEX_MCU_LINK_DEBUG
        if (loop_counter == 0) {
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Daisy main loop started.");
        }
        #endif

        // Start busy timing for this iteration (using ticks for better accuracy)
        uint32_t busy_start_ticks = System::GetTick();

        loop_counter++;

        // Batch SPI operations to reduce timing variations
        uint32_t current_time = System::GetNow();
        
        // Process any incoming SPI messages from ESP32
        process_incoming_spi_messages();
        
        #if WAVEX_SPI_DMA_ENABLED
        // Check for DMA timeouts to prevent hanging
        WaveX::Comm::Spi_CheckTimeout();
        #endif
        
        // Pump WAV I/O for audio playback (including audition)
        #if WAVEX_AUDIO_ENGINE_ENABLED
        if (WaveX::AudioEngine::ShouldPumpWavIO()) {
            WaveX::AudioEngine::PumpWavIO();
        }
        #endif
        
        // Log SPI processing to verify it continues during auditioning
        #if WAVEX_MCU_LINK_PACKET_DEBUG
        static uint32_t spi_debug_count = 0;
        if (++spi_debug_count % 1000 == 0) {
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI message processing active (loop %lu)", (unsigned long)loop_counter);
        }
        #endif
        
        bool send_beacon = (current_time - last_beacon >= 1000);

        #if WAVEX_SPI_LINK_ENABLED
        if (send_beacon) {
            // WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Preparing heartbeat packet");
            // Send heartbeat via SPI with CPU usage
            // Log heartbeat sending to verify it continues during auditioning
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Sending heartbeat during auditioning (if active)");
            #endif
            // Create heartbeat message using flexible packet system
            WaveX::Protocol::HeartbeatMessage heartbeat_msg = {
                current_time,           // uptime_ms
                0,                      // rx_total (placeholder)
                loop_counter,           // loop_counter
                (uint16_t)(s_cpu_usage_percent * 10.0f)  // cpu_usage_percent (scaled by 10)
            };
        
            uint8_t heartbeat_buffer[64];
            size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateHeartbeatPacket(heartbeat_buffer, sizeof(heartbeat_buffer), heartbeat_msg);
            
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            s_hw->PrintLine("DAISY: Created HEARTBEAT packet, size=%d, bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                            (int)pkt_size, heartbeat_buffer[0], heartbeat_buffer[1], heartbeat_buffer[2], heartbeat_buffer[3],
                            heartbeat_buffer[4], heartbeat_buffer[5], heartbeat_buffer[6], heartbeat_buffer[7]);
            s_hw->PrintLine("DAISY: HEARTBEAT CRC bytes: %02X %02X", heartbeat_buffer[pkt_size-2], heartbeat_buffer[pkt_size-1]);
            #endif
            
            // WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Sending heartbeat packet - calling Spi_SendPreCreatedPacket");
            // Heartbeat is fire-and-forget (no ACK needed)
            int heartbeat_result = WaveX::Comm::Spi_SendPreCreatedPacket(heartbeat_buffer, pkt_size);
            // WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Heartbeat packet send result: %d", heartbeat_result);

            #if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "CPU in heartbeat: %.1f%% -> scaled=%u (0x%04X)",
                            s_cpu_usage_percent, (unsigned int)heartbeat_msg.cpu_usage_percent, (unsigned int)heartbeat_msg.cpu_usage_percent);
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI Heartbeat sent: uptime=%lu loop_counter=%lu cpu=%.1f%%",
                            (unsigned long)current_time, (unsigned long)loop_counter, s_cpu_usage_percent);
            #endif
            last_beacon = current_time;
        }
        #else
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "ERROR: SPI link is NOT enabled!");
        #endif


        // Periodic meter update - Daisy (backend) sends to ESP32 (frontend)
        // Enable during audition for audio level display
        // Send meters during audition (when WAV is playing)
        #if WAVEX_AUDIO_ENGINE_ENABLED
        bool should_send_meters = WaveX::AudioEngine::IsWavPlaying() && 
                                 (current_time - last_meter_send >= WAVEX_AUDIO_METERS_SEND_INTERVAL_MS);
        #else
        bool should_send_meters = false;
        #endif
        
        if(should_send_meters) {
            last_meter_send = current_time;
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Sending meter update during auditioning (if active)");
            #endif
            WaveX::AudioEngine::BlockMeters m;
            WaveX::AudioEngine::GetMeters(m);
            // Quantize to Q15 and clamp
            uint16_t q_rmsL = (uint16_t)(fminf(1.f, m.rmsL) * 32767.f);
            uint16_t q_rmsR = (uint16_t)(fminf(1.f, m.rmsR) * 32767.f);
            uint16_t q_pkL  = (uint16_t)(fminf(1.f, m.peakL) * 32767.f);
            uint16_t q_pkR  = (uint16_t)(fminf(1.f, m.peakR) * 32767.f);

            // Create meter push message using flexible packet system
            WaveX::Protocol::MeterPushMessage meter_msg = {
                q_rmsL,  // rms_left
                q_rmsR,  // rms_right
                q_pkL,   // peak_left
                q_pkR    // peak_right
            };
            
            uint8_t meter_buffer[64];
            size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateMeterPushPacket(meter_buffer, sizeof(meter_buffer), meter_msg);

            // Send meter data to ESP32 for audio level display
            int result = WaveX::Comm::Spi_SendPreCreatedPacket(meter_buffer, pkt_size);
            if (result) {
                #if WAVEX_MCU_LINK_PACKET_DEBUG
                WAVEX_LOG_DAISY(INTER_MCU_LINK, "Sent meter data: RMS L=%u R=%u, Peak L=%u R=%u", 
                               (unsigned)q_rmsL, (unsigned)q_rmsR, (unsigned)q_pkL, (unsigned)q_pkR);
                #endif
            } else {
                #if WAVEX_MCU_LINK_PACKET_DEBUG
                WAVEX_LOG_DAISY(INTER_MCU_LINK, "Failed to send meter data - queue full or SPI busy");
                #endif
            }
        }

        // DISABLED: Automatic WAV playback on startup
        // The system should only play audio when audition commands are received via SPI
        // from the ESP32 Sample Load/Save page
        //
        // Previous auto-playback code removed to meet requirements:
        // "When daisy starts, no audio plays (no oscillator, no .wavs)"

        #if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
        // SD card is available but we don't auto-play - waiting for audition commands
        if (sd_available && !wav_started) {
            #if WAVEX_DAISY_SD_DEBUG
            WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: card ready, waiting for audition commands (no auto-play)");
            #endif
            // Mark as "started" to prevent re-scanning, but don't actually start playback
            wav_started = true;
        }
        #endif

        // Measure work time for this iteration (excluding sleep)
        uint32_t work_end_ticks = System::GetTick();
        s_cpu_busy_ticks_accum += (work_end_ticks - busy_start_ticks);
        s_cpu_measurement_count++;
        
        // Precise timing: sleep for exactly 1ms using tick-based timing
        uint32_t sleep_start = System::GetTick();
        uint32_t target_sleep_ticks = System::GetTickFreq() / 1000; // 1ms in ticks
        while ((System::GetTick() - sleep_start) < target_sleep_ticks) {
            // Busy wait for precise timing - this is only 1ms so it's acceptable
            __NOP(); // No operation instruction for CPU efficiency
        }
        
        // Measure CPU usage every 2 seconds (reduced frequency for better performance)
        uint32_t now_ms = System::GetNow();
        if (s_cpu_last_log_ms == 0) {
            s_cpu_last_log_ms = now_ms;
            s_cpu_window_start_ticks = System::GetTick();
        }
        
        // Only do CPU measurement every 2 seconds to reduce overhead
        if (now_ms - s_cpu_last_log_ms >= 2000) {
            // Baseline already measured at startup
            
            // Calculate total time window (excluding sleep)
            uint32_t window_ticks = System::GetTick() - s_cpu_window_start_ticks;
            
            // Calculate CPU usage percentage - simplified approach
            s_cpu_usage_percent = 0.0f;
            if (window_ticks > 0 && s_cpu_busy_ticks_accum <= window_ticks) {
                // Simple approach: calculate what percentage of the window we were busy
                // This gives us a reasonable CPU usage estimate
                s_cpu_usage_percent = (float)s_cpu_busy_ticks_accum / (float)window_ticks * 100.0f;
                
                // Clamp to reasonable range
                if (s_cpu_usage_percent > 100.0f) s_cpu_usage_percent = 100.0f;
                if (s_cpu_usage_percent < 0.0f) s_cpu_usage_percent = 0.0f;
                
            } else if (window_ticks > 0) {
                // Safety fallback: if busy time exceeds window time, cap at 100%
                s_cpu_usage_percent = 100.0f;
            }
            
            // Log CPU usage - convert float to integer to avoid formatting issues
            uint32_t cpu_percent_int = (uint32_t)(s_cpu_usage_percent * 10); // Store as tenths (e.g., 6.5% = 65)
            if (cpu_percent_int > 1000) cpu_percent_int = 1000; // Cap at 100.0%
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "CPU: %lu.%lu%%", 
                            (unsigned long)(cpu_percent_int / 10), 
                            (unsigned long)(cpu_percent_int % 10));
            
            // Reset for next measurement window
            s_cpu_busy_ticks_accum = 0;
            s_cpu_window_start_ticks = System::GetTick();
            s_cpu_last_log_ms = now_ms;
            s_cpu_measurement_count = 0;
        }
    }
}
