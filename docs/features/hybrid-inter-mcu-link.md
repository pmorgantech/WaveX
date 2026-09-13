# Hybrid inter-MCU link — UART controls and SPI data

**Status:** Proposed design, 2026-09-13; no hybrid firmware implemented or
hardware-tested. The experiment baseline is commit `921151f81c1cd07e7042e4a5ab7476e1de0be258`,
preserved on `experiment/mcu-link-switch` and fast-forwarded into `develop`.
Design work continues on `experiment/mcu-link-hybrid`.

Keep musical controls and confirming replies on UART. Use SPI first for Daisy
to ESP waveform envelopes and stereo meters, then extend it to diagnostics and
other bulk responses after their delivery semantics are settled. This is an
authorized Stage A transport experiment supporting the current Phase 2; it
neither adds a roadmap phase nor closes the existing audio/storage gates.

## Contents

- [Evidence and objective](#evidence-and-objective)
- [Data and ownership](#data-and-ownership)
- [Routing policy](#routing-policy)
- [Build selection](#build-selection)
- [Scheduling and backpressure](#scheduling-and-backpressure)
- [Diagnostics require different treatment](#diagnostics-require-different-treatment)
- [Startup and recovery](#startup-and-recovery)
- [Implementation sequence and acceptance](#implementation-sequence-and-acceptance)
- [Related](#related)

## Evidence and objective

The next experiment compares faster UART before implementing the split; see
[UART baud and eight-voice findings](../uart-baud-notes.md). A clean faster
single link may avoid the additional routing and recovery state.

The [SPI findings](../spi-notes.md#measured-cutover-comparison) are the durable
measurement record, including failed trials and image hashes. The final
nominal 32 MHz control trial measured 2.282 ms mean / 2.982 ms p95 / 4.011 ms
maximum during playback, versus UART's 0.907 / 1.038 / 1.450 ms reference.
These are acknowledged Instrument AMP round trips, not audible latency.
No completed heavy-load comparison exists for that final SPI configuration.
Earlier completed runs do not show a repeatable reduction in callback peaks.

The objective is to remove waveform serialization and periodic display traffic
from the UART reply queue while preserving UART's command path. SPI has much
more raw byte capacity, but the existing small waveform chunks underfill its
fixed slots. For the documented 1,140-column stereo example, eighteen slots
at nominal 32 MHz require about 9.792 ms of wire time, versus 25.5 ms for its
5,100 UART-framed bytes. These are calculations without scan, queue or dispatch
time; do not promise a twentyfold improvement in waveform completion.

Keep physical framing and chunk size unchanged for the first comparison.
Larger chunks or multiple packets per slot are later measured changes and must
also preserve bounded ESP decode work. No ITCM relocation, shared PLL retuning,
SPI role reversal, or DSP optimization is part of this design.

## Data and ownership

The shared payload catalog in
[`protocol.h`](../../firmware/shared/spi_protocol/protocol.h) remains the wire
source of truth. UART framing and SPI slot ownership stay in their existing
shared codecs. A small shared routing policy will map **message type and
sender direction** to a transport and delivery class. Packet size alone never
determines routing. Application handlers keep their current payloads and do
not choose a UART or SPI driver.

| Entity | Identity and lifetime | Sole owner / invariant |
|---|---|---|
| Control transaction | Existing application request ID, Track/Instrument identity and revision where provided; pending until an authoritative completion or timeout | Daisy application owns mutation; ESP owns pending presentation. Sending or finishing DMA does not acknowledge application success. |
| Waveform run | Sample ID, content generation, frame window, column count, channels and encoding; superseded by a newer requested view | Existing Daisy `EnvelopeScan` owns one cursor and retained packet. ESP stages a complete matching run before publishing to its cache. |
| Meter snapshot | One latest pending value per supported meter type; independent of control requests | Daisy foreground publishes an immutable copy. Only a pending, unowned copy may be replaced; an active DMA frame never changes. |
| Diagnostic interval | Interval duration plus deltas, extrema and current levels | One foreground collector owns destructive counter reads. An interval is not a replaceable meter value. |
| Link frame | Transport, direction, local sequence and immutable bytes until driver completion | Each adapter owns its own sequence tracker, queues and DMA lifecycle. No cross-link sequence ordering is inferred. |
| Bulk availability | Disabled, starting, online or faulted for the current peer session | UART health is independent. An unavailable SPI path must not make the musical control link unavailable. |

On Daisy, both adapters remain foreground-owned and feed the existing
`ProcessInterMcuMessage` dispatcher. Hybrid v1 accepts application commands
only from UART. Audio continues to consume the existing immutable control
handoffs; it never performs transport work.

On ESP, preserve **one application receive-dispatch owner**. Reuse the UART
link task as that owner initially: the SPI task validates completed framing,
copies into a fixed ingress queue, and wakes the owner through a bounded
notification mechanism. Only that owner invokes `PacketRouter` and listeners.
Do not simply connect both live driver tasks to the same router: existing
callback and shared-statistics code was exercised with one receive caller.
Initialize router callbacks once, outside either driver's repeated startup.

The SPI ingress producer and UART-task consumer form a fixed single-producer,
single-consumer queue with explicit release/acquire publication. Start with two
maximum-payload records, account for their RAM in both build maps, and retain a
completed SPI RX buffer without rearming if the queue is full. No receive
record points into a rearmed DMA buffer. Listener teardown and the rule that
communication callbacks never acquire the LVGL lock remain intact.

## Routing policy

This table is the proposed first split, not an additional wire-format catalog.
The implementation must enumerate supported message types in one shared policy,
validate direction, and test coverage against the central catalog. Unsupported
or wrong-link traffic is rejected and counted, not silently rerouted.

| Traffic | Hybrid route | Admission / delivery rule |
|---|---|---|
| ESP to Daisy notes, CCs, transport, MIDI clock, parameter edits, load/save, subscriptions and read requests | UART | Preserve existing ordering and request IDs. Waveform requests also use UART. |
| Confirming state, edit results, errors, sample metadata, Track bindings, file status, storage mount/loss and audition stop replies | UART | Keep the reply needed to confirm a control on the same low-latency path. Never coalesce a completion away. |
| Heartbeat, sync and Daisy MIDI clock/transport output | UART | Heartbeat is also backend liveness/uptime; SPI faults must not make a running engine look disconnected. CPU fields already in heartbeat remain there. |
| `MSG_ENVELOPE_CHUNK`, Daisy to ESP | SPI | Retained producer packet, bounded ingress, complete-run validation and bounded rerequest on loss. No silent incomplete waveform publication. |
| `MSG_METER_PUSH`, Daisy to ESP | SPI | Latest pending value, current producer cadence, stale indication on loss. |
| `MSG_DIAG_PUSH`, Daisy to ESP | UART initially; SPI after the diagnostic milestone | Preserve interval accounting before moving it. The existing subscription remains a UART command. |
| `MSG_SEQ_PLAYHEAD`, progress/status and editor readbacks | UART initially | Some types mix display progress, busy state and retained results. Move only after separating replaceable observations from confirmation semantics. |
| Browse responses and Sample Pool pages | UART initially; later SPI candidates | Define request/session correlation first. Browser invalidation and authoritative metadata ordering must survive cross-link reordering. |
| `MSG_MIX_METERS` | Future SPI candidate | A wire type exists, but the current backend source still identifies its sender as future work. This transport change does not implement the mixer. |
| ESP to Daisy sample-byte upload | Deferred | Keep the existing route in compatibility modes; do not introduce an unverified bidirectional bulk path in hybrid v1. Reject unsupported hybrid uploads explicitly. |

Existing explicit ACKs, when used, refer to their originating transport's
sequence space. Do not send a bare SPI sequence number as an ambiguous UART
ACK. Waveform retry uses the existing application request identity; a generic
cross-link retransmission protocol is outside the first split.

A delayed SPI chunk cannot override newer UART metadata or resurrect an
unloaded sample. Validate against the current sample generation and requested
window before accepting it. If metadata needed to validate a run has not yet
arrived over UART, retain only bounded staging or discard and request again;
arrival order across the two links is not authoritative ordering.

## Build selection

Extend the centralized selector to three explicit build modes: **UART all**,
**SPI all**, and **hybrid**. Keep UART all as the default and SPI all as the
saved comparison mode. Derive peripheral/DMA enablement from this one selection;
do not independently toggle driver booleans into inconsistent combinations.
Macro names, values, validation and electrical settings belong exclusively in
[`link_config.h`](../../firmware/shared/config/link_config.h) and
[`hardware_config.h`](../../firmware/shared/config/hardware_config.h).

Both boards must use the same selection and SPI electrical profile. Existing
UART/SPI override builds need a documented compatibility mapping or an explicit
compile error when old and new selectors conflict. Rebuild into fresh build
directories and verify the actual images as described in
[flashing](../flashing.md#uartspi-comparison).

The implementation impact includes the Daisy transport facade and startup,
UART/SPI translation-unit guards, UART DMA guards, ESP startup/send selection,
and application-context router injection. `LinkTxIdle()` needs a destination-
aware replacement: waveform admission must inspect SPI availability, not wait
for UART to become idle. Application code asks whether its message can be
admitted; the shared routing policy chooses the transport.

## Scheduling and backpressure

Daisy services UART controls and completions first, preserves streaming refill
priority, then performs bounded waveform scan/telemetry and SPI service. Each
SPI pump may retire a completed frame and launch at most one new frame; it
never waits for READY or completion. Long operations may pump TX on both links
without recursively dispatching another command.

In hybrid v1 all nonempty SPI data originates on the Daisy master. It therefore
knows when a transfer is necessary: launch when data is pending **and** the ESP
has armed READY. Stop continuous empty polling. Keep the existing falling-edge
ATTN generation and high-level READY checks; READY is still descriptor ownership,
not a data-available notification. ESP transmits empty return slots. An idle ESP descriptor may remain armed
indefinitely: expiry of the driver result-wait interval is not a transfer
failure and must not cause rearming, buffer reuse or repetitive error logs.
This avoids
using MISO for application replies, but does not prove the return-path electrical
problem fixed or remove the need for READY/CS and MOSI integrity testing.

Schedule outside the adapter's immutable DMA queue. Keep one latest unowned
snapshot per active telemetry type and the existing retained bulk producer
packet. Admit at most one application packet ahead of the active SPI frame.
When both classes remain pending, alternate one telemetry packet and one bulk
packet; round-robin the due telemetry types. A bulk transfer can then delay a
due meter by a bounded number of slots, while telemetry cannot starve bulk.
Preserve subscription/cadence limits rather than generating telemetry at bus
speed. Report replaced snapshots separately from failed bulk admissions.

The ESP dispatch owner services a bounded UART batch, then at most one SPI
record, and checks UART again. It never drains an unlimited bulk queue before
handling controls. Measure the longest single SPI decode/listener operation:
separate wires still share CPU, memory buses, interrupts and application service.
Do not add a common transport mutex that lets SPI completion waits block UART.

Stage A's UART and SPI use distinct DMA streams. Preserve the current
cache-safe buffers, ownership rules and audio-first IRQ priorities in
[architecture §7](../architecture.md#7-dma-cache-and-timing-rules-normative).
The existing Stage B SPI CV conflict remains a separate hardware/driver gate.

## Diagnostics require different treatment

`DiagPushTick` reads and resets counters before attempting to send; an ignored
failed enqueue can already lose an interval. Blind latest-value replacement
would additionally hide underruns, queue overflows or SD errors. Fix that
accounting before moving diagnostic traffic to SPI.

Use one retained interval packet. While it is pending, do not destructively
collect another interval; counters accumulate until admission is possible,
and the next report uses the actual elapsed interval. Keep collection and
admission times separate. Preserve peak/minimum fields and do not average
averages without their sample weights. Audit other logging consumers of
`TakeLinkPerf`/reset-on-read counters so they cannot steal the diagnostics
window; collect once and distribute immutable snapshots.

SPI completion still cannot prove ESP received a valid diagnostic interval.
Expose transport gaps/stale age and local drop/replacement counters; do not
present missing observations as zero errors. Before diagnostic SPI adoption,
choose and test a continuity indicator for interval reports, or move to
cumulative counters with receiver-computed deltas. Any added fields/messages
must be defined in `protocol.h`, mirrored in the protocol document and covered
by round-trip tests first. Do not claim lossless telemetry from a local queue
fix alone.

Keep UART and SPI performance counters distinct. The existing `DiagPushMessage`
has one link field group; in initial hybrid builds it continues to describe
UART, while SPI counters are available through the debug console. A later
versioned diagnostic extension can expose both explicitly. Foreground elapsed
service time includes audio preemption and is not exclusive CPU utilization.

## Startup and recovery

Start UART independently of SPI. A failed SPI initialization leaves controls,
heartbeats and error reporting usable; waveform work reports unavailable and
meters become stale. Do not fall back by injecting queued bulk onto UART and
reintroducing the congestion the split is intended to remove.

A production-capable split needs an explicit UART-managed bulk session before
SPI data publication. Model that session with peer boot/session identities,
a routing revision, readiness and an error state. Bind received bulk to the
accepted session; application sample IDs can repeat after a backend reboot and
are not sufficient reboot protection. The existing heartbeat and READY signals
alone do not negotiate a hybrid session.

Settle the minimal session wire extension before the waveform/recovery milestone:
UART opens/confirms the session, and SPI data must carry a verifiable session
identity or follow a proven quiesce-and-drain barrier that excludes all old
frames. Prefer an explicit identity if the barrier requires intricate timing.
Define it centrally in `protocol.h`; do not repurpose reserved flags, reinterpret
an existing timestamp, or add private bytes to the SPI padding. Account for any
session envelope in payload limits before implementing it.

Until that handshake and its tests exist, a meters-only matched-boot bench spike
is permissible, but it is not the finished hybrid cutover. On any peer reset or
SPI fault, disable bulk publication and invalidate its staged work. Do not
resume based only on READY returning high. UART control recovery remains
independent. SPI recovery must retain hardware-owned memory until DMA stops or
the ESP driver returns its descriptor; if this cannot be established, leave
SPI faulted and continue UART. No whole-DMA-controller reset is allowed.

## Implementation sequence and acceptance

1. **Selection and ownership seam.** Add the shared directional routing policy,
   three build modes, independent lifecycle/status and a single ESP dispatch
   owner. Keep application traffic on UART while proving both drivers can be
   enabled safely. Host-test route coverage, wrong-link rejection, independent
   sequence wrap, bounded ingress and SPI init failure with UART still usable.
   Build both boards in all three modes and record RAM/task-stack headroom.
2. **Meter split.** Route only current stereo meters to demand-driven SPI with
   coalescing and bounded admission. Verify no continuous idle SPI traffic,
   instrument edits over UART, and SPI failure/stop without a UART stall.
3. **Waveform split and session safety.** Finalize the session contract above,
   add its round-trip tests, then move envelopes using destination-aware
   admission. Verify complete data, superseded requests, metadata reordering,
   full queues, corruption, timeout and either peer rebooting mid-transfer.
   Retry only reads with a fixed limit; never replay note events, toggles or
   file mutations simply because a response was lost.
4. **Measured comparison.** Rebaseline UART all, compare hybrid, and repeat UART
   all with identical images apart from selection and identical instrumentation.
   Timestamp the same acknowledged AMP request path, including admission, with
   at least 100 idle and 100 playing edits per run; add sustained waveform
   navigation, meter traffic and control bursts. Record mean/p95/max and failed
   admissions, completion rate, waveform goodput/completion time, telemetry age,
   queue high-water marks, per-link errors and audio callback average/peak.
   Acceptance requires no repeatable worsening of UART control latency, no
   control loss/stuck notes, and useful bulk improvement. Observed maxima are
   sample maxima, not guaranteed worst-case latency bounds.
5. **Diagnostic and broader bulk candidates.** Resolve interval continuity and
   implement separate per-link metrics, then evaluate browse/Pool pages after
   request correlation. Keep progress/confirmation types on UART until a safe
   split is explicit. Repeat fault and concurrency tests for each added class.

Use the previously successful SPI electrical profile as an initial bench
candidate, not a production rating; its provenance is in
[the return-path investigation](../spi-notes.md#return-path-investigation).
The earlier SD-write failures remain open. Run the full heavy DSP/streaming,
live-edit and repeated file-operation workload, followed by the required
one-hour zero-underrun soak and peer-reset/fault tests before production
adoption. A clean short meter or waveform check does not close those gates.

## Related

- [Project principles](../project-principles.md): audio first, explicit ownership,
  immutable handoffs, incremental scope and measured claims.
- [Architecture](../architecture.md): as-built transport and mandatory DMA rules.
- [Inter-MCU protocol](inter-mcu-protocol.md): payloads and waveform identity.
- [SPI findings](../spi-notes.md): measured comparisons and rollback evidence.
- [Roadmap](../roadmap.md) and [backlog](../backlog.md): phase and adoption gates.
