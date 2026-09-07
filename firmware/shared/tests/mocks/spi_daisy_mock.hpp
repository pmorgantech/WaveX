#pragma once

// Hardware-only model for the real daisy_spi_link.cpp translation unit.
#include "config/pin_config.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>

#define WAVEX_SPI_LINK_ENABLED 1
#define WAVEX_SPI_DMA_ENABLED 1
#define DMA_BUFFER_MEM_SECTION

enum IRQn_Type { SPI1_IRQn, DMA2_Stream2_IRQn, DMA2_Stream3_IRQn, EXTI15_10_IRQn };
struct GPIO_TypeDef {};
inline GPIO_TypeDef gpio_ports[11];
#define GPIOA (&gpio_ports[0])
#define GPIOB (&gpio_ports[1])
#define GPIOC (&gpio_ports[2])
#define GPIOD (&gpio_ports[3])
#define GPIOE (&gpio_ports[4])
#define GPIOF (&gpio_ports[5])
#define GPIOG (&gpio_ports[6])
#define GPIOH (&gpio_ports[7])
#define GPIOI (&gpio_ports[8])
#define GPIOJ (&gpio_ports[9])
#define GPIOK (&gpio_ports[10])
struct GPIO_InitTypeDef {
    uint32_t Pin = 0, Mode = 0, Pull = 0;
};
constexpr uint32_t GPIO_MODE_IT_FALLING = 1, GPIO_PULLDOWN = 2;
struct SpiRegisters {
    uint32_t IER = 0, CFG1 = 0;
};
struct DmaStream {
    uint32_t CR = 0;
};
struct DmaRegisters {
    uint32_t LIFCR = 0;
};
inline SpiRegisters spi_registers;
inline DmaStream rx_stream, tx_stream;
inline DmaRegisters dma_registers;
#define SPI1 (&spi_registers)
#define DMA2_Stream2 (&rx_stream)
#define DMA2_Stream3 (&tx_stream)
#define DMA2 (&dma_registers)
constexpr uint32_t DMA_SxCR_EN = 1, SPI_CFG1_RXDMAEN = 2, SPI_CFG1_TXDMAEN = 4;
constexpr uint32_t DMA_LIFCR_CFEIF2 = 1, DMA_LIFCR_CDMEIF2 = 2, DMA_LIFCR_CTEIF2 = 4,
                   DMA_LIFCR_CHTIF2 = 8, DMA_LIFCR_CTCIF2 = 16, DMA_LIFCR_CFEIF3 = 32,
                   DMA_LIFCR_CDMEIF3 = 64, DMA_LIFCR_CTEIF3 = 128, DMA_LIFCR_CHTIF3 = 256,
                   DMA_LIFCR_CTCIF3 = 512;

namespace SpiDaisyMock {
inline uint32_t now = 10, pending_exti = 0;
inline bool ready = false, cs_high = true, hold_dma_enabled = false, reset = false;
inline std::array<bool, 4> irq_enabled{};
inline std::array<uint32_t, 4> priorities{};
inline unsigned launches = 0, inits = 0, resets = 0, scheduler_resets = 0, routed = 0;
inline size_t routed_bytes = 0;
inline uint8_t first_payload = 0;
inline std::function<void()> before_tick, during_ready_read, on_route;
inline void Clear(uint32_t& reg, uint32_t mask) {
    if (hold_dma_enabled && (&reg == &rx_stream.CR || &reg == &tx_stream.CR))
        return;
    reg &= ~mask;
}
inline void ResetPeripheral() {
    spi_registers = {};
    reset = true;
    ++resets;
}
}  // namespace SpiDaisyMock
#define CLEAR_BIT(reg, bits) SpiDaisyMock::Clear(reg, bits)
#define __HAL_RCC_SPI1_FORCE_RESET() SpiDaisyMock::ResetPeripheral()
#define __HAL_RCC_SPI1_RELEASE_RESET() (SpiDaisyMock::reset = false)
#define __HAL_GPIO_EXTI_CLEAR_IT(mask) (SpiDaisyMock::pending_exti &= ~(mask))
#define __HAL_GPIO_EXTI_GET_IT(mask) (SpiDaisyMock::pending_exti & (mask))
inline void __DSB() {}
inline void __ISB() {}
inline void HAL_NVIC_SetPriority(IRQn_Type irq, uint32_t priority, uint32_t) {
    SpiDaisyMock::priorities[irq] = priority;
}
inline void HAL_NVIC_EnableIRQ(IRQn_Type irq) {
    SpiDaisyMock::irq_enabled[irq] = true;
}
inline void HAL_NVIC_DisableIRQ(IRQn_Type irq) {
    SpiDaisyMock::irq_enabled[irq] = false;
}
inline void HAL_NVIC_ClearPendingIRQ(IRQn_Type) {}
inline void HAL_GPIO_Init(GPIO_TypeDef*, GPIO_InitTypeDef*) {}

namespace daisy {
struct Pin {
    unsigned port = 1;
    uint8_t pin = 12;
    bool IsValid() const { return port < 11 && pin < 16; }
};
class DaisySeed {
   public:
    Pin GetPin(int) { return {}; }
};
class GPIO {
   public:
    enum class Mode { INPUT, OUTPUT };
    enum class Pull { PULLUP, PULLDOWN };
    void Init(Pin, Mode, Pull) {}
    void Write(bool high) { SpiDaisyMock::cs_high = high; }
    bool Read() {
        const bool level = SpiDaisyMock::ready;
        if (SpiDaisyMock::during_ready_read)
            SpiDaisyMock::during_ready_read();
        return level;
    }
};
struct System {
    static uint32_t GetNow() {
        if (SpiDaisyMock::before_tick)
            SpiDaisyMock::before_tick();
        return SpiDaisyMock::now;
    }
};
class SpiHandle {
   public:
    enum class Result { OK, ERR };
    struct Config {
        enum class Peripheral { SPI_1 };
        enum class Mode { MASTER };
        enum class Direction { TWO_LINES };
        enum class ClockPolarity { LOW };
        enum class ClockPhase { ONE_EDGE };
        enum class BaudPrescaler { PS_16 };
        enum class NSS { SOFT };
        Peripheral periph{};
        Mode mode{};
        Direction direction{};
        unsigned datasize = 0;
        ClockPolarity clock_polarity{};
        ClockPhase clock_phase{};
        BaudPrescaler baud_prescaler{};
        NSS nss{};
        struct {
            Pin sclk, mosi, miso, nss;
        } pin_config;
    };
    Result Init(const Config&) {
        assert(!(rx_stream.CR & DMA_SxCR_EN));
        assert(!(tx_stream.CR & DMA_SxCR_EN));
        ++SpiDaisyMock::inits;
        HAL_NVIC_SetPriority(SPI1_IRQn, 0, 0);  // libDaisy's MSP default.
        return Result::OK;
    }
    inline static void (*completion)(void*, Result) = nullptr;
    inline static void* context = nullptr;
    inline static uint8_t* rx = nullptr;
    inline static uint8_t* tx = nullptr;
    inline static bool fail_launch = false;
    Result DmaTransmitAndReceive(uint8_t* tx_data,
                                 uint8_t* rx_data,
                                 size_t bytes,
                                 void (*start)(void*),
                                 void (*end)(void*, Result),
                                 void* ctx) {
        assert(bytes == 2048);
        assert(!(rx_stream.CR & DMA_SxCR_EN));  // A second launch would enter the vendor wait.
        assert(!(tx_stream.CR & DMA_SxCR_EN));
        ++SpiDaisyMock::launches;
        tx = tx_data;
        rx = rx_data;
        completion = end;
        context = ctx;
        start(ctx);
        rx_stream.CR = tx_stream.CR = DMA_SxCR_EN;
        return fail_launch ? Result::ERR : Result::OK;
    }
    static void Complete(Result result = Result::OK) {
        rx_stream.CR = tx_stream.CR = 0;
        completion(context, result);
    }
};
}  // namespace daisy
namespace WaveX {
namespace Log {
template <typename... Args>
void PrintLine(const char*, Args...) {}
}  // namespace Log
}  // namespace WaveX
