#include "sd_sdio.h"

#include "comm/log_ring.h"

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)

#include "fatfs.h"
#include "ff.h"
#include "per/gpio.h"
#include "per/sdmmc.h"
#include "sys/system.h"

extern "C" SD_HandleTypeDef hsd1;  // libDaisy per/sdmmc.cpp

using namespace daisy;

namespace WaveX {
namespace Storage {
namespace SdSdio {

static SdmmcHandler s_sdmmc;
static FatFSInterface s_fsi;
static GPIO s_cd_pin;

namespace {

struct SpeedEntry {
    SdmmcHandler::Speed speed;
    const char* name;
};

// Index order matches WAVEX_DAISY_SD_CARD_SPEED (0..4).
constexpr SpeedEntry kSpeeds[] = {
    {SdmmcHandler::Speed::SLOW, "SLOW/400kHz"},
    {SdmmcHandler::Speed::MEDIUM_SLOW, "MEDIUM_SLOW/12.5MHz"},
    {SdmmcHandler::Speed::STANDARD, "STANDARD/25MHz"},
    {SdmmcHandler::Speed::FAST, "FAST/50MHz"},
    {SdmmcHandler::Speed::VERY_FAST, "VERY_FAST/100MHz"},
};
constexpr int kSpeedCount = static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

static_assert(WAVEX_DAISY_SD_CARD_SPEED >= 0 && WAVEX_DAISY_SD_CARD_SPEED < kSpeedCount,
              "WAVEX_DAISY_SD_CARD_SPEED must be 0..4 (SdmmcHandler::Speed)");

int s_speed_index = WAVEX_DAISY_SD_CARD_SPEED;

// Applies one bus clock and proves it by mounting and reading a directory.
// A mount that succeeds but cannot be read is exactly what a marginal clock
// looks like, so the read is part of the test rather than a separate step.
bool TrySpeed(int index, bool auto_format) {
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    sd_cfg.speed = kSpeeds[index].speed;
    sd_cfg.width = (WAVEX_DAISY_SD_CARD_BUS_WIDTH == 1) ? SdmmcHandler::BusWidth::BITS_1
                                                        : SdmmcHandler::BusWidth::BITS_4;
    sd_cfg.clock_powersave = false;

    WaveX::Log::PrintLine("SD: trying %s, %s",
                          kSpeeds[index].name,
                          WAVEX_DAISY_SD_CARD_BUS_WIDTH == 4 ? "4-bit" : "1-bit");

    // Drop any previous mount and card state so the new clock is applied from
    // a clean start; HAL_SD_Init runs again on the next disk access.
    f_mount(nullptr, "/", 0);
    HAL_SD_DeInit(&hsd1);

    if (s_sdmmc.Init(sd_cfg) != SdmmcHandler::Result::OK) {
        WaveX::Log::PrintLine("SD: SDMMC init FAILED at %s", kSpeeds[index].name);
        return false;
    }

    FatFSInterface::Config fcfg{};
    fcfg.media = FatFSInterface::Config::MEDIA_SD;
    if (s_fsi.Init(fcfg) != FatFSInterface::Result::OK) {
        WaveX::Log::PrintLine("SD: FatFS link failed at %s", kSpeeds[index].name);
        return false;
    }

    FATFS& fs = s_fsi.GetSDFileSystem();
    if (f_mount(&fs, "/", 0) != FR_OK) {
        return false;
    }

    // Exercise the bus. Delayed mount means this is the first real access, so
    // it is also where a clock the card cannot hold will fail.
    DIR dir;
    FRESULT fr = FR_NOT_READY;
    for (int retry = 0; retry < 5; ++retry) {
        fr = f_opendir(&dir, "/");
        if (fr == FR_OK) {
            f_closedir(&dir);
            s_speed_index = index;
            WaveX::Log::PrintLine("SD: mounted at %s", kSpeeds[index].name);
            return true;
        }
        if (fr != FR_NOT_READY) {
            break;
        }
        System::Delay(50);
    }

    if (fr == FR_NO_FILESYSTEM && auto_format) {
        WaveX::Log::PrintLine("SD: No filesystem detected; formatting...");
        static BYTE workbuf[4096];
        if (f_mkfs("/", FM_FAT | FM_SFD, 0, workbuf, sizeof(workbuf)) == FR_OK &&
            f_opendir(&dir, "/") == FR_OK) {
            f_closedir(&dir);
            s_speed_index = index;
            WaveX::Log::PrintLine("SD: mounted at %s (after format)", kSpeeds[index].name);
            return true;
        }
    }

    WaveX::Log::PrintLine("SD: %s unusable (FatFS result %d)", kSpeeds[index].name, (int)fr);
    return false;
}

// Walks down from `start_index` to the slowest rate.
bool ConfigureAndMount(int start_index, bool auto_format) {
    for (int i = start_index; i >= 0; --i) {
        if (TrySpeed(i, auto_format)) {
            if (i != start_index) {
                WaveX::Log::PrintLine(
                    "SD: negotiated DOWN from %s to %s - the card or wiring "
                    "cannot hold the configured rate",
                    kSpeeds[start_index].name,
                    kSpeeds[i].name);
            }
            return true;
        }
    }
    WaveX::Log::PrintLine("SD: unusable at every bus clock");
    return false;
}

}  // namespace

bool DowngradeSpeed() {
    if (s_speed_index <= 0) {
        WaveX::Log::PrintLine("SD: already at %s; cannot go slower", kSpeeds[0].name);
        return false;
    }
    const int target = s_speed_index - 1;
    WaveX::Log::PrintLine("SD: downgrading %s -> %s after read errors",
                          kSpeeds[s_speed_index].name,
                          kSpeeds[target].name);
    return TrySpeed(target, false);
}

const char* CurrentSpeedName() {
    return kSpeeds[s_speed_index].name;
}

bool InitAndMount(DaisySeed& hw, bool auto_format) {
// Check for Card Detect pin if configured
#if WAVEX_DAISY_SD_CARD_DETECT_PIN >= 0
    // Initialize Card Detect pin (active low - card present when pin reads LOW)
    s_cd_pin.Init(hw.GetPin(WAVEX_DAISY_SD_CARD_DETECT_PIN), GPIO::Mode::INPUT, GPIO::Pull::PULLUP);

    // Check if SD card is physically present
    if (s_cd_pin.Read())  // HIGH = no card (pulled up)
    {
        WaveX::Log::PrintLine("SD: No card detected (CD pin D%d HIGH)",
                              WAVEX_DAISY_SD_CARD_DETECT_PIN);
        return false;
    }
    WaveX::Log::PrintLine("SD: Card detected (CD pin D%d LOW)", WAVEX_DAISY_SD_CARD_DETECT_PIN);
#else
    WaveX::Log::PrintLine("SD: Card detect disabled - assuming card present");
#endif

    // Bus clocks, fastest first. Negotiation walks DOWN this list: a card or
    // harness that cannot hold the configured rate settles on the fastest one
    // it can, rather than failing outright or being pinned low for everyone.
    // Figures derived in hardware_config.h.
    if (!ConfigureAndMount(static_cast<int>(WAVEX_DAISY_SD_CARD_SPEED), auto_format)) {
        return false;
    }

    FATFS& fs = s_fsi.GetSDFileSystem();
    // Use delayed mount (0) as per libDaisy standard - mount happens on first filesystem access
    WaveX::Log::PrintLine("SD: Mounting filesystem (delayed mount)...");

    FRESULT fr = f_mount(&fs, "/", 0);
    WaveX::Log::PrintLine("SD: Mount setup result: %d", (int)fr);

    if (fr == FR_OK) {
        WaveX::Log::PrintLine("SD: Mount setup successful");

        // Test actual filesystem access (this triggers the delayed mount)
        // With delayed mount, the first access may return FR_NOT_READY if the disk
        // hasn't finished initializing yet. Retry with small delays to handle this.
        WaveX::Log::PrintLine("SD: Testing filesystem access...");
        DIR dir;
        FRESULT test_fr;
        const int max_retries = 5;
        const int retry_delay_ms = 50;
        bool test_success = false;

        for (int retry = 0; retry < max_retries; retry++) {
            test_fr = f_opendir(&dir, "/");

            // If successful, break out of retry loop
            if (test_fr == FR_OK) {
                test_success = true;
                break;
            }

            // If it's a "not ready" error and we haven't exhausted retries, wait and retry
            if (test_fr == FR_NOT_READY && retry < max_retries - 1) {
                WaveX::Log::PrintLine("SD: Drive not ready, retrying in %d ms (attempt %d/%d)...",
                                      retry_delay_ms,
                                      retry + 1,
                                      max_retries);
                System::Delay(retry_delay_ms);
                continue;
            }

            // For other errors or last retry, break and handle below
            break;
        }

        // Handle the actual mount result
        if (!test_success && test_fr == FR_NO_FILESYSTEM && auto_format) {
            WaveX::Log::PrintLine("SD: No filesystem detected; formatting...");
            static BYTE workbuf[4096];
            test_fr = f_mkfs("/", FM_FAT | FM_SFD, 0, workbuf, sizeof(workbuf));
            WaveX::Log::PrintLine("SD: Format result: %d", (int)test_fr);
            if (test_fr == FR_OK) {
                test_fr = f_opendir(&dir, "/");  // Try again after format
                WaveX::Log::PrintLine("SD: Re-test after format result: %d", (int)test_fr);
                test_success = (test_fr == FR_OK);
            }
        }

        if (test_success) {
            f_closedir(&dir);
            WaveX::Log::PrintLine("SD: Filesystem access successful - SD card ready");
            return true;
        } else {
            // Provide detailed error information
            const char* error_msg = "Unknown error";
            switch (test_fr) {
                case FR_NO_FILE:
                    error_msg = "No file";
                    break;
                case FR_NO_PATH:
                    error_msg = "No path";
                    break;
                case FR_INVALID_NAME:
                    error_msg = "Invalid name";
                    break;
                case FR_DENIED:
                    error_msg = "Access denied";
                    break;
                case FR_NOT_READY:
                    error_msg = "Drive not ready";
                    break;
                case FR_WRITE_PROTECTED:
                    error_msg = "Write protected";
                    break;
                case FR_DISK_ERR:
                    error_msg = "Disk error";
                    break;
                case FR_INT_ERR:
                    error_msg = "Internal error";
                    break;
                case FR_NOT_ENABLED:
                    error_msg = "Not enabled";
                    break;
                case FR_NO_FILESYSTEM:
                    error_msg = "No filesystem";
                    break;
                default:
                    break;
            }

            // Special handling for FR_NOT_READY: Since mount setup was successful and we're
            // using delayed mount, the filesystem will be mounted on first actual access.
            // This timing issue shouldn't fail initialization.
            if (test_fr == FR_NOT_READY) {
                WaveX::Log::PrintLine(
                    "SD: Initial access test failed (drive not ready after %d retries)",
                    max_retries);
                WaveX::Log::PrintLine(
                    "SD: Mount setup was successful - delayed mount will work on first actual "
                    "access");
                WaveX::Log::PrintLine(
                    "SD: SD card initialization completed (may need moment to stabilize)");
                return true;  // Return true since mount setup succeeded
            }

            // For other errors, fail initialization
            WaveX::Log::PrintLine("SD: Filesystem access failed after %d retries - %s (%d)",
                                  max_retries,
                                  error_msg,
                                  (int)test_fr);
            return false;
        }
    } else {
        WaveX::Log::PrintLine("SD: Mount setup failed: %d", (int)fr);
        return false;
    }
}

}  // namespace SdSdio
}  // namespace Storage
}  // namespace WaveX

#endif  // WAVEX_DAISY_SD_CARD_ENABLED && WAVEX_DAISY_SD_CARD_BACKEND == 1
