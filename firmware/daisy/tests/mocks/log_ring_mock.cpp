// Host stub for the non-blocking log transport (src/comm/log_ring.h).
//
// The real implementation talks to the USB CDC endpoint through DaisySeed, so
// it cannot link on the host. Tests that compile real firmware translation
// units only need the symbols to exist - log output is not under test - so
// these discard everything. Kept as a stub rather than routing to stdout so
// test output stays readable.

#include "comm/log_ring.h"

namespace daisy {
class DaisySeed;
}

namespace WaveX {
namespace Log {

void Init(daisy::DaisySeed*) {}
void Write(const char*, size_t) {}
void Printf(const char*, ...) {}
void PrintLine(const char*, ...) {}
void Drain() {}
uint32_t DroppedBytes() {
    return 0;
}

}  // namespace Log
}  // namespace WaveX
