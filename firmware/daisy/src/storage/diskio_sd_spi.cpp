#include "ff.h"
#include "diskio.h"
#include "daisy_seed.h"
#include "sd_spi.h"

using namespace daisy;
using namespace WaveX::Storage;

// Physical drive number for SPI disk
#define SPI_PDRV 0

static bool g_ready = false;
extern DaisySeed hw; // from main.cpp

// Temporary SPI SD wiring for MVP (documented in README):
// We will use SPI3 on Daisy with pins:
//  SCK  = seed::D2  (PC10)
//  MISO = seed::D1  (PC11)
//  MOSI = seed::D6  (PC12)
//  CS   = seed::D9  (PB4)  [free GPIO]
static constexpr Pin kSck  = seed::D2;
static constexpr Pin kMiso = seed::D1;
static constexpr Pin kMosi = seed::D6;
static constexpr Pin kCs   = seed::D9;

// diskio API implementations

extern "C" DSTATUS disk_initialize(BYTE pdrv)
{
    if(pdrv != SPI_PDRV) return STA_NOINIT;
    g_ready = WaveX::Storage::SdSpi::Init(hw, kSck, kMiso, kMosi, kCs, SpiHandle::Config::Peripheral::SPI_3);
    return g_ready ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_status(BYTE pdrv)
{
    if(pdrv != SPI_PDRV) return STA_NOINIT;
    return g_ready ? 0 : STA_NOINIT;
}

extern "C" DRESULT disk_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count)
{
    if(pdrv != SPI_PDRV || !g_ready) return RES_NOTRDY;
    while(count--)
    {
        if(!WaveX::Storage::SdSpi::ReadSector((uint32_t)sector, buff)) return RES_ERROR;
        sector++; buff += 512;
    }
    return RES_OK;
}

#if FF_FS_READONLY == 0
extern "C" DRESULT disk_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count)
{
    if(pdrv != SPI_PDRV || !g_ready) return RES_NOTRDY;
    while(count--)
    {
        if(!WaveX::Storage::SdSpi::WriteSector((uint32_t)sector, buff)) return RES_ERROR;
        sector++; buff += 512;
    }
    return RES_OK;
}
#endif

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if(pdrv != SPI_PDRV || !g_ready) return RES_NOTRDY;
    switch(cmd)
    {
        case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:  *(DWORD*)buff = 1;   return RES_OK; // erase block size (1 sector)
        default: return RES_PARERR;
    }
}

extern "C" DWORD get_fattime(void)
{
    // 2025-08-20 12:00:00
    return ((DWORD)(2025 - 1980) << 25) | (8 << 21) | (20 << 16) | (12 << 11);
}


