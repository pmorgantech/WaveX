#include "storage/sd_io_diagnostics.hpp"

#include <gtest/gtest.h>

using namespace WaveX::Storage::SdIo;

TEST(SdIoDiagnostics, RetainsFirstIrqBeforeCleanupAndLaterFailures) {
    FirstFailure capture;
    capture.Begin(true, 0, 42, 8, 0x24000100);
    Registers irq;
    irq.status = 2;
    irq.error = 2;
    irq.remaining = 3584;
    irq.buffer = 0x24008000;  // A staged DMA buffer differs from the caller.
    capture.ObserveIrq(irq);
    Registers cleanup;
    cleanup.error = 4;
    capture.ObserveIrq(cleanup);
    capture.Finish(1, 17, cleanup);
    capture.Begin(false, 0, 90, 1, 0x24000200);
    capture.Finish(1, 30, cleanup);
    auto f = capture.Snapshot();
    EXPECT_TRUE(f.valid);
    EXPECT_TRUE(f.write);
    EXPECT_TRUE(f.irq);
    EXPECT_EQ(f.sector, 42u);
    EXPECT_EQ(f.buffer, 0x24000100u);
    EXPECT_EQ(f.registers.buffer, 0x24008000u);
    EXPECT_EQ(f.registers.error, 2u);
    EXPECT_EQ(f.registers.remaining, 3584u);
    EXPECT_EQ(f.elapsed_ms, 17u);
}

TEST(SdIoDiagnostics, CapturesForegroundFailureAndRequiresExplicitReset) {
    FirstFailure capture;
    Registers registers;
    capture.Begin(false, 0, 1, 1, 0);
    capture.Finish(0, 1, registers);
    EXPECT_FALSE(capture.Snapshot().valid);
    capture.Begin(true, 0, 2, 1, 0);
    capture.Finish(1, 30000, registers);
    EXPECT_TRUE(capture.Snapshot().valid);
    EXPECT_FALSE(capture.Snapshot().irq);
    capture.Reset();
    EXPECT_FALSE(capture.Snapshot().valid);
    capture.ObserveIrq(registers);  // No transfer owns this interrupt.
    capture.Begin(false, 0, 3, 1, 0);
    capture.Finish(1, 9, registers);
    EXPECT_EQ(capture.Snapshot().sector, 3u);
    EXPECT_FALSE(capture.Snapshot().irq);
}

TEST(SdIoDiagnostics, RetainsIrqErrorEvenIfDriverReportsSuccess) {
    FirstFailure capture;
    capture.Begin(true, 0, 7, 1, 0);
    Registers registers;
    registers.error = 2;
    capture.ObserveIrq(registers);
    capture.Finish(0, 1, {});
    EXPECT_TRUE(capture.Snapshot().valid);
    EXPECT_EQ(capture.Snapshot().result, 0u);
}
