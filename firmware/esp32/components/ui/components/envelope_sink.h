// What an EnvelopePanel draws into.
//
// The panel core (envelope_panel.h) owns the request/fetch/render cycle and
// knows nothing of LVGL; it hands each finished view to one of these. On the
// target that is a WaveformView; in host tests it is a fake that records what
// it was given. A sink's column count is the width it wants the envelope at,
// so the cache can serve each view its own tier and no view is drawn from a
// coarser one than it asked for.
#pragma once

#include "envelope_cache.h"

#include <cstdint>

namespace wavex_ui {

struct EnvelopeSink {
    virtual ~EnvelopeSink() = default;

    // Columns this sink renders. Fixed for the sink's life.
    virtual uint16_t columns() const = 0;

    // A whole view's worth of columns, `count` of them, `channels` wide each
    // (count * channels EnvelopeColumns, channel-interleaved). Only called with
    // count == columns().
    virtual void setEnvelope(const WaveX::Protocol::EnvelopeColumn* columns,
                             uint16_t count,
                             uint8_t channels) = 0;

    // Nothing to show: no sample, or a new one whose data has not arrived.
    virtual void clear() = 0;
};

}  // namespace wavex_ui
