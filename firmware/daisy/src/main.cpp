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
#include "comm/inter_uart.h"
#include "comm/message_router.h"
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

// Whether we've sent the initial SYNC packet
static bool sync_sent = false;

using WaveX::Comm::QueuedMessage;

// Audio callback delegates to AudioEngine
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    WaveX::AudioEngine::Callback(in, out, size);
}

// Router moved to comm/message_router

// (UART ISR and init moved to comm/inter_uart)

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
    hw.PrintLine("Hardware initialized successfully");
    hw.PrintLine("Revision: 0x%lX", DBGMCU->IDCODE);
    
    // Initialize audio (if enabled)
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.SetAudioBlockSize(Timebase::kBlockSize); // 48-sample blocks → 1 kHz control tick
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // Initialize DSP objects
    InitDSP();
    hw.PrintLine("DSP objects initialized");
    #else
    hw.PrintLine("Audio engine disabled (WAVEX_AUDIO_ENGINE_ENABLED = 0)");
    #endif

    // AudioEngine already initializes sampler and CV bus internally
    
    // Initialize UART communication with ESP32 (disabled for isolation if macro set)
    #if !WAVEX_DEBUG_DISABLE_UART
    WaveX::Comm::Uart_Init(hw);
    System::Delay(100);
    #else
    hw.PrintLine("UART init disabled (WAVEX_DEBUG_DISABLE_UART)");
    #endif
    
    // Mount SD card (SPI MVP)
    hw.PrintLine("Attempting SD card mount...");
    
    // Test SPI communication first
    if (WaveX::Storage::SdSpi::Init(hw, seed::D2, seed::D1, seed::D6, seed::D9, SpiHandle::Config::Peripheral::SPI_3)) {
        hw.PrintLine("SD SPI init OK");
    } else {
        hw.PrintLine("SD SPI init FAILED");
    }
    
    FRESULT fr = f_mount(&s_fs, "", 1);
    if(fr == FR_OK) {
        hw.PrintLine("SD mount OK (SPI)\n");
    } else {
        hw.PrintLine("SD mount FAILED: %d", (int)fr);
        switch(fr) {
            case FR_DISK_ERR: hw.PrintLine("  - Disk error (hardware failure)"); break;
            case FR_INT_ERR: hw.PrintLine("  - Internal error"); break;
            case FR_NOT_READY: hw.PrintLine("  - Drive not ready (init failed)"); break;
            case FR_NO_FILESYSTEM: hw.PrintLine("  - No valid FAT volume"); break;
            default: hw.PrintLine("  - Unknown error"); break;
        }
    }

    // Start audio processing (if enabled)
    #if WAVEX_AUDIO_ENGINE_ENABLED
    hw.StartAudio(AudioCallback);
    hw.PrintLine("Audio started - you should hear a 440 Hz tone");
    #else
    hw.PrintLine("Audio engine disabled (WAVEX_AUDIO_ENGINE_ENABLED = 0)");
    #endif
    
    // Startup message
    hw.PrintLine("WaveX Daisy firmware starting");
    #if WAVEX_UART_DEBUG_LOG
    hw.PrintLine("USART1: TX=D13 (PB6), RX=D14 (PB7), Baud=%d", INTER_MCU_UART_BAUD_RATE);
    #if WAVEX_UART_RX_IRQ_MODE
    hw.PrintLine("UART RX Mode: Interrupt-driven");
    #else
    hw.PrintLine("UART RX Mode: Polling");
    #endif
    hw.PrintLine("Ready for inter-MCU communication");
    #endif

    // Main loop
    // Periodic liveness beacon: respond proactively every ~1s with basic health
    uint32_t last_beacon = System::GetNow();
    uint32_t last_tx_pump = System::GetNow();
    
    // Main loop
    while(1)
    {
        static uint32_t loop_counter = 0;
        loop_counter++;
        
        
        // Handle UART reception based on mode
        #if !WAVEX_DEBUG_DISABLE_UART
        // Parse any bytes accumulated in the RX ring into protocol messages
        WaveX::Comm::Uart_ProcessRxRing();

        // Proactively send one queued message every few ms (push model)
        if(WaveX::Comm::Uart_HasPendingData() && (System::GetNow() - last_tx_pump) >= 5) {
            last_tx_pump = System::GetNow();
            QueuedMessage msg;
            if(WaveX::Comm::Uart_GetNextQueuedMessage(msg)) {
                WaveX::Comm::Uart_PrepareResponsePacket(msg);
            }
        }

        // Debug output moved to the conditional blocks above
        #endif

        // Send SYNC packet every 2 seconds
        if(System::GetNow() > 2000 && !sync_sent) {
            uint8_t p[] = {0};
            uint8_t txbuf[64];
            size_t len = ProtocolHandler::CreateGenericPacket(txbuf, sizeof(txbuf), MSG_SYNC, p, 0);
            if(len > 0) WaveX::Comm::Uart_Send(txbuf, len);
            sync_sent = true;
            #if WAVEX_UART_DEBUG_LOG
            hw.PrintLine("Manually sent SYNC packet");
            #endif
        }

        // Periodic liveness beacon (approx 1s)
        #if !WAVEX_DEBUG_DISABLE_UART
        if(System::GetNow() - last_beacon >= 1000) {
            last_beacon = System::GetNow();
            HeartbeatMessage hb{};
            hb.uptime_ms = last_beacon;
            hb.rx_total = WaveX::Comm::Uart_GetRxTotal();
            hb.loop_counter = loop_counter;
            uint8_t txbuf[64];
            size_t packet_len = ProtocolHandler::CreateHeartbeatPacket(
                txbuf,
                sizeof(txbuf),
                hb);
            if(packet_len > 0) {
                WaveX::Comm::Uart_Send(txbuf, packet_len);
                #if WAVEX_UART_DEBUG_LOG
                hw.PrintLine("Heartbeat sent: uptime=%lu rx_total=%lu", (unsigned long)hb.uptime_ms, (unsigned long)hb.rx_total);
                #endif
            }
        }
        #endif

        // 5ms delay
        System::Delay(5);
    }
}
