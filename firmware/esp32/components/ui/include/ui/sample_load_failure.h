#pragma once

#include "spi_protocol/protocol.h"

#include <cstdint>

namespace wavex_ui {

// Shared by every frontend presentation of a SAMPLE_STATUS_LOAD_FAILED reply.
inline const char* sampleLoadFailureText(uint32_t reason) {
    using namespace WaveX::Protocol;
    switch (reason) {
        case SAMPLE_LOAD_FAIL_NO_SDRAM:
            return "sample memory is unavailable on the Daisy";
        case SAMPLE_LOAD_FAIL_OPEN:
            return "the Daisy could not open the file";
        case SAMPLE_LOAD_FAIL_FORMAT:
            return "not a resident-playable WAV (PCM16 mono/stereo)";
        case SAMPLE_LOAD_FAIL_RAM:
            return "does not fit in free sample RAM";
        case SAMPLE_LOAD_FAIL_READ:
            return "SD read error during the load";
        case SAMPLE_LOAD_FAIL_REGISTRY_FULL:
            return "too many samples resident - unload one";
        case SAMPLE_LOAD_FAIL_BUSY:
            return "another sample import is still running - try again when it finishes";
        default:
            return "unknown reason";
    }
}

}  // namespace wavex_ui
