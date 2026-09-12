#pragma once

#include "spi_protocol/protocol.h"

namespace WaveX {
namespace Sequencer {
// One supported-id list for the engine, editor and incoming edit validation.
// Unknown ids in older/newer files remain stored but have no audio effect.
inline bool IsVoiceLockParameter(uint8_t id) {
    using namespace Protocol;
    return (id >= PARAM_FILTER_CUTOFF && id <= PARAM_PITCH) || id == PARAM_GAIN ||
           id == PARAM_SAMPLE_START || id == PARAM_LOOP_START;
}
}  // namespace Sequencer
}  // namespace WaveX
