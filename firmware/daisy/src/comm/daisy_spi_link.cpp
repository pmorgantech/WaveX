#include "daisy_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

#include "comm/log_ring.h"
#include "config/hardware_config.h"
#include "daisy_core.h"
#include "daisy_inter_mcu_message_handlers.h"
#include "daisy_uart_link.h"
#include "per/gpio.h"
#include "per/spi.h"
#include "stm32h7xx_hal.h"
#include "sys/system.h"

#include "spi_protocol/spi_transport.hpp"
#include <atomic>
#include <cstring>

extern "C" void dsy_spi_global_init();

namespace WaveX {
namespace Comm {
namespace {

using namespace daisy;
using namespace WaveX::Protocol;
using namespace WaveX::Protocol::Spi;
using TransferState = MasterTransfer::State;
static_assert(WAVEX_CV_BACKEND != WAVEX_CV_BACKEND_MCP48,
              "SPI link recovery requires exclusive libDaisy SPI DMA ownership");

// libDaisy assigns ALL general-purpose SPI DMA to these two streams. This
// link must be their sole owner. Stage A's CV uses I2C; Stage B must resolve
// ownership before sharing libDaisy SPI DMA (architecture §7.3).
SpiHandle* spi = nullptr;
SpiHandle::Config config;
GPIO cs;
GPIO ready;
uint32_t ready_mask = 0;
std::atomic<uint32_t> falling_edges{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "EXTI cannot acquire a lock");

alignas(32) uint8_t tx_dma[kFrameBytes] DMA_BUFFER_MEM_SECTION;
alignas(32) uint8_t rx_dma[kFrameBytes] DMA_BUFFER_MEM_SECTION;
TxQueue outgoing;
// Foreground-only completed RX queue. DMA may be pumped during a command,
// but only the outer main-loop service dispatches commands from this queue.
TxQueue incoming;
bool dispatching = false;
uint16_t next_sequence = 1;
#if WAVEX_DAISY_UART_PERF_DEBUG
uint32_t perf_calls = 0, perf_max_ticks = 0;
uint64_t perf_ticks = 0;
spi_link_stats_t perf_mark{};
void RecordPerf(uint32_t start) {
    const uint32_t ticks = System::GetTick() - start;
    ++perf_calls;
    perf_ticks += ticks;
    if (ticks > perf_max_ticks)
        perf_max_ticks = ticks;
}
#endif
SequenceTracker rx_sequence;
MasterTransfer transfer;
ReadyGate ready_gate;
spi_link_stats_t stats{};
uint32_t last_poll_ms = 0;
bool initialized = false;

GPIO_TypeDef* const kGpioPorts[] = {
    GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOI, GPIOJ, GPIOK};

void ConfigureOutputSlew() {
#if WAVEX_DAISY_SPI_FAST_GPIO_ENABLED
    // Init/recovery in pinned libDaisy resets these to low slew. Change only
    // the SPI outputs' speed fields: preserve AF, mode, pulls and other pins.
    for (const auto pin: {config.pin_config.sclk, config.pin_config.mosi}) {
        auto* const port = kGpioPorts[pin.port];
        const uint32_t shift = 2u * pin.pin;
        port->OSPEEDR =
            (port->OSPEEDR & ~(3u << shift)) | (uint32_t{GPIO_SPEED_FREQ_VERY_HIGH} << shift);
    }
    __DSB();
#endif
}

void ConfigurePriorities() {
    // libDaisy's SPI MSP init installs priority 0. Apply after every init,
    // including recovery, so neither SPI EOT nor DMA can preempt audio.
    HAL_NVIC_SetPriority(SPI1_IRQn, 10, 0);
    HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 10, 0);
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 10, 0);
    HAL_NVIC_EnableIRQ(SPI1_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
}

void MaskTransferIrqs() {
    HAL_NVIC_DisableIRQ(SPI1_IRQn);
    HAL_NVIC_DisableIRQ(DMA2_Stream2_IRQn);
    HAL_NVIC_DisableIRQ(DMA2_Stream3_IRQn);
    __DSB();
    __ISB();
}

void ClearDmaInterrupts() {
    DMA2->LIFCR = DMA_LIFCR_CFEIF2 | DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CTEIF2 | DMA_LIFCR_CHTIF2 |
                  DMA_LIFCR_CTCIF2 | DMA_LIFCR_CFEIF3 | DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTCIF3;
    __DSB();  // Retire peripheral flags before clearing their NVIC pending bits.
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream2_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream3_IRQn);
    __DSB();
}

void StartDma(void*) {
    cs.Write(false);
}

void FinishDma(void*, SpiHandle::Result result) {
    cs.Write(true);
    // libDaisy releases its DMA owner before calling us. RX completion/EOT
    // can win arbitration over a pending TX TC IRQ at the same priority.
    // Its ownerless handler does not acknowledge flags and would then storm
    // forever, starving the foreground watchdog and USB. A successful normal
    // transfer has both streams stopped; retire their remaining IRQs now.
    const bool stopped = ((DMA2_Stream2->CR | DMA2_Stream3->CR) & DMA_SxCR_EN) == 0;
    if (result == SpiHandle::Result::OK && stopped) {
        ClearDmaInterrupts();
        transfer.CompleteFromIsr(true);
    } else {
        // Error callbacks may still leave DMA active. Mask orphan IRQs and
        // retain buffers until the foreground recovery proves both stopped.
        HAL_NVIC_DisableIRQ(DMA2_Stream2_IRQn);
        HAL_NVIC_DisableIRQ(DMA2_Stream3_IRQn);
        __DSB();
        transfer.CompleteFromIsr(false);
    }
}

void BeginRecovery() {
    // The caller masked only link IRQs and rechecked completion. Quiesce the
    // actual hardware before releasing any buffer or libDaisy bookkeeping.
    transfer.Stop();
    SPI1->IER = 0;
    CLEAR_BIT(SPI1->CFG1, SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN);
    CLEAR_BIT(DMA2_Stream2->CR, DMA_SxCR_EN);
    CLEAR_BIT(DMA2_Stream3->CR, DMA_SxCR_EN);
    __HAL_RCC_SPI1_FORCE_RESET();
    __DSB();
    cs.Write(true);
}

void PollRecovery() {
    // No HAL blocking abort, busy-wait, or DMA2 controller reset: UART's DMA
    // stream and audio must keep running. If a stream never stops, retain
    // ownership and stay offline; the foreground continues servicing audio.
    if ((DMA2_Stream2->CR | DMA2_Stream3->CR) & DMA_SxCR_EN)
        return;
    ClearDmaInterrupts();
    HAL_NVIC_ClearPendingIRQ(SPI1_IRQn);
    __HAL_RCC_SPI1_RELEASE_RESET();

    // Safe only under the exclusive-owner rule above, after both streams are
    // stopped. Init restores HAL READY as well as the peripheral registers.
    dsy_spi_global_init();
    if (spi->Init(config) != SpiHandle::Result::OK) {
        initialized = false;
        return;  // Fail closed, with IRQs masked and buffers still reserved.
    }
    ConfigureOutputSlew();
    ConfigurePriorities();
    outgoing.Finish(false);  // Keep the same queue head for a retry.
    transfer.Release();
}

bool CaptureRx() {
    if (incoming.Full())
        return false;  // Keep completed DMA buffers owned until there is space.
    size_t packet_bytes = 0;
    const auto result = InspectFrame(rx_dma, sizeof(rx_dma), rx_sequence, packet_bytes);
    if (result == RxResult::Invalid) {
#if WAVEX_SPI_SIGNAL_DIAGNOSTICS_ENABLED
        if (stats.crc_errors < 8) {
            char prefix[65]{};
            constexpr char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < 32; ++i) {
                prefix[2 * i] = digits[rx_dma[i] >> 4];
                prefix[2 * i + 1] = digits[rx_dma[i] & 15];
            }
            WaveX::Log::PrintLine("SPI signal RX invalid head=%s", prefix);
        }
#endif
        ++stats.crc_errors;
    }
    if (result == RxResult::Duplicate)
        ++stats.seq_drops;
    if (result == RxResult::Packet) {
        if (!incoming.Push(rx_dma, packet_bytes))
            return false;
        ++stats.packets_received;
        stats.rx_bytes += static_cast<uint32_t>(packet_bytes);
    }
    return true;
}

}  // namespace

bool Spi_Init(daisy::DaisySeed& hw, daisy::SpiHandle* hspi) {
#if WAVEX_SPI_DMA_ENABLED
    if (!hspi || initialized)
        return false;
    const auto attn = hw.GetPin(WAVEX_DAISY_ATTN_IN);
    // This vector owns EXTI 10..15. Refuse an incompatible pin configuration.
    if (!attn.IsValid() || attn.pin < 10)
        return false;
    spi = hspi;
    cs.Init(hw.GetPin(WAVEX_DAISY_SPI_CS), GPIO::Mode::OUTPUT, GPIO::Pull::PULLUP);
    cs.Write(true);
    ready.Init(attn, GPIO::Mode::INPUT, GPIO::Pull::PULLDOWN);
    config.periph = SpiHandle::Config::Peripheral::SPI_1;
    config.mode = SpiHandle::Config::Mode::MASTER;
    config.direction = SpiHandle::Config::Direction::TWO_LINES;
    config.datasize = 8;
    config.clock_polarity = (WAVEX_SPI_CLOCK_MODE & 2) ? SpiHandle::Config::ClockPolarity::HIGH
                                                       : SpiHandle::Config::ClockPolarity::LOW;
    config.clock_phase = (WAVEX_SPI_CLOCK_MODE & 1) ? SpiHandle::Config::ClockPhase::TWO_EDGE
                                                    : SpiHandle::Config::ClockPhase::ONE_EDGE;
#if WAVEX_DAISY_SPI_CLOCK_HZ == 32000000
    // CKPER already selects the running HSI64 in this clock tree. Do not
    // reconfigure a shared clock: the frequency check below fails closed if
    // its source, divider or readiness differs from the expected nominal rate.
    constexpr uint32_t clock_source = RCC_SPI123CLKSOURCE_CLKP;
    constexpr uint32_t divider = 2;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_2;
#elif WAVEX_DAISY_SPI_CLOCK_HZ == 24000000 || WAVEX_DAISY_SPI_CLOCK_HZ == 48000000
    constexpr uint32_t clock_source = RCC_SPI123CLKSOURCE_PLL;
#if WAVEX_DAISY_SPI_CLOCK_HZ == 48000000
    constexpr uint32_t divider = 4;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_4;
#else
    constexpr uint32_t divider = 8;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_8;
#endif
#else
    constexpr uint32_t clock_source = RCC_SPI123CLKSOURCE_PLL2;
#if WAVEX_DAISY_SPI_CLOCK_HZ == 12500000
    constexpr uint32_t divider = 2;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_2;
#elif WAVEX_DAISY_SPI_CLOCK_HZ == 6250000
    constexpr uint32_t divider = 4;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_4;
#else
    constexpr uint32_t divider = 16;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_16;
#endif
#endif
    // The link exclusively owns SPI DMA and is the only active SPI123 user.
    // Select an already-running kernel source; never retune shared clocks here.
    __HAL_RCC_SPI123_CONFIG(clock_source);
    const uint32_t kernel_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI1);
    const uint32_t actual_hz = kernel_hz / divider;
    if (actual_hz != WAVEX_DAISY_SPI_CLOCK_HZ) {
        WaveX::Log::PrintLine("SPI clock mismatch: requested=%lu actual=%lu Hz",
                              static_cast<unsigned long>(WAVEX_DAISY_SPI_CLOCK_HZ),
                              static_cast<unsigned long>(actual_hz));
        return false;
    }
    WaveX::Log::PrintLine("SPI master clock=%lu Hz (kernel=%lu divider=%lu)",
                          static_cast<unsigned long>(actual_hz),
                          static_cast<unsigned long>(kernel_hz),
                          static_cast<unsigned long>(divider));
    config.nss = SpiHandle::Config::NSS::SOFT;
    config.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    config.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    config.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    config.pin_config.nss = Pin();  // CS remains a GPIO throughout DMA.

    dsy_spi_global_init();
    if (spi->Init(config) != SpiHandle::Result::OK)
        return false;
    ConfigureOutputSlew();
    ConfigurePriorities();

    ready_mask = uint32_t{1} << attn.pin;
    GPIO_InitTypeDef gpio{};
    gpio.Pin = ready_mask;
    gpio.Mode = GPIO_MODE_IT_FALLING;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(kGpioPorts[attn.port], &gpio);
    __HAL_GPIO_EXTI_CLEAR_IT(ready_mask);
    HAL_NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 14, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
    initialized = true;
    return true;
#else
    (void)hw;
    (void)hspi;
    return false;
#endif
}

bool Spi_SendPreCreatedPacket(const uint8_t* packet, size_t bytes) {
    // Foreground only, like the unified dispatcher. ISRs never touch this queue.
    if (!initialized)
        return false;
    if (!outgoing.Push(packet, bytes)) {
        ++stats.tx_q_overflows;
        return false;
    }
    return true;
}

int Spi_Send(uint16_t type, const void* payload, uint16_t bytes) {
    if (type > UINT8_MAX || bytes > kMaxPayload || (bytes && !payload))
        return -1;
    uint8_t packet[kFrameBytes];
    const size_t size = UartProtocol::CreateUartPacket(
        packet, sizeof(packet), static_cast<uint8_t>(type), payload, bytes, next_sequence, 0);
    if (!size || !Spi_SendPreCreatedPacket(packet, size))
        return -1;
    next_sequence = NextSequence(next_sequence);
    return bytes;
}

bool Spi_TxIdle() {
    return initialized && outgoing.Count() == 0 && transfer.Get() != TransferState::Stopping &&
           transfer.Get() != TransferState::Failed;
}

void Spi_CheckTimeout() {
    if (!initialized)
        return;
    if (transfer.Expired(System::GetNow()) || transfer.Get() == TransferState::Failed) {
        MaskTransferIrqs();
        // Completion could have arrived just before masking; preserve its RX.
        if (transfer.Get() == TransferState::Complete)
            ConfigurePriorities();
        else {
            ++stats.timeouts;
            BeginRecovery();
        }
    }
    if (transfer.Get() == TransferState::Stopping)
        PollRecovery();
}

namespace {
bool ObserveReady(uint32_t& edges) {
    // Do not combine a level sampled after an edge with an older generation.
    // Defer an unstable sample to the next foreground pass.
    edges = falling_edges.load(std::memory_order_acquire);
    const bool high = ready.Read();
    if (edges != falling_edges.load(std::memory_order_acquire))
        return false;
    return ready_gate.Observe(high, edges);
}
}  // namespace

bool Spi_PollAttnLevel() {
    uint32_t edges = 0;
    return initialized && ObserveReady(edges);
}

namespace {
void PumpTx(bool allow_launch) {
    if (!initialized)
        return;
    Spi_CheckTimeout();
    if (!initialized)
        return;
    if (transfer.Get() == TransferState::Complete) {
        if (!CaptureRx())
            return;
        const size_t bytes = UartProtocol::GetFrameLength(tx_dma, sizeof(tx_dma));
        if (outgoing.Finish(true)) {
            ++stats.packets_sent;
            stats.tx_bytes += static_cast<uint32_t>(bytes);
        }
        stats.last_activity_ms = System::GetNow();
        transfer.Release();
    }

    const uint32_t now = System::GetNow();
    uint32_t edges = 0;
    const bool can_start = ObserveReady(edges);
    if (!allow_launch || incoming.Full() || transfer.Get() != TransferState::Idle || !can_start ||
        (!WAVEX_DAISY_SPI_FAST_SCHEDULING_ENABLED &&
         static_cast<uint32_t>(now - last_poll_ms) < kPollIntervalMs))
        return;

    // Reserve BEFORE touching either buffer. EXTI only records an edge and can
    // never enter libDaisy's one-slot DMA job queue or overwrite this frame.
    if (!transfer.Reserve(now))
        return;
    if (!outgoing.Begin(tx_dma)) {
        transfer.Release();
        return;
    }
    std::memset(rx_dma, 0, sizeof(rx_dma));
    // A READY transition during preparation invalidates the sample. Retry
    // without consuming the packet; no DMA has been handed either buffer yet.
    if (!ready.Read() || edges != falling_edges.load(std::memory_order_acquire)) {
        outgoing.Finish(false);
        transfer.Release();
        return;
    }
    ready_gate.Consume(edges);
    last_poll_ms = now;
    // With one foreground owner and Init/completion establishing HAL READY,
    // libDaisy takes its immediate launch path, never its unbounded queue wait.
    if (spi->DmaTransmitAndReceive(tx_dma, rx_dma, kFrameBytes, StartDma, FinishDma, nullptr) !=
        SpiHandle::Result::OK) {
        transfer.CompleteFromIsr(false);
    }
}

}  // namespace

void Spi_PumpTx() {
    PumpTx(true);
}

void ProcessQueuedSpiMessage() {
    if (dispatching)
        return;
#if WAVEX_DAISY_UART_PERF_DEBUG
    const uint32_t start = System::GetTick();
#endif
    // Fast mode first retires DMA and retains RX, without arming an empty
    // response ahead of the command being dispatched. Still one command per
    // foreground pass; nested TX-only pumping never recursively dispatches.
    PumpTx(!WAVEX_DAISY_SPI_FAST_SCHEDULING_ENABLED);
    const bool had_command = incoming.Count() != 0;
    if (had_command) {
        uint8_t frame[kFrameBytes];
        incoming.Begin(frame);
        incoming.Finish(true);
        uint8_t payload[kMaxPayload];
        size_t bytes = sizeof(payload);
        uint8_t type = 0, flags = 0;
        uint16_t sequence = 0;
        const size_t framed = UartProtocol::GetFrameLength(frame, sizeof(frame));
        if (UartProtocol::ParseUartPacket(frame, framed, type, payload, bytes, sequence, flags)) {
            dispatching = true;
            if (!(flags & (UartProtocol::UART_FLAG_ACK | UartProtocol::UART_FLAG_NACK)))
                ProcessInterMcuMessage(type, sequence, payload, bytes);
            dispatching = false;
        }
    }
#if WAVEX_DAISY_SPI_FAST_SCHEDULING_ENABLED
    // Some handlers stage their reply for a producer later in this main-loop
    // pass. Do not put an empty frame ahead of that reply. The next outer
    // pass polls normally; long handlers can still explicitly pump TX.
    if (!had_command || outgoing.Count())
        Spi_PumpTx();
#endif
#if WAVEX_DAISY_UART_PERF_DEBUG
    RecordPerf(start);
#endif
}

#if WAVEX_DAISY_UART_PERF_DEBUG
void Spi_TakePerf(UartPerfSample& out) {
    const uint32_t ticks_per_us = System::GetTickFreq() / 1000000u;
    out = {};
    out.calls = perf_calls;
    out.total_us = static_cast<uint32_t>(perf_ticks / ticks_per_us);
    out.avg_us = perf_calls ? out.total_us / perf_calls : 0;
    out.max_us = perf_max_ticks / ticks_per_us;
    out.rx_bytes = stats.rx_bytes - perf_mark.rx_bytes;
    out.tx_bytes = stats.tx_bytes - perf_mark.tx_bytes;
    out.rx_frames = stats.packets_received - perf_mark.packets_received;
    out.tx_frames = stats.packets_sent - perf_mark.packets_sent;
    out.errors = (stats.crc_errors - perf_mark.crc_errors) + (stats.timeouts - perf_mark.timeouts) +
                 (stats.tx_q_overflows - perf_mark.tx_q_overflows);
    out.seq_drops = stats.seq_drops;
    out.queue_overflows = stats.tx_q_overflows;
    perf_mark = stats;
    perf_calls = perf_max_ticks = 0;
    perf_ticks = 0;
}
#endif

void Spi_GetStats(spi_link_stats_t* out) {
    if (out) {
        *out = stats;
        out->irq_asserts = falling_edges.load(std::memory_order_relaxed);
    }
}

void Spi_DebugState() {
    WaveX::Log::PrintLine(
        "SPI initialized=%u state=%u queued=%u RX=%lu TX=%lu invalid=%lu timeouts=%lu",
        unsigned(initialized),
        unsigned(transfer.Get()),
        unsigned(outgoing.Count()),
        static_cast<unsigned long>(stats.packets_received),
        static_cast<unsigned long>(stats.packets_sent),
        static_cast<unsigned long>(stats.crc_errors),
        static_cast<unsigned long>(stats.timeouts));
}

void SpiReadyFallingEdge() {
    if (__HAL_GPIO_EXTI_GET_IT(ready_mask)) {
        __HAL_GPIO_EXTI_CLEAR_IT(ready_mask);
        falling_edges.fetch_add(1, std::memory_order_release);
    }
}

}  // namespace Comm
}  // namespace WaveX

extern "C" void EXTI15_10_IRQHandler() {
    WaveX::Comm::SpiReadyFallingEdge();
}

#endif  // WAVEX_SPI_LINK_ENABLED
