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
bool s_mounted = false;
daisy::DaisySeed* s_hw = nullptr;
bool s_auto_format = false;
CardEventCallback s_card_cb = nullptr;

#if WAVEX_DAISY_SD_CARD_DETECT_PIN >= 0
// Card-detect is a mechanical switch, so it bounces on insertion and
// removal. Require the level to hold for this long before acting - acting on
// a bounce would tear down a working mount or try to mount a card that is
// still moving in its socket.
constexpr uint32_t kCardDebounceMs = 250;
bool s_card_present = false;
bool s_pending_level = false;
uint32_t s_pending_since_ms = 0;
// A card can report ready before it is actually readable, so a remount that
// fails is retried rather than being abandoned until the next physical
// insertion - otherwise one unlucky attempt leaves the slot dead.
uint32_t s_remount_retry_after_ms = 0;
constexpr uint32_t kRemountRetryMs = 2000;

// Card-detect closes before the card is electrically ready; this settle
// window covers power-up, not switch bounce (kCardDebounceMs above already
// covers that). Re-identification then walks the speed table one entry per
// Poll() call instead of in one shot: the previous single-shot version could
// block the main loop (audio ring/CV/UART all serviced from the same loop)
// for the settle delay plus every speed's own FR_NOT_READY retry loop -
// worst case around 1.5 s. Spreading it out bounds any one Poll() call to a
// single TrySpeed() attempt's own worst case (~250 ms from its internal
// retry loop, left untouched - see TrySpeed's comments on why that
// sequencing is hardware-sensitive) instead of the full negotiation.
enum class ReinitState { Idle, AwaitingSettle, Negotiating };
ReinitState s_reinit_state = ReinitState::Idle;
constexpr uint32_t kInsertSettleMs = 200;
uint32_t s_settle_since_ms = 0;
int s_negotiate_index = 0;
#endif

// Applies one bus clock and proves it by mounting and reading a directory.
// A mount that succeeds but cannot be read is exactly what a marginal clock
// looks like, so the read is part of the test rather than a separate step.
// s_sd_brought_up tracks whether the SDMMC peripheral has ever been
// initialized. The teardown below must NOT run before that: HAL_SD_DeInit()
// on a handle that was never initialized invokes HAL_SD_MspDeInit and tears
// down clocks and GPIOs that libDaisy only ever configures from
// HAL_SD_MspInit inside HAL_SD_Init - which runs later, at first disk
// access. Doing it on the first attempt left the interface unusable at every
// speed, which is what "no card opens any more" was.
bool s_sd_brought_up = false;
// FATFS_LinkDriver() claims a slot in a fixed-size global volume table and is
// NOT idempotent, so calling FatFSInterface::Init() per attempt leaked a slot
// each time and quickly returned ERR_TOO_MANY_VOLUMES - the "FatFS link
// failed" on the second and third speeds. The link is independent of the
// mount (unmounting does not release it), so it is done exactly once.
bool s_fs_linked = false;

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

    // Only tear down when there is something to tear down. On the very first
    // bring-up this path is skipped entirely, so boot follows exactly the
    // sequence that worked before negotiation existed.
    if (s_sd_brought_up) {
        f_mount(nullptr, "/", 0);
        HAL_SD_DeInit(&hsd1);  // also returns hsd1.State to RESET
    }
    s_sd_brought_up = true;

    if (s_sdmmc.Init(sd_cfg) != SdmmcHandler::Result::OK) {
        WaveX::Log::PrintLine("SD: SDMMC init FAILED at %s", kSpeeds[index].name);
        return false;
    }

    // Bring the card up here rather than leaving it to FatFS, because
    // SD_initialize() (sd_diskio.c:106) hides the failure:
    //
    //     if (BSP_SD_Init() == MSD_OK) { Stat = SD_CheckStatus(lun); }
    //     return Stat;                 // unchanged when BSP_SD_Init FAILED
    //
    // Stat is a file static that still says "ready" from the successful boot,
    // so a failed re-init returns ready anyway, FatFS skips its STA_NOINIT
    // check, and reads go to an uninitialized peripheral. Every bus clock
    // then reports FR_DISK_ERR and the real cause is never visible - which is
    // exactly what re-inserting a card produced. Doing it explicitly means a
    // failure aborts this attempt and names itself.
    HAL_StatusTypeDef hal = HAL_SD_Init(&hsd1);
    if (hal != HAL_OK) {
        WaveX::Log::PrintLine("SD: HAL_SD_Init failed at %s (hal=%d err=0x%08lX state=%u)",
                              kSpeeds[index].name,
                              (int)hal,
                              (unsigned long)HAL_SD_GetError(&hsd1),
                              (unsigned)HAL_SD_GetCardState(&hsd1));
        return false;
    }
    hal = HAL_SD_ConfigWideBusOperation(&hsd1, hsd1.Init.BusWide);
    if (hal != HAL_OK) {
        WaveX::Log::PrintLine("SD: bus-width config failed at %s (hal=%d err=0x%08lX)",
                              kSpeeds[index].name,
                              (int)hal,
                              (unsigned long)HAL_SD_GetError(&hsd1));
        return false;
    }

    if (!s_fs_linked) {
        FatFSInterface::Config fcfg{};
        fcfg.media = FatFSInterface::Config::MEDIA_SD;
        if (s_fsi.Init(fcfg) != FatFSInterface::Result::OK) {
            WaveX::Log::PrintLine("SD: FatFS link failed at %s", kSpeeds[index].name);
            return false;
        }
        s_fs_linked = true;
    }

    FATFS& fs = s_fsi.GetSDFileSystem();
    if (f_mount(&fs, "/", 0) != FR_OK) {
        return false;
    }

    // Report what the card actually is. Capacity decides whether FR_NO_FILESYSTEM
    // means "corrupt" or simply "exFAT": this FatFS build has _FS_EXFAT 0, and
    // anything over 32 GB is exFAT out of the box (SDXC), so it can never mount
    // regardless of bus clock. Card info is only valid once HAL_SD_Init has run,
    // which the mount above triggers.
    {
        HAL_SD_CardInfoTypeDef info{};
        if (HAL_SD_GetCardInfo(&hsd1, &info) == HAL_OK && info.LogBlockNbr > 0) {
            const uint32_t mib = (uint32_t)(((uint64_t)info.LogBlockNbr * info.LogBlockSize) >> 20);
            WaveX::Log::PrintLine("SD: card type=%lu capacity=%lu MiB (%lu blocks x %lu B)",
                                  (unsigned long)info.CardType,
                                  (unsigned long)mib,
                                  (unsigned long)info.LogBlockNbr,
                                  (unsigned long)info.LogBlockSize);
            if (mib > 32768u) {
                WaveX::Log::PrintLine(
                    "SD: >32 GiB card - if this reports FR_NO_FILESYSTEM (13) it is almost "
                    "certainly exFAT, which this FatFS build cannot read. Reformat as FAT32.");
            }
        }
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
            s_mounted = true;
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
            s_mounted = true;
            WaveX::Log::PrintLine("SD: mounted at %s (after format)", kSpeeds[index].name);
            return true;
        }
    }

    const char* why = "";
    if (fr == FR_NO_FILESYSTEM) {
        why = " (FR_NO_FILESYSTEM - not FAT12/16/32; exFAT is not supported by this build)";
    } else if (fr == FR_DISK_ERR) {
        why = " (FR_DISK_ERR - the read itself failed; see hal_err on the next read failure)";
    } else if (fr == FR_NOT_READY) {
        why = " (FR_NOT_READY - card did not become ready in time)";
    }
    WaveX::Log::PrintLine("SD: %s unusable (FatFS result %d)%s", kSpeeds[index].name, (int)fr, why);
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

bool IsMounted() {
    return s_mounted;
}

void SetCardEventCallback(CardEventCallback cb) {
    s_card_cb = cb;
}

void Poll() {
#if WAVEX_DAISY_SD_CARD_DETECT_PIN >= 0
    if (s_hw == nullptr) {
        return;
    }

    // Drive an in-progress reinsertion sequence first, one bounded step per
    // call, instead of blocking the whole main loop until the card is fully
    // renegotiated. Removal can't be observed while this is running (same as
    // the previous single-shot version - TrySpeed() will simply fail at
    // every speed if the card is actually gone, and the sequence below gives
    // up gracefully after the last one).
    if (s_reinit_state == ReinitState::AwaitingSettle) {
        if ((System::GetNow() - s_settle_since_ms) < kInsertSettleMs) {
            return;
        }
        s_reinit_state = ReinitState::Negotiating;
        s_negotiate_index = static_cast<int>(WAVEX_DAISY_SD_CARD_SPEED);
    }
    if (s_reinit_state == ReinitState::Negotiating) {
        if (TrySpeed(s_negotiate_index, s_auto_format)) {
            if (s_negotiate_index != static_cast<int>(WAVEX_DAISY_SD_CARD_SPEED)) {
                WaveX::Log::PrintLine(
                    "SD: negotiated DOWN from %s to %s - the card or wiring cannot hold the "
                    "configured rate",
                    kSpeeds[static_cast<int>(WAVEX_DAISY_SD_CARD_SPEED)].name,
                    kSpeeds[s_negotiate_index].name);
            }
            s_reinit_state = ReinitState::Idle;
            s_remount_retry_after_ms = 0;
            if (s_card_cb) {
                s_card_cb(true);
            }
        } else if (--s_negotiate_index < 0) {
            WaveX::Log::PrintLine("SD: unusable at every bus clock");
            s_reinit_state = ReinitState::Idle;
            // Leave the state as "absent" so the still-present card reads as
            // a fresh insertion next time round and the attempt repeats,
            // spaced out so a card that never mounts does not spin the loop
            // or the log.
            s_card_present = false;
            s_remount_retry_after_ms = System::GetNow() + kRemountRetryMs;
            WaveX::Log::PrintLine("SD: remount FAILED - retrying in %lu ms",
                                  (unsigned long)kRemountRetryMs);
        }
        // Else: still negotiating, try s_negotiate_index (now one step lower)
        // on the next Poll() call.
        return;
    }

    // Active low: LOW means a card is seated.
    const bool present_now = !s_cd_pin.Read();
    const uint32_t now = System::GetNow();

    if (present_now != s_pending_level) {
        s_pending_level = present_now;
        s_pending_since_ms = now;
        return;
    }
    if (present_now == s_card_present) {
        return;  // already settled in this state
    }
    if ((now - s_pending_since_ms) < kCardDebounceMs) {
        return;  // still bouncing
    }
    if (s_remount_retry_after_ms != 0 && now < s_remount_retry_after_ms) {
        return;  // waiting out a failed remount
    }

    s_card_present = present_now;
    if (!present_now) {
        // Unmount FIRST, then notify: a handler must never touch a file on a
        // card that is physically gone.
        f_mount(nullptr, "/", 0);
        s_mounted = false;
        WaveX::Log::PrintLine("SD: card REMOVED - filesystem unmounted");
        if (s_card_cb) {
            s_card_cb(false);
        }
        return;
    }

    WaveX::Log::PrintLine("SD: card INSERTED - remounting");
    // Card-detect closes before the card is electrically ready; this settle
    // window covers power-up, not switch bounce (kCardDebounceMs above
    // already covers that). Handed off to the state machine at the top of
    // this function instead of blocking here - see its declaration comment.
    s_reinit_state = ReinitState::AwaitingSettle;
    s_settle_since_ms = now;
#endif
}

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

int CurrentSpeedIndex() {
    return s_speed_index;
}

const char* CurrentSpeedName() {
    return kSpeeds[s_speed_index].name;
}

bool InitAndMount(DaisySeed& hw, bool auto_format) {
    s_hw = &hw;
    s_auto_format = auto_format;
// Check for Card Detect pin if configured
#if WAVEX_DAISY_SD_CARD_DETECT_PIN >= 0
    // Initialize Card Detect pin (active low - card present when pin reads LOW)
    s_cd_pin.Init(hw.GetPin(WAVEX_DAISY_SD_CARD_DETECT_PIN), GPIO::Mode::INPUT, GPIO::Pull::PULLUP);

    if (s_cd_pin.Read())  // HIGH = no card (pulled up)
    {
        WaveX::Log::PrintLine("SD: No card detected (CD pin D%d HIGH)",
                              WAVEX_DAISY_SD_CARD_DETECT_PIN);
        return false;
    }
    WaveX::Log::PrintLine("SD: Card detected (CD pin D%d LOW)", WAVEX_DAISY_SD_CARD_DETECT_PIN);
    s_card_present = true;
    s_pending_level = true;
    s_pending_since_ms = System::GetNow();
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

            if (test_fr == FR_OK) {
                test_success = true;
                break;
            }

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
