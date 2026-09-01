---
name: esp32p4
description: Design, implement, or review ESP32-P4 / ESP-IDF embedded C++ with SMP, ISR, IRAM, DMA, cache, and task-architecture constraints.
---

# ESP32-P4 platform skill

Use this skill for any WaveX code that runs on the ESP32-P4 frontend, especially FreeRTOS tasks, ISRs, IRAM-safe handlers, UI/peripheral DMA, SPI/display/storage paths, and cross-core communication.

Before making design decisions, edits, or reviews, read the complete
[`docs/project-principles.md`](../../docs/project-principles.md) and
[`docs/esp32p4_coding_guide.md`](../../docs/esp32p4_coding_guide.md). The
principles are the architectural constitution and the platform guide is the
detailed source of truth for ESP32-P4 constraints; do not duplicate or casually
override either one.

Apply these principles while working:

- Treat ESP32-P4 as an SMP system. Use atomics, fixed SPSC rings, FreeRTOS primitives, or short `portMUX_TYPE` critical sections as appropriate; disabling interrupts on one CPU does not protect the other CPU.
- Keep ISRs minimal and bounded. An `ESP_INTR_FLAG_IRAM` ISR requires a completely IRAM/DRAM-safe transitive call and data graph; no flash/DROM code, PSRAM data, or virtual dispatch may be reached.
- Choose memory by capability and ownership. Use the correct internal DMA or supported PSRAM DMA capabilities and alignment, never task-stack DMA buffers casually, and do not modify buffers while hardware owns them.
- Separate CPU synchronization from DMA cache coherency. Atomics/fences do not replace `esp_cache_msync()`; first follow the individual ESP-IDF driver's cache and buffer-ownership contract before adding raw synchronization.
- Prefer a small, documented task architecture, task notifications for simple wakeups, asynchronous state machines over polling/sleeps, and initialization-time allocation over heap churn in latency-sensitive paths. Keep exceptions disabled and avoid RTTI-dependent designs.
- Use GPTimer rather than `esp_timer` when strict hardware timing is required, and measure ISR/task/DMA/display behavior with ESP-IDF timing and high-water-mark facilities before claiming improvements.

## Review gate

For every significant ESP32-P4 review:

- Apply the decision filter in `docs/project-principles.md`. Cite the relevant
  principle number(s) on each constitutional finding and identify the concrete
  failure mode in the reviewed scope.
- Trace whether UI, storage, logging, or transport work can disturb audio or
  protocol delivery; then verify one owner/one writer, immutable snapshots
  across tasks/cores/ISRs, bounded queues, and explicit buffer lifetimes.
- Check that UI and application logic are independent of UART, GPIO, display,
  and storage details; that drivers/backends own hardware details; and that
  tasks and modules have one understandable responsibility.
- Prefer ESP-IDF or existing project facilities to custom synchronization,
  transport, and DSP code, and require measurements for ISR, task, DMA, cache,
  queue-depth, memory, or display performance claims.
- Check roadmap phase alignment and require architecture/feature documentation
  to change with intentional design changes. Record unrelated architectural
  debt in the roadmap or backlog instead of expanding the reviewed patch.

Only report concrete, actionable findings; the principles are not a reason to
manufacture style objections or demand a big-bang refactor. For review or
implementation completion, also check the guide's ESP32-P4 checklist and report
host/compile verification separately from real-panel, DMA, timing, memory, and
hardware verification.
