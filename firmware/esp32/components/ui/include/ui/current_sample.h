// WaveX current-sample: which resident sample Sample Edit acts on
#pragma once

#include <cstdint>

namespace wavex_ui {

/// The sample Sample Edit shows and edits. Set by the Sample Browser's Load
/// and by the Sample Manager's Edit softkey (track-and-patch-model.md §6), so
/// Sample Edit can act on any resident sample rather than only the Browser's
/// most recent load. Survives page navigation, like SampleBrowserState.
/// 0 means none.
uint16_t getCurrentSampleId();
void setCurrentSampleId(uint16_t sample_id);

}  // namespace wavex_ui
