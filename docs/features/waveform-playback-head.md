# Waveform playback head

**As built, Phase 2 follow-up.** Browser, Sample Edit and Record share a vertical
playback line through `EnvelopePanel` / `WaveformView`. Both stereo lanes share
one head. Loop splice halves omit it because they show disjoint time windows.
Physical validation is tracked in [HV-010](../hardware-validation.md#hv-010--waveform-playback-head);
host checks do not close the [Phase 2 gate](../roadmap.md#phase-2--groovebox-core-sequencer-and-pads).

## Identity and display rule

The displayed Pool sample ID and generation are authoritative; these pages
currently have sample context, not a specific Track or Zone. A matching
streamed audition takes precedence, including silent loop gaps (no line during
the gap). Otherwise the newest active RAM voice using that sample wins, across
primary and secondary sources. An older matching voice can resume when the
newer one ends. No matching playback means no line. RAM reports its current
block-boundary source phase, clamped/wrapped to the source region; streaming
reports the last source frame consumed into the audio block.

Map absolute source frame through the visible `[start, end)` window using
integer arithmetic. Out-of-window positions disappear. Clear on sample change,
missing data, stop, page exit or stale telemetry. Current forward playback,
pitch/rate changes, retriggers and loop wraps follow engine positions; no
wall-clock extrapolation is used. Reverse playback and Track/Zone-specific
views remain future features (see the [requirements](../architecture-notes.md#playback-head-in-waveform-views)).

## Ownership and handoff

The wire contract lives in [protocol.h](../../firmware/shared/spi_protocol/protocol.h)
and the [protocol catalog](inter-mcu-protocol.md): `MSG_SAMPLE_PLAYHEAD` carries
an 8-byte request and a 16-byte reply, strictly size/identity/reserved-field
validated. A request carries nonzero request ID, sample ID and generation;
a reply echoes them with source frame and idle/voice/stream state.

The foreground resolves a request to a comparison-only PCM pointer and matching
stream epoch, then publishes a bounded snapshot. Once per new request, the
audio callback checks its consumed stream position or scans the fixed voice
array and publishes a reply snapshot. It does no UART, I/O, allocation or
waiting. The foreground rechecks Pool identity and stream epoch before sending;
a full TX queue may drop display telemetry. The stream epoch changes with the
ring disabled, so positions cannot cross stream restarts.

Streaming provenance uses a parallel 2,048-frame tag ring plus 1,024-frame
prebuffer (12 KiB CPU-only SRAM). Tags and PCM share the same SPSC head/tail
publication and reuse. Foreground generates tags from the same local phase
walk as the resampler, retaining an integer absolute frame for long files and
the previous chunk's actual history frame across loop rewinds. Silence tags
hide the head. The callback reads only the last consumed tag per block.
Neither these arrays nor the PCM ring are DMA buffers. Existing SD progress
notifications retain their legacy read-ahead semantics and do not drive this line.

ESP32 RX copies the latest reply under a short mailbox lock. The UI asks at
most every 50 ms, one request in flight, with a 200 ms request timeout and a
200 ms age limit since the last timely reply. Request IDs survive page reentry;
a boot-random seed reduces collision with replies from a previous boot.
Mismatched or late replies cannot revive a stale line. No subscription remains
after page exit; invisible splice views do not poll. Cadence is an initial
bounded setting, pending the link/panel measurements in HV-010.

## Rendering and verification

The line is a noninteractive, opaque two-pixel LVGL child using a named theme
colour distinct from the waveform. UI-task code changes only its visibility
and pixel x position; identical positions are no-ops. Existing waveform data
stays cached, with no refetch per movement. Trim/loop handles remain above it.

Host tests cover wire/router validation, large source positions, resampler
chunk/history mapping, newest voice/secondary/loop/stop selection, stale and
wrong-context replies, timer wrap, splice suppression, stereo/zoom pixels and
narrow old/new repaint regions in real LVGL. Device builds check integration
and memory fit. DWT overhead, SD streaming under load, UART latency and physical
panel tracking remain unmeasured until HV-010 is run.
