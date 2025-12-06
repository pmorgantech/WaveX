#include "config/logging_config.h"
#include "daisy_uart_link.h"  // for WaveX::Comm::s_hw
#include "daisy_seed.h"

#include <cstdarg>
#include <cstdio>

// Daisy-side logging helper: prefer s_hw->PrintLine if available, otherwise printf.
void wavex_daisy_log(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("%s", buf);
    } else {
        printf("%s\n", buf);
    }
}

void wavex_daisy_log_raw(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("%s", buf);
    } else {
        printf("%s", buf);
    }
}

