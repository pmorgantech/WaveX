#include "config/logging_config.h"
#include "daisy_seed.h"
#include "daisy_uart_link.h"  // for WaveX::Comm::s_hw
#include "log_ring.h"

#include <cstdarg>
#include <cstdio>

// Daisy-side logging helper. Routes through the non-blocking ring (log_ring.h)
// rather than DaisySeed::PrintLine, whose libDaisy Logger spins unbounded
// waiting for the USB host and stalls the audio ring refill.
void wavex_daisy_log(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    WaveX::Log::PrintLine("%s", buf);
}

void wavex_daisy_log_raw(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    WaveX::Log::Printf("%s", buf);
}
