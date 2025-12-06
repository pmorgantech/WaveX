#include "daisy_seed.h"
#include "../comm/daisy_uart_link.h"

extern "C" void wavex_sd_log(const char* msg)
{
    using namespace WaveX::Comm;
    if(s_hw)
        s_hw->PrintLine("%s", msg);
    else
        printf("%s\r\n", msg);
}


