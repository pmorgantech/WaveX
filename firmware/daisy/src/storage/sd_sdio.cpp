#include "sd_sdio.h"

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)

#include "per/sdmmc.h"
#include "fatfs.h"
#include "ff.h"
#include "per/gpio.h"

using namespace daisy;

namespace WaveX {
namespace Storage {
namespace SdSdio {

static SdmmcHandler s_sdmmc;
static FatFSInterface s_fsi;
static GPIO s_cd_pin;

bool InitAndMount(DaisySeed& hw, bool auto_format)
{
    // Check for Card Detect pin if configured
    #if WAVEX_DAISY_SD_CARD_DETECT_PIN >= 0
    // Initialize Card Detect pin (active low - card present when pin reads LOW)
    s_cd_pin.Init(hw.GetPin(WAVEX_DAISY_SD_CARD_DETECT_PIN), GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    
    // Check if SD card is physically present
    if(s_cd_pin.Read()) // HIGH = no card (pulled up)
    {
        hw.PrintLine("SD: No card detected (CD pin D%d HIGH)", WAVEX_DAISY_SD_CARD_DETECT_PIN);
        return false;
    }
    hw.PrintLine("SD: Card detected (CD pin D%d LOW)", WAVEX_DAISY_SD_CARD_DETECT_PIN);
    #else
    hw.PrintLine("SD: Card detect disabled - assuming card present");
    #endif

    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    
    // Configure speed based on macro
    switch(WAVEX_DAISY_SD_CARD_SPEED) {
        case 0: sd_cfg.speed = SdmmcHandler::Speed::SLOW; break;
        case 1: sd_cfg.speed = SdmmcHandler::Speed::MEDIUM_SLOW; break;
        case 2: sd_cfg.speed = SdmmcHandler::Speed::STANDARD; break;
        case 3: sd_cfg.speed = SdmmcHandler::Speed::FAST; break;
        default: sd_cfg.speed = SdmmcHandler::Speed::STANDARD; break;
    }
    
    // Configure bus width based on macro
    sd_cfg.width = (WAVEX_DAISY_SD_CARD_BUS_WIDTH == 1) ? 
                   SdmmcHandler::BusWidth::BITS_1 : 
                   SdmmcHandler::BusWidth::BITS_4;
    
    sd_cfg.clock_powersave = false;
    
    const char* speed_names[] = {"SLOW", "MEDIUM_SLOW", "STANDARD", "FAST"};
    const char* width_names[] = {"1-bit", "4-bit"};
    hw.PrintLine("SD: Configuring SDMMC - Speed: %s, Width: %s", 
                 speed_names[WAVEX_DAISY_SD_CARD_SPEED], 
                 width_names[WAVEX_DAISY_SD_CARD_BUS_WIDTH == 4 ? 1 : 0]);

    if(s_sdmmc.Init(sd_cfg) != SdmmcHandler::Result::OK)
    {
        hw.PrintLine("SD: SDMMC init FAILED");
        return false;
    }
    hw.PrintLine("SD: SDMMC init OK (4-bit, STANDARD)");

    FatFSInterface::Config fcfg{};
    fcfg.media = FatFSInterface::Config::MEDIA_SD;
    if(s_fsi.Init(fcfg) != FatFSInterface::Result::OK)
    {
        hw.PrintLine("SD: FatFS link failed");
        return false;
    }
    hw.PrintLine("SD: FatFS interface initialized successfully");

    // Try to get some card information before mounting
    hw.PrintLine("SD: Checking SDMMC status before mount...");
    
    FATFS& fs = s_fsi.GetSDFileSystem();
    // Use delayed mount (0) as per libDaisy standard - mount happens on first filesystem access
    hw.PrintLine("SD: Mounting filesystem (delayed mount)...");
    
    FRESULT fr = f_mount(&fs, "/", 0);
    hw.PrintLine("SD: Mount setup result: %d", (int)fr);
    
    if(fr == FR_OK)
    {
        hw.PrintLine("SD: Mount setup successful");
        
        // Test actual filesystem access (this triggers the delayed mount)
        hw.PrintLine("SD: Testing filesystem access...");
        DIR dir;
        FRESULT test_fr = f_opendir(&dir, "/");
        
        // Handle the actual mount result
        if(test_fr == FR_NO_FILESYSTEM && auto_format)
        {
            hw.PrintLine("SD: No filesystem detected; formatting...");
            static BYTE workbuf[4096];
            test_fr = f_mkfs("/", FM_FAT | FM_SFD, 0, workbuf, sizeof(workbuf));
            hw.PrintLine("SD: Format result: %d", (int)test_fr);
            if(test_fr == FR_OK)
            {
                test_fr = f_opendir(&dir, "/"); // Try again after format
                hw.PrintLine("SD: Re-test after format result: %d", (int)test_fr);
            }
        }
        
        if(test_fr == FR_OK)
        {
            f_closedir(&dir);
            hw.PrintLine("SD: Filesystem access successful - SD card ready");
            return true;
        }
        else
        {
            // Provide detailed error information
            const char* error_msg = "Unknown error";
            switch(test_fr)
            {
                case FR_NO_FILE: error_msg = "No file"; break;
                case FR_NO_PATH: error_msg = "No path"; break;
                case FR_INVALID_NAME: error_msg = "Invalid name"; break;
                case FR_DENIED: error_msg = "Access denied"; break;
                case FR_NOT_READY: error_msg = "Drive not ready"; break;
                case FR_WRITE_PROTECTED: error_msg = "Write protected"; break;
                case FR_DISK_ERR: error_msg = "Disk error"; break;
                case FR_INT_ERR: error_msg = "Internal error"; break;
                case FR_NOT_ENABLED: error_msg = "Not enabled"; break;
                case FR_NO_FILESYSTEM: error_msg = "No filesystem"; break;
                default: break;
            }
            hw.PrintLine("SD: Filesystem access failed - %s (%d)", error_msg, (int)test_fr);
            return false;
        }
    }
    else
    {
        hw.PrintLine("SD: Mount setup failed: %d", (int)fr);
        return false;
    }
}

} // namespace SdSdio
} // namespace Storage
} // namespace WaveX

#endif // WAVEX_DAISY_SD_CARD_ENABLED && WAVEX_DAISY_SD_CARD_BACKEND == 1


