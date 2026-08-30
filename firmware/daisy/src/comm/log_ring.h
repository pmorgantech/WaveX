#pragma once

// Non-blocking log transport for the Daisy.
//
// libDaisy's Logger is unusable on this firmware's main loop. After two
// successful packets it latches into blocking mode (hid/logger.cpp:73) and
// every subsequent line goes through
//
//     while(false == impl_.Transmit(buffer, bytes)) {}   // logger.h:113
//
// an unbounded spin with no timeout, waiting for the USB host to drain the
// CDC endpoint. That happens even with StartLog(false): `wait_for_pc` only
// sets the initial counter, which then increments on success. The main loop
// is the only thing refilling the audio ring, so from the third log line of
// any boot onward, log volume is coupled directly to audio dropouts, bounded
// only by how fast the host reads the port. That is the mechanism behind the
// audition underruns - every reduction in log volume improved audio because
// every line was a potential unbounded stall.
//
// This replaces it with the usual arrangement: writers append to a ring and
// never block, and the main loop drains a bounded amount per pass with the
// non-blocking primitive (CDC_Transmit_FS returns USBD_BUSY rather than
// waiting). If the host stops reading, output is dropped and counted - the
// audio never pays for it.

#include <cstddef>
#include <cstdint>

namespace daisy {
class DaisySeed;
}

namespace WaveX {
namespace Log {

// Registers the hardware used for draining. Until this is called (and after
// a null is passed) writes still accumulate, so early boot logging is kept.
void Init(daisy::DaisySeed* hw);

// Appends bytes. Never blocks. On overflow the OLDEST buffered bytes are
// dropped so the newest output - normally the interesting part when
// something is going wrong - survives, and the loss is counted.
//
// MAIN-LOOP CONTEXT ONLY. The ring is not SPSC-safe: this function advances
// the read index as well as the write index on overflow, and Drain() advances
// it too, so two contexts writing it concurrently corrupts the ring rather
// than losing a line. Calls from exception context are refused and counted
// (see IsrWrites()) instead of being allowed to do that damage.
void Write(const char* data, size_t len);

// printf-style helpers. Formatting happens on the caller's stack; only the
// result touches the ring.
void Printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void PrintLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Transmits at most one USB FS packet per call and returns immediately if
// the endpoint is busy. Call once per main-loop pass.
void Drain();

// Bytes discarded through overflow since boot. Non-zero means the host was
// not keeping up, and the log has gaps.
uint32_t DroppedBytes();

// Write() calls refused because they came from exception context. Must stay
// zero: a non-zero value means something on an ISR path started logging, and
// names a real bug at that call site rather than here.
uint32_t IsrWrites();

}  // namespace Log
}  // namespace WaveX
