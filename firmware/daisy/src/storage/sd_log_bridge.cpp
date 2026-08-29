#include "../comm/daisy_uart_link.h"
#include "comm/log_ring.h"
#include "daisy_seed.h"

extern "C" void wavex_sd_log(const char* msg) {
    using namespace WaveX::Comm;
    if (s_hw)
        WaveX::Log::PrintLine("%s", msg);
    else
        printf("%s\r\n", msg);
}
