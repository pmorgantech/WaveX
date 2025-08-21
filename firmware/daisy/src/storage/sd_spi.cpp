
#include "sd_spi.h"
#include <cstring>
#include "daisy_seed.h"

using namespace daisy;

namespace WaveX {
namespace Storage {
namespace SdSpi {

static DaisySeed*  s_hw = nullptr;
static SpiHandle   s_spi;
static GPIO        s_cs;

static inline void CS_L() { s_cs.Write(0); }
static inline void CS_H() { s_cs.Write(1); }

// Send/recv single byte
static uint8_t xfer1(uint8_t b)
{
    uint8_t rx = 0;
    s_spi.BlockingTransmitAndReceive(&b, &rx, 1, 10);
    return rx;
}

// Send N bytes of 0xFF (clocking)
static void clocks(unsigned n)
{
    uint8_t ff = 0xFF, rx;
    while(n--) s_spi.BlockingTransmitAndReceive(&ff, &rx, 1, 10);
}

// Wait for a non-0xFF token with timeout (ms)
static uint8_t wait_token(uint32_t ms)
{
    uint32_t start = System::GetNow();
    uint8_t  r;
    do { r = xfer1(0xFF); } while(r == 0xFF && (System::GetNow() - start) < ms);
    return r;
}

// Send SD command
static uint8_t cmd(uint8_t c, uint32_t arg, uint8_t crc)
{
    xfer1(0xFF);
    uint8_t frame[6] = { (uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)(arg), crc };
    for(int i = 0; i < 6; i++) xfer1(frame[i]);
    for(int i = 0; i < 10; i++)
    {
        uint8_t r = xfer1(0xFF);
        if((r & 0x80) == 0) return r;
    }
    return 0xFF;
}

bool Init(DaisySeed& hw,
          Pin sck,
          Pin miso,
          Pin mosi,
          Pin cs_pin,
          SpiHandle::Config::Peripheral periph)
{
    s_hw = &hw;

    s_cs.Init(cs_pin, GPIO::Mode::OUTPUT);
    CS_H();
    hw.PrintLine("SD: CS pin initialized");

    // Try different SPI modes - some SD cards need different clock polarity/phase
    SpiHandle::Config cfg;
    cfg.periph         = periph;
    cfg.mode           = SpiHandle::Config::Mode::MASTER;
    cfg.direction      = SpiHandle::Config::Direction::TWO_LINES;
    cfg.nss            = SpiHandle::Config::NSS::SOFT;
    cfg.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_256; // ~400kHz
    cfg.pin_config.sclk = sck;
    cfg.pin_config.miso = miso;
    cfg.pin_config.mosi = mosi;
    
    // Try Mode 0 first (CPOL=0, CPHA=0) - most common
    cfg.clock_polarity = SpiHandle::Config::ClockPolarity::LOW;
    cfg.clock_phase    = SpiHandle::Config::ClockPhase::ONE_EDGE;
    
    hw.PrintLine("SD: Attempting SPI init with periph=%d, Mode 0", (int)periph);
    if(s_spi.Init(cfg) != SpiHandle::Result::OK) {
        hw.PrintLine("SD: SPI init FAILED");
        return false;
    }
    hw.PrintLine("SD: SPI init OK at 400kHz (Mode 0)");

    // 80+ clocks with CS high
    CS_H(); clocks(10);
    hw.PrintLine("SD: Sent 80+ clocks");
    
    // Add longer power-up delay for some SD cards (send more clocks)
    clocks(50); // Send 50 more clocks for power-up delay
    hw.PrintLine("SD: Power-up delay complete");
    
    // CMD0
    CS_L();
    clocks(2); // Send 2 more clocks after CS_L
    uint8_t r = cmd(0, 0, 0x95);
    clocks(2); // Send 2 more clocks before CS_H
    CS_H(); xfer1(0xFF);
    hw.PrintLine("SD: CMD0 response: 0x%02X", r);
    
    // If Mode 0 fails, try Mode 3 (CPOL=1, CPHA=1)
    if(r != 0x01) {
        hw.PrintLine("SD: CMD0 failed with Mode 0, trying Mode 3...");
        
        cfg.clock_polarity = SpiHandle::Config::ClockPolarity::HIGH;
        cfg.clock_phase    = SpiHandle::Config::ClockPhase::TWO_EDGE;
        
        if(s_spi.Init(cfg) != SpiHandle::Result::OK) {
            hw.PrintLine("SD: Mode 3 SPI init FAILED");
            return false;
        }
        hw.PrintLine("SD: SPI reinit OK at 400kHz (Mode 3)");
        
        // Reset and try again
        CS_H(); clocks(10);
        clocks(50); // Power-up delay
        
        CS_L();
        clocks(2); // Delay after CS_L
        r = cmd(0, 0, 0x95);
        clocks(2); // Delay before CS_H
        CS_H(); xfer1(0xFF);
        hw.PrintLine("SD: CMD0 response (Mode 3): 0x%02X", r);
        
        if(r != 0x01) {
            hw.PrintLine("SD: CMD0 failed with both modes - expected 0x01, got 0x%02X", r);
            return false;
        }
    }
    
    hw.PrintLine("SD: CMD0 OK - card in idle state");

    // CMD8
    CS_L();
    r = cmd(8, 0x1AA, 0x87);
    uint8_t v2 = (r == 0x01);
    hw.PrintLine("SD: CMD8 response: 0x%02X (v2=%s)", r, v2 ? "yes" : "no");
    if(v2){ for(int i=0;i<4;i++) xfer1(0xFF); }
    CS_H(); xfer1(0xFF);

    // ACMD41 loop
    hw.PrintLine("SD: Starting ACMD41 initialization loop...");
    uint32_t t0 = System::GetNow();
    int acmd41_count = 0;
    do {
        CS_L(); cmd(55, 0, 0x65); CS_H(); xfer1(0xFF);
        CS_L(); r = cmd(41, v2 ? (1UL<<30) : 0, 0x77); CS_H(); xfer1(0xFF);
        acmd41_count++;
        if(System::GetNow() - t0 > 1000) {
            hw.PrintLine("SD: ACMD41 timeout after %d attempts", acmd41_count);
            return false;
        }
    } while(r != 0x00);
    hw.PrintLine("SD: ACMD41 OK after %d attempts", acmd41_count);

    // CMD58 OCR
    CS_L(); r = cmd(58, 0, 0xFD); (void)r;
    for(int i=0;i<4;i++) xfer1(0xFF);
    CS_H(); xfer1(0xFF);
    hw.PrintLine("SD: CMD58 OK");

    // Speed up
    hw.PrintLine("SD: Switching to high speed...");
    auto fast = s_spi.GetConfig();
    fast.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_8; // ~20-24MHz
    if(s_spi.Init(fast) != SpiHandle::Result::OK) {
        hw.PrintLine("SD: High speed SPI init FAILED");
        return false;
    }
    hw.PrintLine("SD: High speed SPI OK at 20-24MHz");

    hw.PrintLine("SD: Initialization complete");
    return true;
}

bool ReadSector(uint32_t lba, uint8_t* buf)
{
    CS_L();
    uint8_t r = cmd(17, lba, 0x01);
    if(r != 0x00) { CS_H(); xfer1(0xFF); return false; }
    if(wait_token(100) != 0xFE) { CS_H(); xfer1(0xFF); return false; }
    for(int i=0;i<512;i++) buf[i] = xfer1(0xFF);
    xfer1(0xFF); xfer1(0xFF);
    CS_H(); xfer1(0xFF);
    return true;
}

bool WriteSector(uint32_t lba, const uint8_t* buf)
{
    CS_L();
    uint8_t r = cmd(24, lba, 0x01);
    if(r != 0x00) { CS_H(); xfer1(0xFF); return false; }
    xfer1(0xFE);
    for(int i=0;i<512;i++) xfer1(buf[i]);
    xfer1(0xFF); xfer1(0xFF);
    uint8_t dr = xfer1(0xFF) & 0x1F;
    if(dr != 0x05) { CS_H(); xfer1(0xFF); return false; }
    if(wait_token(500) == 0x00) { CS_H(); xfer1(0xFF); return false; }
    CS_H(); xfer1(0xFF);
    return true;
}

} // namespace SdSpi
} // namespace Storage
} // namespace WaveX


