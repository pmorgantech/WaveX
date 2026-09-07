#include "daisy_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

#include "comm/log_ring.h"
#include "config/hardware_config.h"
#include "daisy_core.h"
#include "daisy_inter_mcu_message_handlers.h"
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
SequenceTracker rx_sequence;
MasterTransfer transfer;
ReadyGate ready_gate;
spi_link_stats_t stats{};
uint32_t last_poll_ms = 0;
bool initialized = false;

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

void StartDma(void*) {
    cs.Write(false);
}

void FinishDma(void*, SpiHandle::Result result) {
    cs.Write(true);
    transfer.CompleteFromIsr(result == SpiHandle::Result::OK);
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
    DMA2->LIFCR = DMA_LIFCR_CFEIF2 | DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CTEIF2 | DMA_LIFCR_CHTIF2 |
                  DMA_LIFCR_CTCIF2 | DMA_LIFCR_CFEIF3 | DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTCIF3;
    HAL_NVIC_ClearPendingIRQ(SPI1_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream2_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream3_IRQn);
    __HAL_RCC_SPI1_RELEASE_RESET();

    // Safe only under the exclusive-owner rule above, after both streams are
    // stopped. Init restores HAL READY as well as the peripheral registers.
    dsy_spi_global_init();
    if (spi->Init(config) != SpiHandle::Result::OK) {
        initialized = false;
        return;  // Fail closed, with IRQs masked and buffers still reserved.
    }
    ConfigurePriorities();
    outgoing.Finish(false);  // Keep the same queue head for a retry.
    transfer.Release();
}

void ProcessRx() {
    size_t packet_bytes = 0;
    const auto result = InspectFrame(rx_dma, sizeof(rx_dma), rx_sequence, packet_bytes);
    if (result == RxResult::Invalid) {
        ++stats.crc_errors;
        return;
    }
    if (result != RxResult::Packet)
        return;

    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t sequence = 0;
    uint8_t payload[kFrameBytes];
    size_t payload_bytes = sizeof(payload);  // ParseWaveXPacket takes capacity IN/OUT.
    if (!ProtocolHandler::ParseWaveXPacket(
            rx_dma, packet_bytes, type, payload, payload_bytes, sequence, flags)) {
        ++stats.crc_errors;
        return;
    }
    ++stats.packets_received;
    if (!(flags & (PKT_FLAG_ACK | PKT_FLAG_NACK)))
        ProcessInterMcuMessage(type, sequence, payload, payload_bytes);
}

}  // namespace

void Spi_Init(daisy::DaisySeed& hw, daisy::SpiHandle* hspi) {
#if WAVEX_SPI_DMA_ENABLED
    if (!hspi || initialized)
        return;
    const auto attn = hw.GetPin(WAVEX_DAISY_ATTN_IN);
    // This vector owns EXTI 10..15. Refuse an incompatible pin configuration.
    if (!attn.IsValid() || attn.pin < 10)
        return;
    spi = hspi;
    cs.Init(hw.GetPin(WAVEX_DAISY_SPI_CS), GPIO::Mode::OUTPUT, GPIO::Pull::PULLUP);
    cs.Write(true);
    ready.Init(attn, GPIO::Mode::INPUT, GPIO::Pull::PULLDOWN);
    config.periph = SpiHandle::Config::Peripheral::SPI_1;
    config.mode = SpiHandle::Config::Mode::MASTER;
    config.direction = SpiHandle::Config::Direction::TWO_LINES;
    config.datasize = 8;
    config.clock_polarity = SpiHandle::Config::ClockPolarity::LOW;
    config.clock_phase = SpiHandle::Config::ClockPhase::ONE_EDGE;
    config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_16;
    config.nss = SpiHandle::Config::NSS::SOFT;
    config.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    config.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    config.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    config.pin_config.nss = Pin();  // CS remains a GPIO throughout DMA.

    dsy_spi_global_init();
    if (spi->Init(config) != SpiHandle::Result::OK)
        return;
    ConfigurePriorities();

    GPIO_TypeDef* const ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOI, GPIOJ, GPIOK};
    ready_mask = uint32_t{1} << attn.pin;
    GPIO_InitTypeDef gpio{};
    gpio.Pin = ready_mask;
    gpio.Mode = GPIO_MODE_IT_FALLING;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(ports[attn.port], &gpio);
    __HAL_GPIO_EXTI_CLEAR_IT(ready_mask);
    HAL_NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 14, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
    initialized = true;
#else
    (void)hw;
    (void)hspi;
#endif
}

bool Spi_SendPreCreatedPacket(const uint8_t* packet, size_t bytes) {
    // Foreground only, like the unified dispatcher. ISRs never touch this queue.
    return initialized && outgoing.Push(packet, bytes);
}

void Spi_CheckTimeout() {
    if (!initialized)
        return;
    if (transfer.Expired(System::GetNow()) || transfer.Get() == TransferState::Failed) {
        MaskTransferIrqs();
        // Completion could have arrived just before masking; preserve its RX.
        if (transfer.Get() == TransferState::Complete)
            ConfigurePriorities();
        else
            BeginRecovery();
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

void ProcessQueuedSpiMessage() {
    if (!initialized)
        return;
    Spi_CheckTimeout();
    if (!initialized)
        return;
    if (transfer.Get() == TransferState::Complete) {
        // Keep both buffers reserved while routing; a response can append to the
        // queue but cannot start another transfer or overwrite the completed RX.
        ProcessRx();
        if (outgoing.Finish(true))
            ++stats.packets_sent;
        stats.last_activity_ms = System::GetNow();
        transfer.Release();
    }

    const uint32_t now = System::GetNow();
    uint32_t edges = 0;
    const bool can_start = ObserveReady(edges);
    if (transfer.Get() != TransferState::Idle || !can_start ||
        static_cast<uint32_t>(now - last_poll_ms) < kPollIntervalMs)
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

void Spi_GetStats(spi_link_stats_t* out) {
    if (out) {
        *out = stats;
        out->irq_asserts = falling_edges.load(std::memory_order_relaxed);
    }
}

void Spi_DebugState() {
    WaveX::Log::PrintLine("SPI initialized=%u state=%u queued=%u",
                          unsigned(initialized),
                          unsigned(transfer.Get()),
                          unsigned(outgoing.Count()));
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
