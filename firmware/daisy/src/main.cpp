#include "daisy_seed.h"
#include "daisysp.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cmath>
#include "../shared/spi_protocol/spi_protocol.h"
#include "../shared/config/pin_config.h"
#include "config.hpp"
#include "comm/inter_spi.h"
#include "comm/inter_spi_hal.h"
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

// Helper function to process SPI packets
void process_spi_packet(::pkt_t* pkt) {
    // Convert SPI packet to existing protocol format for compatibility
    // This allows existing application code to work unchanged
    
    switch (pkt->h.type) {
        case 0x1001: // Control change
            // Convert to existing protocol format
            // ... implementation ...
            break;
        // ... handle other packet types ...
    }
}

// Optional loop/CPU probe GPIO
// CPU usage measurement state
static uint32_t s_cpu_last_us = 0;
static uint32_t s_cpu_busy_us_accum = 0;
static uint32_t s_cpu_window_start_us = 0;

// QueuedMessage removed - using SPI only

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
    
    // Initialize CPU usage measurement window
    s_cpu_last_us = System::GetUs();
    s_cpu_window_start_us = s_cpu_last_us;
    
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
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Calling WaveX::Comm::Spi_Init");
        WaveX::Comm::Spi_Init(hw, &spi_handle);
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Returned from Spi_Init");
        System::Delay(100);
        
        // Test SPI immediately after init to verify it's working
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Testing SPI with immediate test packet");
        uint8_t test_payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        int test_result = WaveX::Comm::Spi_Send(0x99, test_payload, 4);  // Test packet
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

    // Start audio processing (if enabled)
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.StartAudio(WaveX::AudioEngine::Callback);
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio started - you should hear a 440 Hz tone");
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

        // Start busy timing for this iteration
        uint32_t busy_start_us = System::GetUs();

        loop_counter++;

        // Send SYNC packet every 2 seconds
        uint32_t current_time = System::GetNow();   
        if(current_time - last_sync >= 2000) {
            #if WAVEX_SPI_LINK_ENABLED
            // Send SYNC via SPI
            uint8_t payload[1] = {0};
            WaveX::Comm::Spi_Send(0x92, payload, 1);  // SYNC packet type
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI SYNC packet sent");
            WaveX::Comm::Spi_DebugState();  // Add debug output
            #endif
            #endif
            last_sync = current_time;
        }

        // Periodic liveness beacon (approx 1s)
        #if WAVEX_SPI_LINK_ENABLED
        current_time = System::GetNow();
        if(current_time - last_beacon >= 1000) {
            last_beacon = current_time;
            // Send heartbeat via SPI
            uint8_t payload[12];
            payload[0] = (last_beacon >> 0) & 0xFF;
            payload[1] = (last_beacon >> 8) & 0xFF;
            payload[2] = (last_beacon >> 16) & 0xFF;
            payload[3] = (last_beacon >> 24) & 0xFF;
            payload[4] = (loop_counter >> 0) & 0xFF;
            payload[5] = (loop_counter >> 8) & 0xFF;
            payload[6] = (loop_counter >> 16) & 0xFF;
            payload[7] = (loop_counter >> 24) & 0xFF;
            payload[8] = 0; // rx_total placeholder
            payload[9] = 0;
            payload[10] = 0;
            payload[11] = 0;
                       
            WaveX::Comm::Spi_Send(0x91, payload, 12);  // Heartbeat packet type
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI Heartbeat sent: uptime=%lu loop_counter=%lu", (unsigned long)last_beacon, (unsigned long)loop_counter);
            WaveX::Comm::Spi_DebugState();  // Add debug output for heartbeat too
            #endif
        }

        // Periodic meter update
        if(current_time - last_meter_send >= WAVEX_AUDIO_METERS_SEND_INTERVAL_MS) {
            last_meter_send = current_time;
            WaveX::AudioEngine::BlockMeters m;
            WaveX::AudioEngine::GetMeters(m);
            // Quantize to Q15 and clamp
            uint16_t q_rmsL = (uint16_t)(fminf(1.f, m.rmsL) * 32767.f);
            uint16_t q_rmsR = (uint16_t)(fminf(1.f, m.rmsR) * 32767.f);
            uint16_t q_pkL  = (uint16_t)(fminf(1.f, m.peakL) * 32767.f);
            uint16_t q_pkR  = (uint16_t)(fminf(1.f, m.peakR) * 32767.f);

            uint8_t payload[8];
            payload[0] = (uint8_t)(q_rmsL & 0xFF);
            payload[1] = (uint8_t)(q_rmsL >> 8);
            payload[2] = (uint8_t)(q_rmsR & 0xFF);
            payload[3] = (uint8_t)(q_rmsR >> 8);
            payload[4] = (uint8_t)(q_pkL & 0xFF);
            payload[5] = (uint8_t)(q_pkL >> 8);
            payload[6] = (uint8_t)(q_pkR & 0xFF);
            payload[7] = (uint8_t)(q_pkR >> 8);

            WaveX::Comm::Spi_Send(0x90, payload, sizeof(payload));
        }

        // SD WAV: find and start once
        #if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
        if (!wav_started && sd_available) {
            #if WAVEX_DAISY_SD_DEBUG
            WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: scanning root for .wav files");
            #endif
            DIR dir;
            FILINFO fno;
            FRESULT res = f_opendir(&dir, "/");
            if (res == FR_OK) {
                #if WAVEX_DAISY_SD_DEBUG
                WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: f_opendir('/') OK, reading entries");
                #endif
                uint32_t file_count = 0;
                for (;;) {
                    res = f_readdir(&dir, &fno);
                    if (res != FR_OK || fno.fname[0] == 0) break;
                    file_count++;
                    #if WAVEX_DAISY_SD_DEBUG
                    WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: entry %lu: %s %s", 
                        (unsigned long)file_count, 
                        (fno.fattrib & AM_DIR) ? "[DIR]" : "[FILE]", 
                        fno.fname);
                    #endif
                    if (!(fno.fattrib & AM_DIR)) {
                        // check .wav extension (case-insensitive)
                        const char* name = fno.fname;
                        size_t n = strlen(name);
                        if (n >= 4) {
                            const char* ext = name + (n - 4);
                            char e0 = ext[0] | 0x20;
                            char e1 = ext[1] | 0x20;
                            char e2 = ext[2] | 0x20;
                            char e3 = ext[3] | 0x20;
                            if (e0 == '.' && e1 == 'w' && e2 == 'a' && e3 == 'v') {
                                snprintf(wav_path, sizeof(wav_path), "/%s", name);
                                #if WAVEX_DAISY_SD_DEBUG
                                WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: found wav %s", wav_path);
                                #endif
                                break;
                            }
                        }
                    }
                }
                f_closedir(&dir);
                #if WAVEX_DAISY_SD_DEBUG
                WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: scanned %lu entries total", (unsigned long)file_count);
                #endif
            } else {
                #if WAVEX_DAISY_SD_DEBUG
                WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: f_opendir('/') failed with FRESULT %d", (int)res);
                #endif
            }
            if (wav_path[0]) {
                if (WaveX::AudioEngine::OpenWav(wav_path)) {
                    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Playing WAV: %s", wav_path);
                    wav_started = true;
                } else {
                    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Failed to open WAV: %s", wav_path);
                    wav_path[0] = 0;
                }
            } else {
                #if WAVEX_DAISY_SD_DEBUG
                WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: no .wav found in root");
                #endif
            }
        }
        // Pump WAV I/O to fill ring buffer (adaptive polling)
        if (wav_started && sd_available) {
            // Only pump I/O when ring buffer is getting low (adaptive polling)
            if (WaveX::AudioEngine::ShouldPumpWavIO()) {
                #if WAVEX_DAISY_SD_DEBUG
                static uint32_t last_log = 0; uint32_t now = System::GetNow();
                if (now - last_log > 1000) { last_log = now; WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD: pumping WAV I/O (adaptive)"); }
                #endif
                WaveX::AudioEngine::PumpWavIO();
            }
        }
        #endif
        #endif

        // Compute busy time for this loop iteration and sleep briefly
        {
            uint32_t now_us = System::GetUs();
            s_cpu_busy_us_accum += (now_us - busy_start_us);
        }
        System::Delay(10);

        // Emit CPU usage once per second
        static uint32_t last_cpu_log_ms = 0;
        uint32_t now_ms = System::GetNow();
        if(last_cpu_log_ms == 0) {
            last_cpu_log_ms = now_ms;
            s_cpu_window_start_us = System::GetUs();
        }
        if(now_ms - last_cpu_log_ms >= 1000) {
            uint32_t window_us = (System::GetUs() - s_cpu_window_start_us);
            uint32_t pct10 = window_us ? (s_cpu_busy_us_accum * 1000u) / window_us : 0u; // tenths
            if (pct10 > 1000u) pct10 = 1000u; // clamp at 100.0%
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "CPU busy: %lu.%lu%% (window %lu us)",
                            (unsigned long)(pct10 / 10u),
                            (unsigned long)(pct10 % 10u),
                            (unsigned long)window_us);
            s_cpu_busy_us_accum = 0;
            s_cpu_window_start_us = System::GetUs();
            last_cpu_log_ms = now_ms;
        }
    }
}
