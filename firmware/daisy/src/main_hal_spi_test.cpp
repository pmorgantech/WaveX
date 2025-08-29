#include "daisy_seed.h"
#include <cstdint>
#include <cstdio>
#define DAISY_PLATFORM 1
#include "shared/config/pin_config.h"
#include "comm/inter_spi_hal.h"
#include "config/link_config.h"

using namespace daisy;

// Hardware
DaisySeed hw;

int main(void)
{
    // Initialize Daisy Seed hardware
    hw.Configure();
    hw.Init();

    // Initialize USB CDC for debugging
    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);
    hw.StartLog(true);
    hw.PrintLine("HAL SPI Slave Test - Hardware initialized");

    // Initialize HAL-based SPI slave for inter-MCU communication
    #if WAVEX_SPI_LINK_ENABLED
    hw.PrintLine("Initializing HAL-based SPI slave...");
    
    WaveX::Comm::SpiHal_Init(hw);
    hw.PrintLine("HAL SPI slave initialized successfully");
    
    // Test GPIO readings
    WaveX::Comm::SpiHal_TestBlocking();
    
    #else
    hw.PrintLine("SPI link disabled");
    #endif

    // Main loop
    uint32_t last_debug = System::GetNow();
    uint32_t loop_counter = 0;
    
    hw.PrintLine("Entering main loop - waiting for NSS falling edge from ESP32");
    
    while(1)
    {
        loop_counter++;
        
        // Service HAL SPI communication (NSS edge detection and transfers)
        #if WAVEX_SPI_LINK_ENABLED
        WaveX::Comm::SpiHal_Service();
        #endif
        
        // Debug output every 2 seconds
        if(System::GetNow() - last_debug >= 2000) {
            last_debug = System::GetNow();
            hw.PrintLine("Loop %lu - Uptime: %lu ms", (unsigned long)loop_counter, (unsigned long)last_debug);
            
            #if WAVEX_SPI_LINK_ENABLED
            WaveX::Comm::SpiHal_DebugState();
            WaveX::Comm::SpiHal_TestBlocking();
            #endif
        }

        // Small delay
        System::Delay(1);
    }
}
