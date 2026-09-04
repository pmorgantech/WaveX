#include "../shared/config/pin_config.h"
#include "comm/daisy_filesystem.h"
#include "comm/daisy_spi_link.h"
#include "comm/daisy_uart_link.h"
#include "comm/diag_push.h"
#include "comm/log_ring.h"
#include "config/link_config.h"
#include "config/logging_config.h"
#include "daisy_seed.h"
#include "memory_sections.h"
#include "per/gpio.h"
#include "stm32h7xx_hal.h"

#include "config.hpp"
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Feature macros moved to config.hpp
#include "audio/audio_engine.h"
#include "profiling/profiler.h"

#include "timebase.hpp"

using namespace daisy;
using namespace WaveX::Protocol;

// Hardware
DaisySeed hw;
#if WAVEX_SPI_LINK_ENABLED
// Every use of spi_handle lives inside a WAVEX_SPI_LINK_ENABLED block below;
// UART is the transport of record (roadmap 0.2) and this flag is hard-coded
// 0, so the declaration is guarded to match rather than carrying a permanently
// unused static in every image.
static daisy::SpiHandle spi_handle;
#endif

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
#include "storage/sd_sdio.h"
#endif

// (Review M11: a hand-rolled CPU-usage measurement scaffold sat here -
// eight state variables plus a 100 ms boot-time busy-loop baseline - whose
// results nothing ever read. Deleted; audio CPU load comes from libDaisy's
// CpuLoadMeter in the audio engine and is reported in heartbeats.)

// ---------------------------------------------------------------------------
// Host-triggered DFU entry (scripts/daisy_dfu_trigger.py)
//
// The persistent app runs from QSPI (CMake DAISY_STORAGE=qspi), so the Daisy
// bootloader is present in internal flash and System::ResetToBootloader() is
// valid - it refuses only when the program itself runs from internal flash.
// Writing the token below to the USB CDC port reboots into the bootloader's
// DFU mode with an infinite timeout, letting dfu-util flash without anyone
// touching BOOT/RESET. Firmware that is already wedged cannot answer, so the
// manual BOOT+RESET sequence behind `make daisy-flash` remains the recovery
// path.
// ---------------------------------------------------------------------------
static constexpr char kDfuTriggerToken[] = "WAVEX-ENTER-DFU";
static constexpr size_t kDfuTriggerLen = sizeof(kDfuTriggerToken) - 1;
static volatile bool s_dfu_requested = false;

// ---------------------------------------------------------------------------
// Host-driven console commands on the CDC port
//
// A line "WAVEX-<VERB> ...\n" is captured by the USB ISR and parsed on the
// main loop, which replies through the log ring on the same port. Verbs:
//
//   LOG <MODULE|*> <LEVEL> / LOG ?   runtime log levels (scripts/wavex_log.py)
//   FILTER <wavex|daisysp> [12|24] [drive%] / FILTER ?
//                                    per-voice lowpass selection for A/B
//                                    listening (scripts/wavex_filter.py,
//                                    audio/voice_filter.hpp)
//
// Same discipline as the DFU token: the USB ISR only captures bytes; parsing
// and the reply happen on the main loop.
//
// Guarded by WAVEX_DEBUG_HARNESS_ENABLED so a release image carries neither
// the buffer nor the ISR match state (README.md#build-profiles).
// The DFU token above is deliberately NOT guarded - it is the only reflash
// path that needs no BOOT+RESET. (Its bytes also match this prefix and get
// captured as the verb "ENTER-DFU", which the dispatcher ignores; the DFU
// matcher has already done its job by then.)
// ---------------------------------------------------------------------------
#if WAVEX_DEBUG_HARNESS_ENABLED
static constexpr char kLogCmdToken[] = "WAVEX-";
static constexpr size_t kLogCmdTokenLen = sizeof(kLogCmdToken) - 1;
static char s_log_cmd_buf[64];
// release/acquire pair: the ISR fills s_log_cmd_buf and only then publishes
// via this flag; the main loop must not observe true before those writes.
static std::atomic<bool> s_log_cmd_pending{false};
#endif

#if WAVEX_DEBUG_HARNESS_ENABLED
static const char* FilterTopologyName(uint8_t topology) {
    return topology == 1 ? "daisysp" : "wavex";
}

// "FILTER ?" reports; "FILTER <wavex|daisysp> [12|24] [drive 0-100]" applies.
// Drive is a percentage so the parser needs no float support. Main-loop
// context; the engine publishes the selection to the callback.
static void HandleFilterCommand(const char* args) {
    while (*args == ' ')
        ++args;
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::AudioEngine::FilterSelection sel = WaveX::AudioEngine::GetFilterSelection();
    if (*args != '?' && *args != '\0') {
        char word[16];
        size_t n = 0;
        while (*args != ' ' && *args != '\0' && n + 1 < sizeof(word))
            word[n++] = *args++;
        word[n] = '\0';
        if (std::strcmp(word, "wavex") == 0 || std::strcmp(word, "mine") == 0) {
            sel.topology = 0;
        } else if (std::strcmp(word, "daisysp") == 0 || std::strcmp(word, "dsp") == 0) {
            sel.topology = 1;
        } else {
            WaveX::Log::PrintLine(
                "WAVEX-FILTER: bad topology '%s' - usage: WAVEX-FILTER <wavex|daisysp> "
                "[12|24] [drive 0-100]",
                word);
            return;
        }
        // Optional numeric fields, in order: slope, drive%.
        int fields[2] = {-1, -1};
        for (int f = 0; f < 2; ++f) {
            while (*args == ' ')
                ++args;
            if (*args < '0' || *args > '9')
                break;
            int value = 0;
            while (*args >= '0' && *args <= '9' && value < 1000)
                value = value * 10 + (*args++ - '0');
            fields[f] = value;
        }
        if (fields[0] == 12 || fields[0] == 24) {
            sel.slope_db = static_cast<uint8_t>(fields[0]);
        } else if (fields[0] != -1) {
            WaveX::Log::PrintLine("WAVEX-FILTER: slope must be 12 or 24 (got %d)", fields[0]);
            return;
        }
        if (fields[1] != -1) {
            sel.drive = static_cast<float>(fields[1] > 100 ? 100 : fields[1]) / 100.0f;
        }
        WaveX::AudioEngine::SetFilterSelection(sel);
    }
    WaveX::Log::PrintLine("WAVEX-FILTER: topology=%s slope=%u drive=%d%%",
                          FilterTopologyName(sel.topology),
                          static_cast<unsigned>(sel.slope_db),
                          static_cast<int>(sel.drive * 100.0f + 0.5f));
#else
    (void)args;
    WaveX::Log::PrintLine("WAVEX-FILTER: audio engine disabled in this build");
#endif
}
#endif

// Runs in USB interrupt context: match bytes and set a flag, nothing else.
// No logging, no allocation, and above all no reset from in here.
static void UsbRxCallback(uint8_t* buff, uint32_t* len) {
    if (buff == nullptr || len == nullptr) {
        return;
    }
    static size_t matched = 0;
#if WAVEX_DEBUG_HARNESS_ENABLED
    static size_t log_matched = 0;
    static size_t log_capture = 0;  // bytes of command captured; 0 = not capturing
    static bool log_capturing = false;
#endif
    for (uint32_t i = 0; i < *len; ++i) {
        const char c = static_cast<char>(buff[i]);
        if (c == kDfuTriggerToken[matched]) {
            if (++matched == kDfuTriggerLen) {
                s_dfu_requested = true;
                matched = 0;
            }
        } else {
            // A mismatch can still be the start of a fresh match.
            matched = (c == kDfuTriggerToken[0]) ? 1u : 0u;
        }

#if WAVEX_DEBUG_HARNESS_ENABLED
        if (log_capturing) {
            if (c == '\n' || c == '\r' || log_capture + 1 >= sizeof(s_log_cmd_buf)) {
                s_log_cmd_buf[log_capture] = '\0';
                log_capturing = false;
                log_capture = 0;
                s_log_cmd_pending.store(true, std::memory_order_release);
            } else {
                s_log_cmd_buf[log_capture++] = c;
            }
        } else if (c == kLogCmdToken[log_matched]) {
            if (++log_matched == kLogCmdTokenLen) {
                log_matched = 0;
                // While a previous command awaits the main loop, drop this
                // one rather than scribble over the buffer being parsed.
                if (!s_log_cmd_pending.load(std::memory_order_acquire)) {
                    log_capturing = true;
                    log_capture = 0;
                }
            }
        } else {
            log_matched = (c == kLogCmdToken[0]) ? 1u : 0u;
        }
#endif
    }
}

// Card insertion/removal. On removal the filesystem is already unmounted and
// the card is physically gone, so the open WAV handle is dead: drop playback
// rather than let the streaming path keep failing reads against it.
static void OnSdCardEvent(bool inserted) {
    if (inserted) {
        // The browser does NOT refresh by itself - it only re-lists when the
        // user leaves the page and returns - so it has to be told.
        WaveX::Comm::NotifyStorageAvailable();
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    if (WaveX::AudioEngine::IsWavPlaying()) {
        WaveX::AudioEngine::CloseWav();
        WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD card removed during playback - audio stopped");
    }
#endif
    // Sent even when nothing was playing: the browser's list refers to files
    // that no longer exist, so it has to be cleared either way.
    WaveX::Comm::NotifyStorageLost();
}

// Initialize DSP objects via AudioEngine
void InitDSP(bool sdram_available) {
    WaveX::AudioEngine::Init(hw, hw.AudioSampleRate(), sdram_available);
}

#if WAVEX_PROFILING_ENABLED
static void PrintProfilingStats(DaisySeed& hw) {
    WaveX::Log::PrintLine("\n=== Profiling Stats ===");
    uint32_t zone_count = WaveX::Profiling::Profiler::GetZoneCount();
    for (uint32_t i = 0; i < zone_count; ++i) {
        const auto* zone = WaveX::Profiling::Profiler::GetZone(i);
        if (!zone || zone->entry_count == 0)
            continue;
        // Nanoseconds, not %f microseconds: newlib-nano's vfprintf has no
        // float support in this image, so the old "%.2f" printed nothing and
        // this dump reported empty avg/max/min for its whole life.
        uint32_t avg_ns, min_ns, max_ns;
        zone->GetStatsNs(avg_ns, min_ns, max_ns);
        WaveX::Log::PrintLine("%s: calls=%u avg=%u ns max=%u ns min=%u ns last=%u ns",
                              zone->name,
                              (unsigned)zone->entry_count,
                              (unsigned)avg_ns,
                              (unsigned)max_ns,
                              (unsigned)min_ns,
                              (unsigned)WaveX::Profiling::CyclesToNanoseconds(zone->last_cycles));
    }
    WaveX::Log::PrintLine("=======================\n");
}
#endif

int main(void) {
    // Initialize Daisy Seed hardware.
    //
    // Init(true) selects System::Config::Boost(): SysClk 480 MHz instead of
    // the 400 MHz default, which is the STM32H750's rated maximum. Boost()
    // differs from Defaults() in the CPU frequency and NOTHING else - both
    // already set use_dcache and use_icache - so this does not change cache
    // behaviour or the DMA coherency rules that depend on it.
    //
    // Safe for the peripherals that matter here because the clock tree keeps
    // them off PLL1: SDRAM/FMC runs from PLL2 and the audio SAI from PLL3, so
    // neither the sample memory timing nor the 48 kHz sample rate moves with
    // the core clock. What does move is the TIM2 tick feeding System::GetTick()
    // (PCLK1*2, 200 -> 240 MHz); every site converting ticks to microseconds
    // derives the divisor from System::GetTickFreq() at runtime, so they follow
    // it. Grep before adding a hard-coded one.
    hw.Configure();
    hw.Init(/*boost=*/true);
    // Before anything reads DTCM-placed state: the section is (NOLOAD) and
    // libDaisy's startup does not cover it, so its initializers never landed.
    WaveX::MemorySections::InitDtcmBss();
    WaveX::MemorySections::InitItcm();

    // Initialize USB CDC for debugging
    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);

    // Route logging through the non-blocking ring before anything logs. See
    // comm/log_ring.h: libDaisy's Logger spins unbounded on a busy CDC
    // endpoint, which stalls the loop that refills the audio ring.
    WaveX::Log::Init(&hw);

    // Start USB CDC logging. WAVEX_DAISY_WAIT_FOR_SERIAL=1 (bench builds)
    // blocks here - and on every subsequent PrintLine - until a terminal
    // attaches; the default 0 boots standalone and drops early logs instead
    // (hardware_config.h / review H8).
    hw.StartLog(WAVEX_DAISY_WAIT_FOR_SERIAL != 0);

    // Listen for the host's DFU trigger token. Registered after StartLog so it
    // cannot be clobbered by the logger's own USB bring-up; the logger only
    // ever transmits, so there is no callback to conflict with.
    hw.usb_handle.SetReceiveCallback(UsbRxCallback, UsbHandle::FS_INTERNAL);

    // Add delay to ensure USB CDC is ready before logging
    System::Delay(100);

    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== WAVEX DAISY BOOT START ===");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "Hardware initialized successfully");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "USB CDC logging active");
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI Link Enabled: %d", WAVEX_SPI_LINK_ENABLED);
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI DMA Enabled: %d", WAVEX_SPI_DMA_ENABLED);

    // Initialize SD card (SDMMC + FatFS) if enabled
    bool sd_available = false;
#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: Starting SD card initialization...");
    uint32_t sd_start_time = System::GetNow();

#if WAVEX_DAISY_SD_DEBUG
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "SD: InitAndMount start");
#endif
    WaveX::Storage::SdSdio::SetCardEventCallback(OnSdCardEvent);
    sd_available = WaveX::Storage::SdSdio::InitAndMount(hw, WAVEX_DAISY_SD_AUTO_FORMAT != 0);

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
        // Continue with streaming-only audio. The audio engine receives this
        // state and leaves all SDRAM-backed sample operations disabled.
    } else {
        WAVEX_LOG_DAISY(INTER_MCU_LINK, "SDRAM initialized successfully");
    }
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== SDRAM INIT COMPLETE ===");

    // Initialize audio (if enabled)
    WAVEX_LOG_DAISY(INTER_MCU_LINK, "=== AUDIO ENGINE INIT START ===");

#if WAVEX_AUDIO_ENGINE_ENABLED
    hw.SetAudioBlockSize(Timebase::kBlockSize);  // 48-sample blocks → 1 kHz control tick
    // 48 kHz: preserves the 1-block = 1-ms control-tick invariant (48-sample
    // blocks). Decision recorded 2026-07-03 - see timebase.hpp and
    // docs/dma-timing-review-2026-07-03.md Finding 1. 44.1 kHz WAV content
    // is rate-converted at playback (PumpWavIO resampler for streaming/
    // audition; Voice::increment scaling for RAM-resident samples).
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    // Initialize DSP objects
    InitDSP(sdram_result == SdramHandle::Result::OK);
    WAVEX_LOG_DAISY(AUDIO_ENGINE, "DSP objects initialized");

    // Load the persisted CV calibration table (item 5 stage 4) - after
    // SD mount and engine init, before audio starts flushing CVs.
    if (sd_available) {
        WaveX::AudioEngine::LoadCvCalFromSd();
    }
#if WAVEX_DAISY_SFZ_BOOT_ENABLED
    // Narrow SFZ v1 trigger (docs/features/sfz-import.md): load the
    // conventional instrument into MIDI channel/slot 0 before audio starts.
    // Missing/invalid files are non-fatal; the engine keeps its existing
    // single-sample note route available.
    if (sd_available && sdram_result == SdramHandle::Result::OK) {
        WaveX::AudioEngine::LoadSfzInstrument(WAVEX_DAISY_SFZ_BOOT_PATH, 0);
    }
#endif
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

    // libDaisy's SdmmcHandler installs SDMMC1_IRQn at priority 0
    // (per/sdmmc.cpp:84) - ABOVE audio - and SD init runs earlier in this
    // function, so nothing had corrected it. Every SD transfer's interrupt
    // could therefore preempt the audio callback, inverting the §7.1.5
    // hierarchy (audio highest). Re-set below audio and above the SPI link.
    // Safe to demote: SDMMC1 moves data through its own IDMA, so this IRQ
    // only signals completion; a few microseconds of added latency cannot
    // overrun the FIFO.
    HAL_NVIC_SetPriority(SDMMC1_IRQn, 8, 0);

    // Set SPI DMA to lower priority than audio (higher number = lower priority)
    HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 10, 0);  // SPI1 DMA RX
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 10, 0);  // SPI1 DMA TX
    HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
    WAVEX_LOG_DAISY(INTER_MCU_LINK,
                    "Interrupt priorities configured: Audio DMA=5/6, SDMMC=8, SPI DMA=10");
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
    // dsy_dma_init installs UART DMA IRQs at priority 0. Restore the §7.1.5
    // hierarchy after the WaveX UART4 driver initializes both streams:
    // audio (5/6), UART full-duplex RX/TX (7), dormant SPI link (10).
    HAL_NVIC_SetPriority(UART4_IRQn, 7, 0);
    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 7, 0);
    HAL_NVIC_SetPriority(DMA2_Stream4_IRQn, 7, 0);
    WAVEX_LOG_DAISY(INTER_MCU_LINK,
                    "DAISY: UART full-duplex DMA started (IRQ priority 7, below audio)");

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
    uint32_t last_meter_send = System::GetNow();
#if WAVEX_PROFILING_ENABLED
    uint32_t last_profile_print = 0;
#endif
    bool wav_started = false;
    static uint32_t loop_counter = 0;

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

        loop_counter++;

        // Host adjusted the runtime log levels over CDC. Parse on the main
        // loop (log_ring context rules), reply through the ring so the host
        // script gets confirmation on the same port it sent the command.
#if WAVEX_DEBUG_HARNESS_ENABLED
        if (s_log_cmd_pending.load(std::memory_order_acquire)) {
            const char* cmd = s_log_cmd_buf;
            if (std::strncmp(cmd, "LOG ", 4) == 0) {
                const char* args = cmd + 4;
                if (args[0] == '?' && args[1] == '\0') {
                    for (size_t m = 0; m < WaveX::Log::kModuleCount; ++m) {
                        WaveX::Log::PrintLine("WAVEX-LOG: %s=%s",
                                              WaveX::Log::kModuleNames[m],
                                              WaveX::Log::kLevelNames[WaveX::Log::GetLevel(
                                                  static_cast<WaveX::Log::Module>(m))]);
                    }
                } else {
                    char reply[128];  // ApplyLevelCommand's longest reply is 117 bytes
                    WaveX::Log::ApplyLevelCommand(args, reply, sizeof(reply));
                    WaveX::Log::PrintLine("%s", reply);
                }
            } else if (std::strncmp(cmd, "FILTER", 6) == 0) {
                HandleFilterCommand(cmd + 6);
            } else if (std::strncmp(cmd, "ENTER-DFU", 9) != 0) {
                WaveX::Log::PrintLine("WAVEX: unknown command '%s'", cmd);
            }
            s_log_cmd_pending.store(false, std::memory_order_release);
        }
#endif

        // Host asked us to hand the USB port over to the bootloader. Do it
        // from the main loop, not the USB ISR, and give the log a moment to
        // drain over CDC before the port disappears.
        if (s_dfu_requested) {
            s_dfu_requested = false;
            WAVEX_LOG_DAISY(INTER_MCU_LINK, "DFU trigger received - rebooting to bootloader");
            System::Delay(50);
#if WAVEX_AUDIO_ENGINE_ENABLED
            hw.StopAudio();
#endif
            System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
        }

        uint32_t current_time = System::GetNow();

        // Process any incoming SPI messages from ESP32. The level-poll fallback
        // (for a missed ATTN edge) is not needed here: the GPIO interrupt
        // (EXTI15_10) catches the edge, and the ESP32 clears ATTN in
        // post_trans_cb, eliminating the race the fallback existed for.
#if WAVEX_SPI_LINK_ENABLED
        // The new, correct approach is to call a function that handles polling,
        // dequeuing, and processing in one step, avoiding the legacy conversion.
        WaveX::Comm::ProcessQueuedSpiMessage();
#endif

        WaveX::Comm::UartLinkProcess();

        // Push at most one USB packet of buffered log output. Non-blocking:
        // a busy or unread endpoint costs nothing here.
        WaveX::Log::Drain();

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
        // Debounced card-detect read; handles hot-swap without a reboot.
        WaveX::Storage::SdSdio::Poll();
#endif

#if WAVEX_AUDIO_ENGINE_ENABLED
        // The audio engine aborts playback itself when reads stop working;
        // the frontend has no way to learn that except by being told.
        if (WaveX::AudioEngine::TakePlaybackAborted()) {
            WaveX::Comm::NotifyStorageLost();
        }
#endif

// Check for audio underruns (logging handled here to avoid blocking audio callback)
#if WAVEX_AUDIO_ENGINE_ENABLED
        WaveX::AudioEngine::CheckAndLogUnderruns();
        // Stage A paraphonic CV: flush tick-staged DAC values (blocking
        // I2C ~225 us; §7.1.4-compliant on the main loop, item 5).
        WaveX::AudioEngine::FlushCv();
#endif

// Pump WAV I/O for audio playback (including audition)
#if WAVEX_AUDIO_ENGINE_ENABLED
        if (WaveX::AudioEngine::ShouldPumpWavIO()) {
            // Measure in ticks, convert once. GetTick() is the raw TIM2
            // counter (period 0xFFFFFFFF), so an unsigned delta stays exact
            // across its wrap. GetUs() is that counter divided by MHz, which
            // wraps at 2^32/200 = 21474836 - NOT a power of two - so
            // subtracting two GetUs() readings across a wrap yields garbage
            // (the "LONG I/O: 4273494187 us" line). Ticks count at PCLK1*2,
            // which is 240 MHz now that main() boosts SysClk to 480 MHz - and
            // was 200 MHz before it did. Hence GetTickFreq() below rather than
            // a constant: the rate is a consequence of the clock setup, not a
            // property of the board.
            // Hoisted: GetTickFreq() reaches HAL_RCC_GetSysClockFreq(),
            // which recomputes the PLL tree in floating point. Fine once,
            // wasteful on every pump - which is where I first put it.
            static const uint32_t io_ticks_per_us = System::GetTickFreq() / 1000000u;
            uint32_t io_start = System::GetTick();
            WaveX::AudioEngine::PumpWavIO();
            uint32_t io_duration = (System::GetTick() - io_start) / io_ticks_per_us;

            // Log long I/O operations that might cause audio pauses. A normal
            // pump with resampling costs 1.7-5 ms, so a 1 ms threshold fired
            // on every pump and the USB CDC write itself stole main-loop time
            // from the refill - the logging caused the underruns it was
            // reporting. Only flag genuine outliers.
            if (io_duration > 10000) {  // More than 10ms
                WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                "LONG I/O: %u us (might cause audio pause)",
                                (unsigned)io_duration);
            }
        }
#endif

// Advance any waveform envelope scan (roadmap 1.5.5), after the WAV pump:
// the refill is the thing with a deadline, so the envelope gets what is left
// over rather than competing for it. PumpEnvelopeJob is itself budgeted
// (~0.25 ms of SDRAM reads per pass) and returns immediately when no scan is
// in flight, which is the normal state.
#if WAVEX_AUDIO_ENGINE_ENABLED
        // SFZ inspection/loading is one bounded cooperative step per pass.
        // It follows the deadline-driven streaming refill; a LOAD closes the
        // audition first, while a lightweight PROBE yields between files.
        WaveX::AudioEngine::PumpInstrumentLoad();
        WaveX::AudioEngine::PumpEnvelopeJob();
        WaveX::AudioEngine::PumpPreviewSend();
        WaveX::AudioEngine::PumpTrackBinding();
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

        // Report I/O performance stats every 5 seconds - but only every 10th
        // report while a sample is playing. Each line is a blocking USB CDC
        // write on the same loop that has to refill the audio ring, so during
        // playback the reporting competes with the thing it reports on.
        static uint32_t last_stats_report = 0;
        static uint32_t stats_report_tick = 0;
        if (current_time - last_stats_report >= 5000) {
            last_stats_report = current_time;
            ++stats_report_tick;
#if WAVEX_AUDIO_ENGINE_ENABLED
            // Report every interval WHILE PLAYING and throttle to every 10th
            // when idle - the inverse of the first cut. Throttling during
            // playback produced zero stats for any audition shorter than
            // 50 s, i.e. no data exactly when the ring is under load and
            // something might be wrong. The block is 1-2 lines per interval;
            // the log volume that actually hurt was the per-pump LONG I/O
            // and per-episode underrun lines, both now rate-limited.
            const bool stats_throttled =
                !WaveX::AudioEngine::IsWavPlaying() && (stats_report_tick % 10 != 0);
            if (!stats_throttled) {
                uint32_t io_count, max_io_duration, last_io_duration;
                WaveX::AudioEngine::GetIOStats(io_count, max_io_duration, last_io_duration);
                // now/blocks give every other number a time base and say
                // which clock stopped. blocks advances at 1 kHz from the SAI
                // DMA interrupt, so over a nominal 5 s interval:
                //   dt ~5000, blocks ~5000 -> both healthy; a stall is
                //                             downstream (codec/analog)
                //   dt ~5000, blocks <<5000 -> audio callback starved: look
                //                              at IRQ priorities, not I/O
                //   dt >>5000              -> the main loop itself stalled
                static uint32_t last_blocks = 0;
                static uint32_t last_now = 0;
                const uint32_t now_ms = System::GetNow();
                const uint32_t blocks = WaveX::AudioEngine::GetCallbackBlocks();
                WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                "I/O Stats: count=%u, max=%u ticks, last=%u ticks | "
                                "now=%lu dt=%lu blocks=+%lu cpu=%d%% logdrop=%lu",
                                (unsigned)io_count,
                                (unsigned)max_io_duration,
                                (unsigned)last_io_duration,
                                (unsigned long)now_ms,
                                (unsigned long)(now_ms - last_now),
                                (unsigned long)(blocks - last_blocks),
                                (int)(WaveX::AudioEngine::GetAvgCpuLoad() * 100.0f),
                                (unsigned long)WaveX::Log::DroppedBytes());

                // How close the ring came to empty this interval. 2048 frames
                // is full (~42 ms); a low figure with no underruns means the
                // margin is being eaten periodically, which is audible long
                // before it ever reaches zero.
                //
                // TakeRingLowWater()/TakeIOThroughput() are destructive
                // reads (they reset the interval on every call). DiagPushTick
                // drains the same counters whenever a diagnostics client is
                // subscribed, and two destructive consumers on independent
                // timers silently steal each other's window - the low-water
                // figure this block reports can read "healthy" solely
                // because diag_push already claimed the real dip a moment
                // earlier. Cede ownership to diag_push while it is active
                // rather than fight it over the same counters.
                const bool diag_owns_counters = WaveX::Comm::DiagIsSubscribed();
                if (!diag_owns_counters) {
                    WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                    "RING: low_water=%lu of %u frames",
                                    (unsigned long)WaveX::AudioEngine::TakeRingLowWater(),
                                    2048u);
                } else {
                    WAVEX_LOG_DAISY(AUDIO_ENGINE, "RING: low_water owned by diag subscriber");
                }

#if WAVEX_DAISY_SD_DEBUG
                // Throughput/latency for THIS interval. Rates are derived from
                // the measured dt rather than the nominal 5 s, so a late
                // report does not read as a throughput drop. Computed
                // unconditionally (not just when this block owns the
                // counters): the WAVEX_DAISY_UART_PERF_DEBUG block further
                // down reuses dt_ms and needs it in scope either way.
                const uint32_t dt_ms = (now_ms - last_now) ? (now_ms - last_now) : 1u;
                if (!diag_owns_counters) {
                    uint32_t sd_bytes, sd_reads, sd_avg_us, sd_min_us, sd_max_us;
                    WaveX::AudioEngine::TakeIOThroughput(
                        sd_bytes, sd_reads, sd_avg_us, sd_min_us, sd_max_us);
                    WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                    "SD PERF: %lu KB/s (%lu reads, %lu B) latency avg=%lu us "
                                    "min=%lu us max=%lu us @ %s",
                                    (unsigned long)((uint64_t)sd_bytes * 1000u / dt_ms / 1024u),
                                    (unsigned long)sd_reads,
                                    (unsigned long)sd_bytes,
                                    (unsigned long)sd_avg_us,
                                    (unsigned long)sd_min_us,
                                    (unsigned long)sd_max_us,
                                    WaveX::Storage::SdSdio::CurrentSpeedName());
                } else {
                    WAVEX_LOG_DAISY(AUDIO_ENGINE, "SD PERF: owned by diag subscriber");
                }
#endif

                uint32_t io_errors = 0, io_last_err = 0;
                WaveX::AudioEngine::GetIOErrors(io_errors, io_last_err);
                if (io_errors > 0) {
                    WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                    "SD ERRORS: %lu failed reads, last FatFS result=%lu",
                                    (unsigned long)io_errors,
                                    (unsigned long)io_last_err);
                }

                // Output level, so a healthy-looking clock can be told apart
                // from a healthy clock emitting silence. blocks advancing with
                // peak at zero means the DSP is producing nothing (a data
                // problem); peak non-zero while audio is inaudible puts the
                // fault after the DSP - codec, SAI, or analog.
                WaveX::AudioEngine::BlockMeters lvl;
                WaveX::AudioEngine::GetMeters(lvl);
                WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                "LEVEL: peakL=%d peakR=%d rmsL=%d rmsR=%d (x1000)",
                                (int)(lvl.peakL * 1000.0f),
                                (int)(lvl.peakR * 1000.0f),
                                (int)(lvl.rmsL * 1000.0f),
                                (int)(lvl.rmsR * 1000.0f));
                last_blocks = blocks;
                last_now = now_ms;

#if WAVEX_DAISY_STREAM_DEBUG
                // Where the chain stops when an audition goes silent:
                //   playing=1 prebuf=0        -> stuck pre-buffering (SD/convert)
                //   playing=1 prebuf=1 peak=0 -> data reaches the ring as silence
                //   playing=1 prebuf=1 peak>0 -> DSP is fine; look at codec/analog
                WaveX::AudioEngine::BlockMeters dbg_meters;
                WaveX::AudioEngine::GetMeters(dbg_meters);
                uint32_t dbg_filled, dbg_target, dbg_sr;
                uint8_t dbg_ch, dbg_bits;
                WaveX::AudioEngine::GetStreamDebug(
                    dbg_filled, dbg_target, dbg_sr, dbg_ch, dbg_bits);
                WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                "STREAM: playing=%d prebuf=%u/%u peakL=%d peakR=%d (x1000) "
                                "wav=%luHz ch=%u bits=%u",
                                (int)WaveX::AudioEngine::IsWavPlaying(),
                                (unsigned)dbg_filled,
                                (unsigned)dbg_target,
                                (int)(dbg_meters.peakL * 1000.0f),
                                (int)(dbg_meters.peakR * 1000.0f),
                                (unsigned long)dbg_sr,
                                (unsigned)dbg_ch,
                                (unsigned)dbg_bits);
                uint32_t dbg_free, dbg_want, dbg_res, dbg_pushes;
                WaveX::AudioEngine::GetStreamDiscardDebug(dbg_free, dbg_want, dbg_res, dbg_pushes);
                WAVEX_LOG_DAISY(AUDIO_ENGINE,
                                "STREAM2: free=%u want=%u resampled=%u pushes=%u",
                                (unsigned)dbg_free,
                                (unsigned)dbg_want,
                                (unsigned)dbg_res,
                                (unsigned)dbg_pushes);

#endif  // WAVEX_DAISY_STREAM_DEBUG

#if WAVEX_DAISY_UART_PERF_DEBUG
                // total_us against the interval says what fraction of the
                // main loop the link consumed; max_us says whether any single
                // pass was long enough to matter to the ring refill.
                WaveX::Comm::UartPerfSample uart_perf;
                WaveX::Comm::TakeUartPerf(uart_perf);
                WAVEX_LOG_DAISY(INTER_MCU_LINK,
                                "UART PERF: %lu calls, %lu us total (%lu.%02lu%% of loop) "
                                "avg=%lu us max=%lu us | rx %lu B/%lu fr, tx %lu B/%lu fr | "
                                "err=%lu seqdrop=%lu ovf=%lu",
                                (unsigned long)uart_perf.calls,
                                (unsigned long)uart_perf.total_us,
                                (unsigned long)(uart_perf.total_us / (dt_ms * 10u)),
                                (unsigned long)((uart_perf.total_us * 100u / (dt_ms * 10u)) % 100u),
                                (unsigned long)uart_perf.avg_us,
                                (unsigned long)uart_perf.max_us,
                                (unsigned long)uart_perf.rx_bytes,
                                (unsigned long)uart_perf.rx_frames,
                                (unsigned long)uart_perf.tx_bytes,
                                (unsigned long)uart_perf.tx_frames,
                                (unsigned long)uart_perf.errors,
                                (unsigned long)uart_perf.seq_drops,
                                (unsigned long)uart_perf.queue_overflows);
#endif

                WaveX::Comm::UartLinkLogStats();
            }  // !stats_throttled
#else
            // Log UART stats
            WaveX::Comm::UartLinkLogStats();
#endif
        }

        if (send_beacon) {
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
            (void)heartbeat_result;  // read only by the packet-debug log below
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

        // Diagnostics telemetry. No-op unless the frontend has subscribed, so
        // this costs one comparison per pass while the page is closed.
        WaveX::Comm::DiagPushTick(current_time);

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

        // Playback position, at a fifth of the meter rate. A progress bar does
        // not need 20-50 Hz, and this shares the audition's send budget with
        // the meters rather than adding a second periodic sender.
        static uint32_t s_pos_divider = 0;
        if (should_send_meters && (++s_pos_divider % 5u) == 0u) {
            uint32_t played = 0, region = 0;
            if (WaveX::AudioEngine::GetPlaybackPosition(played, region) && region > 0) {
                WaveX::Protocol::SampleStatusMessage pos{};
                pos.state = 1;  // playing
                pos.frames_played = played;
                pos.sample_rate = region;  // region length, so the UI can scale
                WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_STATUS, &pos, sizeof(pos));
            }
        }

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
            (void)result;  // read only by the packet-debug log below
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
        // Repeated rather than one-shot: after a DFU reset the serial logger
        // takes a few seconds to reattach, and a single boot-time run scrolls
        // past uncaptured. Every 20 s is cheap and guarantees a capture. Bench
        // build only.
        static uint32_t last_registry_bench = 0;
        if (current_time - last_registry_bench >= 20000) {
            last_registry_bench = current_time;
            WaveX::AudioEngine::BenchmarkRegistryScan();
        }
        if (current_time - last_profile_print >= 5000) {
            PrintProfilingStats(hw);
            WaveX::Profiling::Profiler::ResetAll();
            last_profile_print = current_time;
        }
#endif
    }
}
