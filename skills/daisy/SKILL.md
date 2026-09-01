---
name: daisy
description: Design, implement, or review Daisy Seed / STM32H750 real-time audio code using libDaisy, CMSIS, and CMSIS-DSP constraints.
---

# Daisy platform skill

Use this skill for any WaveX code that runs on the Daisy Seed / STM32H750 backend, especially audio callbacks, SAI/DMA, SD/sample streaming, DSP, ISR communication, and performance-sensitive control code.

Before making design decisions, edits, or reviews, read the complete
[`docs/project-principles.md`](../../docs/project-principles.md) and
[`docs/daisy_rt_audio_coding_guide.md`](../../docs/daisy_rt_audio_coding_guide.md).
The principles are the architectural constitution and the platform guide is
the detailed source of truth for Daisy constraints; do not duplicate or
casually override either one.

Apply these principles while working:

- Treat the libDaisy audio callback as a hard real-time ISR. No allocation, filesystem or peripheral I/O, logging, blocking, unbounded traversal, or DSP initialization may enter it.
- Keep DMA ownership and memory placement explicit. DMA1/DMA2 cannot access DTCM; prefer `DMA_BUFFER_MEM_SECTION` for DMA buffers and `DTCM_MEM_SECTION` for hot non-DMA state. Account for 32-byte cache lines whenever cacheable memory is shared with DMA.
- Use bounded, preallocated buffers and lock-free or otherwise ISR-safe handoff mechanisms. `volatile` is not synchronization, and atomics do not perform cache maintenance.
- Prefer CMSIS-DSP kernels where they cover the operation, initialize persistent DSP state before audio starts, and measure callback/control-path changes with the DWT cycle counter before claiming a performance improvement.
- Model storage and peripheral work as deferred state machines. Preserve the current Stage A analog configuration and UART transport unless the task explicitly changes those architecture decisions.

## Review gate

For every significant Daisy review:

- Apply the decision filter in `docs/project-principles.md`. Cite the relevant
  principle number(s) on each constitutional finding and identify the concrete
  failure mode in the reviewed scope.
- Trace audio stability and determinism first; then verify one owner/one writer,
  immutable snapshots across callback, ISR, and main-loop domains, bounded
  queues, and explicit lifetime handshakes.
- Check that DSP is independent of codecs, DACs, UART, GPIO, and storage; that
  output and peripheral backends own hardware details; and that modules have
  one understandable responsibility.
- Check libDaisy, DaisySP, and CMSIS-DSP coverage before accepting custom DSP,
  and require DWT evidence for callback, control-tick, memory-placement, or
  kernel performance claims.
- Check roadmap phase alignment and require architecture/feature documentation
  to change with intentional design changes. Record unrelated architectural
  debt in the roadmap or backlog instead of expanding the reviewed patch.

Only report concrete, actionable findings; the principles are not a reason to
manufacture style objections or demand a big-bang refactor. For review or
implementation completion, also check the guide's Daisy checklist and report
host/compile verification separately from hardware audio, DMA, latency, and
underrun verification.
