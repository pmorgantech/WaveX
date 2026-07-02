#include "../shared/config/pin_config.h"
#include "../shared/spi_protocol/spi_protocol.h"
#include "comm/daisy_spi_link.h"
#include "comm/daisy_uart_link.h"
#include "config/link_config.h"
#include "daisy_seed.h"
#include "daisysp.h"
#include "ff.h"
#include "metrics/metrics.h"
#include "per/gpio.h"
#include "stm32h7xx_hal.h"

#include "config.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Feature macros moved to config.hpp
#include "audio/audio_engine.h"
#include "profiling/profiler.h"

#include "timebase.hpp"

using namespace daisy;
using namespace daisysp;
using namespace WaveX::Protocol;

// Hardware
DaisySeed hw;
static FATFS s_fs;  // FatFs object
static daisy::SpiHandle spi_handle;

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
#include "storage/sd_sdio.h"
#endif

// Optional loop/CPU probe GPIO
// CPU usage measurement state - improved accuracy
static uint32_t s_cpu_window_start_ticks = 0;
static uint32_t s_cpu_busy_ticks_accum = 0;
static uint32_t s_cpu_total_ticks_accum = 0;
static uint32_t s_cpu_last_log_ms = 0;
static uint32_t s_cpu_baseline_ticks_per_second = 0;
static bool s_cpu_baseline_measured = false;
static uint32_t s_cpu_measurement_count = 0;
static float s_cpu_usage_percent = 0.0f;  // Current CPU usage percentage

// QueuedMessage removed - using SPI only

/**
 * @brief Measure CPU baseline to determine maximum possible CPU usage
 * This runs a tight loop for 1 second to measure maximum ticks per second
 */
static void measure_cpu_baseline() {
    if (s_cpu_baseline_measured)
        return;

    WAVEX_LOG_DAISY(INTER_MCU_LINK, "CPU: Measuring baseline performance...");

    uint32_t start_time = System::GetNow();
    uint32_t start_ticks = System::GetTick();
    uint32_t end_time = start_time + 100;  // Run for 100ms (much shorter)

    // Tight loop to measure maximum CPU capability
    volatile uint32_t counter = 0;
    while (System::GetNow() < end_time) {
        counter++;
        // Prevent compiler optimization
        __asm__ __volatile__("" : "+r"(counter));
    }

    uint32_t end_ticks = System::GetTick();
    uint32_t elapsed_ticks = end_ticks - start_ticks;
    s_cpu_baseline_ticks_per_second = elapsed_ticks * 10;  // Scale up from 100ms to 1 second
    s_cpu_baseline_measured = true;

    WAVEX_LOG_DAISY(INTER_MCU_LINK,
                    "CPU: Baseline = %lu ticks/second (counter=%lu)",
                    (unsigned long)s_cpu_baseline_ticks_per_second,
                    (unsigned long)counter);
}

// Initialize DSP objects via AudioEngine
void InitDSP() {
    WaveX::AudioEngine::Init(hw, hw.AudioSampleRate());
}

static void PrintProfilingStats(DaisySeed& hw) {
#if WAVEX_PROFILING_ENABLED
    hw.PrintLine("\n=== Profiling Stats ===");
    uint32_t zone_count = WaveX::Profiling::Profiler::GetZoneCount();
    for (uint32_t i = 0; i < zone_count; ++i) {
        const auto* zone = WaveX::Profiling::Profiler::GetZone(i);
        if (!zone || zone->entry_count == 0)
            continue;
        float avg_us, min_us, max_us;
        zone->GetStats(avg_us, min_us, max_us);
        hw.PrintLine("%s: calls=%u avg=%.2f max=%.2f min=%.2f last=%.2f",
                     zone->name,
                     zone->entry_count,
                     avg_us,
                     max_us,
                     min_us,
                     WaveX::Profiling::CyclesToMicroseconds(zone->last_cycles));
    }
    hw.PrintLine("=======================\n");
#else
    (void)hw;
#endif
}

int main(void) {
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
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Revision: 0x%lX", 0x20036450);  // STM32H7B3
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
    WAVEX_LOG_DAISY(
        INTER_MCU_LINK, "SD: Initialization completed in %lu ms", (unsigned long)sd_init_time);
    if (sd_available) {
#if WAVEX_DAISY_SD_DEBUG
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: InitAndMount SUCCESS");
#endif
        // Give SD card time to stabilize after mount
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Starting SD card stabilization delay...");
        System::Delay(200);
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: SD card stabilization delay complete");
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: InitAndMount FAILED - no card or mount error");
    }
#endif

    // Initialize SDRAM (required for sample RAM manager)
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== SDRAM INIT START ===");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Initializing SDRAM...");
    System::Delay(10);  // Small delay before SDRAM init

    SdramHandle::Result sdram_result = hw.sdram_handle.Init();

    if (sdram_result != SdramHandle::Result::OK) {
        WAVEX_LOG_DAISY(
            INTER_MCU_LINK, "SDRAM initialization FAILED! Result: %d", (int)sdram_result);
        // Continue anyway - audio engine will handle gracefully
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SDRAM initialized successfully");
    }
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== SDRAM INIT COMPLETE ===");

    // Initialize audio (if enabled)
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== AUDIO ENGINE INIT START ===");

#if WAVEX_AUDIO_ENGINE_ENABLED
    hw.SetAudioBlockSize(Timebase::kBlockSize);  // 48-sample blocks → 1 kHz control tick
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_44KHZ);

    // Initialize DSP objects
    InitDSP();
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "DSP objects initialized");
#else
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio engine disabled (WAVEX_AUDIO_ENGINE_ENABLED = 0)");
#endif

// AudioEngine already initializes sampler and CV bus internally
#if WAVEX_AUDIO_ENGINE_ENABLED
    // Configure interrupt priorities to prevent audio starvation
    // Audio DMA should have higher priority (lower number) than SPI
    HAL_NVIC_SetPriority(SAI1_IRQn, 5, 0);
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 5, 0);  // SAI1 DMA A
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);  // SAI1 DMA B
    HAL_NVIC_SetPriority(SAI2_IRQn, 6, 0);
    HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 6, 0);  // SAI2 DMA A
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 6, 0);  // SAI2 DMA B

    // Set SPI DMA to lower priority than audio (higher number = lower priority)
    HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 10, 0);  // SPI1 DMA RX
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 10, 0);  // SPI1 DMA TX
    HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Interrupt priorities configured: Audio DMA=5/6, SPI DMA=10");
#endif

    // Initialize communication with ESP32
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== COMMUNICATION INIT START ===");

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

    spi_conf.baud_prescaler =
        daisy::SpiHandle::Config::BaudPrescaler::PS_8;  // Unused in slave mode
#if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Set SPI baud prescaler to PS_8");
#endif

    spi_conf.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
#if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(
        INTER_MCU_LINK, "Set SPI SCLK pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_SCK);
#endif

    spi_conf.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
#if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(
        INTER_MCU_LINK, "Set SPI MOSI pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_MOSI);
#endif

    spi_conf.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
#if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(
        INTER_MCU_LINK, "Set SPI MISO pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_MISO);
#endif

    spi_conf.pin_config.nss = hw.GetPin(WAVEX_DAISY_SPI_CS);
#if WAVEX_MCU_LINK_DEBUG
    WAVEX_LOG_DAISY(
        INTER_MCU_LINK, "Set SPI NSS pin to %d (from pin_config.h)", WAVEX_DAISY_SPI_CS);
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
        WAVEX_LOG_DAISY(INTER_MCU_LINK,
                        "DAISY: SPI Init SUCCESS - About to call WaveX::Comm::Spi_Init");
        WaveX::Comm::Spi_Init(hw, &spi_handle);
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "DAISY: WaveX::Comm::Spi_Init completed");
        System::Delay(100);

        // SPI is reserved for BROWSE_RESP and WAVE_DATA_CHUNK only
        // All other messages (SYNC, HEARTBEAT, METER, etc.) use UART
        WAVEX_LOG_DAISY(INTER_MCU_LINK,
                        "SPI link initialized - reserved for file browser and wave data only");
        WaveX::Comm::Spi_DebugState();
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK,
                        "ERROR: Skipping SPI link init due to SPI init failure - NO SPI CLOCK WILL "
                        "BE GENERATED");
        WAVEX_LOG_DAISY(INTER_MCU_LINK,
                        "ERROR: This explains why you see no SPI1 CLK on the scope!");
    }
#endif

    WaveX::Comm::UartLinkInit(&hw);
    WaveX::Comm::UartLinkStart();
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "DAISY: UART link started");

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
    uint32_t last_profile_print = 0;
    char wav_path[64] = {0};
    bool wav_started = false;
    static uint32_t loop_counter = 0;
    static uint32_t last_heartbeat = 0;

    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== BOOT COMPLETE - ENTERING MAIN LOOP ===");

    // Main loop
    while (1) {
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

        uint32_t current_time = System::GetNow();

        // Process any incoming SPI messages from ESP32
        uint32_t spi_start = System::GetTick();
// Fallback: if ATTN edge was missed, poll the level and start a receive
// NOTE: Disabled - relying on GPIO interrupt (EXTI15_10) for edge detection
// ESP32 now clears ATTN in post_trans_cb to eliminate race condition
// #if WAVEX_SPI_LINK_ENABLED
// WaveX::Comm::Spi_PollAttnLevel();
// #endif
#if WAVEX_SPI_LINK_ENABLED
        // The new, correct approach is to call a function that handles polling,
        // dequeuing, and processing in one step, avoiding the legacy conversion.
        WaveX::Comm::ProcessQueuedSpiMessage();
#endif
        uint32_t spi_duration = System::GetTick() - spi_start;

        WaveX::Comm::UartLinkProcess();

// Check for audio underruns (logging handled here to avoid blocking audio callback)
#if WAVEX_AUDIO_ENGINE_ENABLED
        WaveX::AudioEngine::CheckAndLogUnderruns();
#endif

        // // Log long SPI operations
        // if (spi_duration > 2) {  // More than 2ms
        //     WAVEX_LOG_DAISY(INTER_MCU_LINK, "LONG SPI: %u ms", (unsigned)spi_duration);
        // }

// Pump WAV I/O for audio playback (including audition)
#if WAVEX_AUDIO_ENGINE_ENABLED
        if (WaveX::AudioEngine::ShouldPumpWavIO()) {
            uint32_t io_start = System::GetTick();
            WaveX::AudioEngine::PumpWavIO();
            uint32_t io_duration = System::GetTick() - io_start;

            // Log long I/O operations that might cause audio pauses
            if (io_duration > 5) {  // More than 5ms
                WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                "LONG I/O: %u ms (might cause audio pause)",
                                (unsigned)io_duration);
            }
        }
#endif

// Log SPI processing to verify it continues during auditioning
#if WAVEX_MCU_LINK_PACKET_DEBUG
        static uint32_t spi_debug_count = 0;
        if (++spi_debug_count % 1000 == 0) {
            WAVEX_LOG_DAISY(INTER_MCU_LINK,
                            "SPI message processing active (loop %lu)",
                            (unsigned long)loop_counter);
        }
#endif

        bool send_beacon = (current_time - last_beacon >= 1000);

        // Report I/O performance stats every 5 seconds
        static uint32_t last_stats_report = 0;
        if (current_time - last_stats_report >= 5000) {
            last_stats_report = current_time;
#if WAVEX_AUDIO_ENGINE_ENABLED
            uint32_t io_count, max_io_duration, last_io_duration;
            WaveX::AudioEngine::GetIOStats(io_count, max_io_duration, last_io_duration);
            WAVEX_LOG_DAISY(AUDIO_ENGINE,
                            "I/O Stats: count=%u, max=%u ms, last=%u ms",
                            (unsigned)io_count,
                            (unsigned)max_io_duration,
                            (unsigned)last_io_duration);
#endif

            // Log UART stats
            WaveX::Comm::UartLinkLogStats();
        }

        // Log UART stats every 1 second for more detailed debug
        static uint32_t last_uart_log = 0;
        if (current_time - last_uart_log >= 1000) {
            last_uart_log = current_time;
            WaveX::Comm::UartLinkLogStats();
            printf("DAISY: === UART Link Stats (1sec) ===\n");
        }

        if (send_beacon) {
// WAVEX_LOG_DAISY(INTER_MCU_LINK, "DEBUG: Preparing heartbeat packet");
// Send heartbeat via UART with CPU usage
// Log heartbeat sending to verify it continues during auditioning
#if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Sending heartbeat during auditioning (if active)");
#endif
            // Create heartbeat message using flexible packet system with detailed CPU metrics
            WaveX::Protocol::HeartbeatMessage heartbeat_msg(
                current_time,  // uptime_ms
                0,             // rx_total (placeholder)
                loop_counter,  // loop_counter
                (uint16_t)(WaveX::AudioEngine::GetAvgCpuLoad() *
                           1000.0f),  // cpu_avg_percent (scaled by 10)
                (uint16_t)(WaveX::AudioEngine::GetMinCpuLoad() *
                           1000.0f),  // cpu_min_percent (scaled by 10)
                (uint16_t)(WaveX::AudioEngine::GetMaxCpuLoad() *
                           1000.0f)  // cpu_max_percent (scaled by 10)
            );

            int heartbeat_result = WaveX::Comm::UartLinkSend(
                WaveX::Protocol::MSG_HEARTBEAT, &heartbeat_msg, sizeof(heartbeat_msg));
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "DAISY: Heartbeat send result=%d", heartbeat_result);
#if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Heartbeat send result: %d", heartbeat_result);
#endif

#if WAVEX_MCU_LINK_PACKET_DEBUG
            float avg_cpu = WaveX::AudioEngine::GetAvgCpuLoad() * 100.0f;
            float min_cpu = WaveX::AudioEngine::GetMinCpuLoad() * 100.0f;
            float max_cpu = WaveX::AudioEngine::GetMaxCpuLoad() * 100.0f;
            WAVEX_LOG_DAISY(INTER_MCU_LINK,
                            "CPU in heartbeat: avg=%.1f%% min=%.1f%% max=%.1f%% -> scaled=%u/%u/%u",
                            avg_cpu,
                            min_cpu,
                            max_cpu,
                            (unsigned int)heartbeat_msg.cpu_avg_percent,
                            (unsigned int)heartbeat_msg.cpu_min_percent,
                            (unsigned int)heartbeat_msg.cpu_max_percent);
            WAVEX_LOG_DAISY(INTER_MCU_LINK,
                            "UART Heartbeat sent: uptime=%lu loop_counter=%lu",
                            (unsigned long)current_time,
                            (unsigned long)loop_counter);
#endif
            last_beacon = current_time;
        }

// Periodic meter update - Daisy (backend) sends to ESP32 (frontend)
// Enable during audition for audio level display
// Send meters during audition (when WAV is playing)
#if WAVEX_AUDIO_ENGINE_ENABLED
        bool should_send_meters =
            WaveX::AudioEngine::IsWavPlaying() &&
            (current_time - last_meter_send >= WAVEX_AUDIO_METERS_SEND_INTERVAL_MS);
#else
        bool should_send_meters = false;
#endif

        if (should_send_meters) {
            last_meter_send = current_time;
#if WAVEX_MCU_LINK_PACKET_DEBUG
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "Sending meter update during auditioning (if active)");
#endif
            WaveX::AudioEngine::BlockMeters m;
            WaveX::AudioEngine::GetMeters(m);
            // Quantize to Q15 and clamp
            uint16_t q_rmsL = (uint16_t)(fminf(1.f, m.rmsL) * 32767.f);
            uint16_t q_rmsR = (uint16_t)(fminf(1.f, m.rmsR) * 32767.f);
            uint16_t q_pkL = (uint16_t)(fminf(1.f, m.peakL) * 32767.f);
            uint16_t q_pkR = (uint16_t)(fminf(1.f, m.peakR) * 32767.f);

            // Create meter push message for UART transmission
            WaveX::Protocol::MeterPushMessage meter_msg(q_rmsL,  // rms_left
                                                        q_rmsR,  // rms_right
                                                        q_pkL,   // peak_left
                                                        q_pkR    // peak_right
            );

            // Send meter data via UART (not SPI - SPI reserved for browse/wave only)
            int result = WaveX::Comm::UartLinkSend(
                WaveX::Protocol::MSG_METER_PUSH, &meter_msg, sizeof(meter_msg));
#if WAVEX_MCU_LINK_PACKET_DEBUG
            if (result > 0) {
                WAVEX_LOG_DAISY(INTER_MCU_LINK,
                                "Sent meter data via UART: RMS L=%u R=%u, Peak L=%u R=%u",
                                (unsigned)q_rmsL,
                                (unsigned)q_rmsR,
                                (unsigned)q_pkL,
                                (unsigned)q_pkR);
            } else {
                WAVEX_LOG_DAISY(
                    INTER_MCU_LINK, "Failed to send meter data via UART: result=%d", result);
            }
#endif
        }

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
        // SD card is available but we don't auto-play - waiting for audition commands
        if (sd_available && !wav_started) {
#if WAVEX_DAISY_SD_DEBUG
            WAVEX_LOG_DAISY(AUDIO_ENGINE,
                            "SD: card ready, waiting for audition commands (no auto-play)");
#endif
            // Mark as "started" to prevent re-scanning, but don't actually start playback
            wav_started = true;
        }
#endif

#if WAVEX_PROFILING_ENABLED
        if (current_time - last_profile_print >= 5000) {
            PrintProfilingStats(hw);
            WaveX::Profiling::Profiler::ResetAll();
            last_profile_print = current_time;
        }
#endif
    }
}
