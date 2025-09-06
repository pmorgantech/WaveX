#include "daisy_seed.h"
#include "daisysp.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cmath>
#include "spi_protocol/protocol.h"
#include "config.hpp"
#include "comm/inter_spi.h"
#include "comm/inter_spi_hal.h"
#include "config/link_config.h"
#include "metrics/metrics.h"
#include "ff.h"
#include "storage/sd_spi.h"

// Feature macros moved to config.hpp
#include "timebase.hpp"
#include "audio/audio_engine.h"

using namespace daisy;
using namespace daisysp;
using namespace WaveX::Protocol;
using namespace WaveX::Storage;

// Hardware
DaisySeed hw;
static FATFS s_fs; // FatFs object
static daisy::SpiHandle spi_handle;

// Helper function to process SPI packets
void process_spi_packet(pkt_t* pkt) {
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

// Whether we've sent the initial SYNC packet
static bool sync_sent = false;

// QueuedMessage removed - using SPI only

// Audio callback delegates to AudioEngine
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    WaveX::AudioEngine::Callback(in, out, size);
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
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Hardware initialized successfully");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Revision: 0x%lX", DBGMCU->IDCODE);
    
    // Initialize audio (if enabled)
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.SetAudioBlockSize(Timebase::kBlockSize); // 48-sample blocks → 1 kHz control tick
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
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
    
    // Prefer raw HAL SPI1 slave (interrupt-driven) to avoid libDaisy SPI conflicts
    WaveX::Comm::SpiHal_Init(hw);
    #if 0
    // If needed later: libDaisy SPI1 init path (currently disabled)
    daisy::SpiHandle::Config spi_conf;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Created SPI config object");
    #endif
    
    // Use configuration macros from pin_config.h and link_config.h
    spi_conf.periph = (daisy::SpiHandle::Config::Peripheral::SPI_1);  // WAVEX_DAISY_SPI_PERIPH
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI peripheral to SPI_1");
    #endif
    
    spi_conf.mode = daisy::SpiHandle::Config::Mode::SLAVE;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI mode to SLAVE");
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
    
    spi_conf.nss = daisy::SpiHandle::Config::NSS::HARD_INPUT;
    #if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI NSS to HARD_INPUT");
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
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Skipping SPI link init due to SPI init failure");
    }
    #endif
    #endif
    
    // Mount SD card disabled for now: avoid SPI pin conflicts with inter-MCU link
    // The previous SD init used D9 (MISO) which conflicts with SPI1 MISO to ESP32.
    // Re-enable only after assigning SD to non-overlapping pins/peripheral.
    // WAVEX_LOG_DAISY(INTER_MCU_LINK, "Skipping SD card init to prevent SPI pin conflicts");

    // Start audio processing (if enabled)
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.StartAudio(AudioCallback);
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio started - you should hear a 440 Hz tone");
    #else
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio engine disabled (WAVEX_AUDIO_ENGINE_ENABLED = 0)");
    #endif
    


    // Main loop
    // Periodic liveness beacon: respond proactively every ~1s with basic health
    uint32_t last_beacon = System::GetNow();
    uint32_t last_tx_pump = System::GetNow();
    
    // Main loop
    while(1)
    {
        static uint32_t loop_counter = 0;
        #if WAVEX_MCU_LINK_DEBUG
        if (loop_counter == 0) {
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Daisy main loop started.");
        }
        #endif
        loop_counter++;
        
        
        // Handle communication based on mode
        #if WAVEX_SPI_LINK_ENABLED
        // Handle SPI communication (HAL path)
        WaveX::Comm::SpiHal_Service();
        #endif

        // Send SYNC packet every 2 seconds
        if(System::GetNow() > 2000 && !sync_sent) {
            #if WAVEX_SPI_LINK_ENABLED
            // Send SYNC via SPI
            uint8_t payload[1] = {0};
            WaveX::Comm::Spi_Send(0x0000, payload, 1);
            #if WAVEX_MCU_LINK_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Manually sent SPI SYNC packet");
            WaveX::Comm::Spi_DebugState();  // Add debug output
            #endif
            #endif
            sync_sent = true;
        }

        // Periodic liveness beacon (approx 1s)
        #if WAVEX_SPI_LINK_ENABLED
        if(System::GetNow() - last_beacon >= 1000) {
            last_beacon = System::GetNow();
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
                       
            WaveX::Comm::Spi_Send(0x1000, payload, 12);
            #if WAVEX_MCU_LINK_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI Heartbeat sent: uptime=%lu loop_counter=%lu", (unsigned long)last_beacon, (unsigned long)loop_counter);
            WaveX::Comm::Spi_DebugState();  // Add debug output for heartbeat too
            #endif
        }
        #endif

        // 5ms delay
        System::Delay(1);
    }
}
