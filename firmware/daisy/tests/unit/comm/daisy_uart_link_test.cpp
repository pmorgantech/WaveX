// daisy_uart_link_test.cpp - NOT BUILT ON THE HOST.
//
// STATUS (read before "fixing" this file): tests/CMakeLists.txt filters this
// file out of the host build entirely (`list(FILTER ... EXCLUDE ...
// daisy_uart_link_test)`), so nothing here compiles or runs in CI - the
// DISABLED_ prefixes below never even register with GoogleTest. The stubs
// are kept only as a checklist of what a future suite must cover.
//
// WHY it is excluded: daisy_uart_link.cpp is built around libDaisy's
// UartHandler with circular-mode DMA reception, idle-line interrupts and the
// STM32 HAL underneath. The mocks in tests/mocks/ deliberately cover only
// what the OTHER comm test needs (a null-able DaisySeed for logging, FatFS,
// the log ring) - none of them model a DMA ring or interrupt timing, and a
// mock UART deep enough to exercise resync/overrun behaviour would be a
// libDaisy-sized project of its own.
//
// What ACTUALLY covers this area today:
//   - the framing/CRC layer (uart_protocol.cpp) is fully host-tested in
//     firmware/shared/tests/, including malformed/truncated frames;
//   - the dispatch layer above it is host-tested by
//     unit/comm/message_dispatch_test.cpp against the real handlers;
//   - the DMA/ISR glue in daisy_uart_link.cpp itself is only exercised on
//     real hardware.
//
// If daisy_uart_link.cpp ever grows a HAL-seam (an injectable byte-source
// interface), delete this placeholder and write real tests against that
// seam; until then there is genuinely nothing host-testable in that file.

#include <gtest/gtest.h>

class DaisyUartLinkTest : public ::testing::Test {};

// Hardware-bound: needs a mock of libDaisy UartHandler DMA reception.
TEST_F(DaisyUartLinkTest, DISABLED_Initialization) {}

// Hardware-bound: TX path calls into libDaisy blocking/DMA transmit.
TEST_F(DaisyUartLinkTest, DISABLED_SendMessage) {}

// Hardware-bound: RX path is fed by the DMA ring + idle-line ISR.
TEST_F(DaisyUartLinkTest, DISABLED_ReceiveMessage) {}

// Hardware-bound: ring wrap/overrun behaviour is a property of the DMA
// controller configuration, not of host-visible code.
TEST_F(DaisyUartLinkTest, DISABLED_DMARingBufferWrap) {}

// Hardware-bound: resync after a dropped byte depends on real inter-byte
// timing; the CRC/framing part is already host-tested in shared/tests.
TEST_F(DaisyUartLinkTest, DISABLED_LinkResyncAfterDrop) {}
