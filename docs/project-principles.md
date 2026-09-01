# WaveX Project Principles

**Status:** Living document

**Purpose:** Capture the fundamental engineering principles behind WaveX and
explain why its architecture exists.

All implementation and review decisions must support these principles. This
document is WaveX's architectural constitution; it does not replace the
as-built design in [`architecture.md`](architecture.md) or the implementation
order in [`roadmap.md`](roadmap.md).

## Contents

- [Core principles](#core-principles)
- [Decision filter](#decision-filter)
- [Coding-agent rules](#coding-agent-rules)
- [Success criterion](#success-criterion)

## Core principles

### Principle 1: Audio first

Everything exists to support uninterrupted audio. If a decision improves the
UI but risks audio stability, choose audio. Always.

### Principle 2: Deterministic execution

The firmware should behave identically given identical inputs. Avoid:

- Hidden state
- Timing dependencies
- Race conditions
- Dynamic behavior

### Principle 3: Separation of responsibilities

Each subsystem owns one responsibility. For example:

- `VoiceEngine` owns voice rendering.
- `Sequencer` owns musical timing.
- `OutputBackend` owns hardware output.

Subsystems should collaborate, not overlap.

### Principle 4: Hardware independence

DSP should never know the DAC type, codec type, UART, or GPIO. Hardware changes
should not require DSP changes.

### Principle 5: Explicit ownership

Every object has one owner and one writer. Shared mutable state is a design
failure.

### Principle 6: Immutable data flow

Whenever possible, data crosses execution domains as a complete immutable
snapshot:

```text
Producer -> Immutable Snapshot -> Consumer
```

Never pass partially updated state between execution domains.

### Principle 7: Small modules

Prefer 20 focused modules to two enormous managers. Modules should be
understandable independently.

### Principle 8: Incremental development

Every commit should compile, boot, and play audio. Avoid big-bang rewrites.
When hardware verification is unavailable, state that limitation explicitly;
compilation is not evidence that the firmware booted or played audio.

### Principle 9: Library first

Before writing code, check in this order:

1. libDaisy
2. DaisySP
3. CMSIS-DSP

Only write custom code when required.

### Principle 10: Measure before optimizing

Never optimize because "it feels slow." Measure callback time, CPU load,
memory, and queue depth first.

### Principle 11: Architecture before features

Adding features should not require redesigning the core. Future work should
plug into existing interfaces.

### Principle 12: Keep version 1 small

Version 1 proves architecture, stability, and sound quality—not feature count.
Finish the instrument before expanding it.

### Principle 13: Documentation is code

Architecture documents are part of the repository. Implementation should
follow documentation. If implementation changes, update the documentation in
the same commit.

### Principle 14: Favor simplicity

Prefer simple, predictable, and measured code over clever, abstract, and
magical code.

### Principle 15: Optimize for five years

The firmware should still make sense years later. Future contributors,
including coding agents, should understand the design quickly without
reverse-engineering architectural intent.

## Decision filter

Before merging any significant change, ask:

1. Does this improve the architecture?
2. Does this preserve deterministic audio?
3. Does it reduce coupling?
4. Does it simplify ownership?
5. Can another developer understand it six months from now?
6. Does it align with these principles?

If several answers are "no," redesign before merging.

## Coding-agent rules

These principles take precedence over implementation convenience.

When documentation and code disagree:

- Fix the code if the documentation is correct.
- Update the documentation if the design has intentionally evolved.

Do not invent new architectural principles without updating this document.
Code reviews should cite the principle number behind each constitutional
finding and still identify a concrete failure mode in the reviewed scope.

## Success criterion

Every major design decision made during the lifetime of WaveX should be
explainable by one or more principles in this document.

Related sources of truth:

- [`architecture.md`](architecture.md) — as-built and target system design
- [`roadmap.md`](roadmap.md) — implementation order and remediation ownership
- [`daisy_rt_audio_coding_guide.md`](daisy_rt_audio_coding_guide.md) — Daisy
  real-time rules
- [`esp32p4_coding_guide.md`](esp32p4_coding_guide.md) — ESP32-P4 platform rules
