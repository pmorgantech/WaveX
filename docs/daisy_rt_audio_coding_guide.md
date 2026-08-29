# Daisy / STM32H750 Real-Time Audio Coding Guide

Use this guide when designing or reviewing C++ code for Daisy Seed, STM32H750, libDaisy, CMSIS, and CMSIS-DSP.

The primary design goals are deterministic audio execution, correct DMA/cache behavior, bounded memory use, and predictable interrupt latency.

## 1. Real-time audio is the highest-priority workload

Treat the libDaisy audio callback as a hard real-time interrupt handler.

The callback must complete within:

    deadline = block_size / sample_rate

At 48 kHz:
- 4 samples = 83.3 us
- 8 samples = 166.7 us
- 16 samples = 333.3 us
- 48 samples = 1 ms

Missing this deadline produces audio underruns/glitches.

Inside the audio callback DO NOT:
- allocate or free memory
- grow STL containers
- access FATFS, SD card, QSPI flash, or filesystems
- print or log
- wait on a mutex, semaphore, queue, DMA operation, or task
- call blocking peripheral APIs
- perform unpredictable searches or data-structure traversal
- construct/destruct complicated objects
- initialize DSP objects
- perform cache maintenance over large arbitrary regions

The callback should primarily:
1. consume already-prepared control/state data
2. process fixed-size audio blocks
3. update small deterministic state machines
4. write output samples

Move everything else outside the callback.

libDaisy explicitly describes the audio callback as interrupt-context code with a deadline determined by sample rate and block size.

## 2. Understand STM32H750 memory before placing buffers

The STM32H750 is not a flat-memory machine.

Important regions include:

### DTCM

DTCM is directly connected to the Cortex-M7.

Use it for:
- hot DSP state
- envelopes
- oscillator state
- filter state
- small lookup/state structures
- frequently accessed non-DMA scratch buffers

Advantages:
- deterministic CPU access
- no D-cache involvement
- very low latency

Important: normal DMA1/DMA2 cannot access DTCM.

Do NOT put DMA1/DMA2 buffers in DTCM.

libDaisy provides:

    DTCM_MEM_SECTION

for this purpose.

### SRAM1/SRAM2/SRAM3

These are appropriate for peripheral DMA buffers.

libDaisy provides:

    DMA_BUFFER_MEM_SECTION

which currently maps DMA buffers into SRAM1 and is specifically intended for cacheless DMA-safe storage.

Use this instead of inventing arbitrary placement whenever possible.

libDaisy itself uses this mechanism for ADC DMA buffers.

### AXI SRAM / external SDRAM

Use larger memory for:
- sample storage
- waveform data
- large delay lines
- large working sets

These memories are reached through the bus/cache architecture rather than the Cortex-M7 TCM ports.

If CPU and DMA both touch the same cacheable region, explicit cache coherency may be required.

The H750 has distinct D1, D2, and D3 bus domains. DMA1/DMA2 reside in D2; MDMA, SDMMC1 and some high-bandwidth masters reside in D1; BDMA resides in D3. DMA reachability and bus placement therefore matter.

## 3. Cache coherency and DMA are separate from C++ synchronization

Cortex-M7 has 32-byte L1 cache lines.

CPU writes can remain dirty in D-cache while DMA reads stale RAM.

DMA writes can update RAM while the CPU continues reading stale cached data.

Atomics, `volatile`, mutexes and memory fences DO NOT fix this.

For cacheable DMA buffers:

### CPU -> DMA

Before DMA reads CPU-produced data:

    SCB_CleanDCache_by_Addr(...)

Then ensure ordering before starting the DMA transaction when required.

### DMA -> CPU

After DMA completes and before CPU consumes DMA-written data:

    SCB_InvalidateDCache_by_Addr(...)

Buffers and ranges should be aligned to complete 32-byte cache lines.

Never casually invalidate an unaligned cache region: unrelated dirty data sharing the first or last cache line can be destroyed.

Prefer one of these designs, in order:

1. use libDaisy's non-cacheable `DMA_BUFFER_MEM_SECTION`
2. dedicate complete cache-line-aligned buffers to DMA
3. perform explicit clean/invalidate operations
4. create MPU non-cacheable regions only where architectural reasons justify them

Do not disable D-cache globally to make DMA bugs disappear.

AN4839 defines the Cortex-M7's 32-byte cache lines and CMSIS clean/invalidate operations.

## 4. Buffer ownership must be explicit

Every DMA buffer must have an identifiable owner.

Prefer double buffering:

    CPU owns buffer A
    DMA owns buffer B

then exchange ownership.

Never allow CPU and DMA to modify the same buffer concurrently.

Useful states include:

    FREE
    CPU_FILLING
    READY_FOR_DMA
    DMA_ACTIVE
    READY_FOR_CPU

Represent ownership directly rather than relying on timing assumptions.

For audio/sample streaming, prefer fixed-size rings of buffers over a continuously malloc/free model.

## 5. C++ rules for real-time embedded code

Prefer compile-time structure.

Use:
- `constexpr`
- `consteval` when genuinely useful
- templates for fixed configuration
- `std::array`
- `std::span`
- `std::optional`
- strongly typed `enum class`
- trivially copyable command/state structures
- RAII for peripheral/resource ownership
- fixed-capacity custom containers where necessary

Prefer:

    std::array<Voice, 8>

over:

    std::vector<Voice>

when the maximum is known.

Templates and `constexpr` should move work to compile time without causing unreasonable code-size expansion.

### Heap policy

Prefer no general-purpose heap allocation after system initialization.

If dynamic allocation is necessary:
- perform it during initialization
- allocate pools up front
- use fixed-size arenas or object pools
- never allocate from audio/ISR context

Be suspicious of:
- `std::vector`
- `std::string`
- `std::function`
- associative containers
- shared ownership
- libraries hiding allocation internally

### RAII

RAII is desirable for ownership and cleanup.

However, destructors executed in real-time context must themselves be bounded and nonblocking.

Avoid architectures dependent on complicated destruction during active audio processing.

## 6. ISR communication

Do not use `volatile` as a synchronization primitive.

`volatile` is appropriate for hardware registers and certain externally modified objects. It does not provide atomicity or inter-thread ordering.

For tiny shared state use:

    std::atomic<uint32_t>
    std::atomic<bool>

Prefer acquire/release semantics when passing ownership or publishing data.

For ISR -> foreground/task communication, prefer:
- atomic flags/counters
- fixed-capacity SPSC queues
- ring buffers
- deferred processing

A typical SPSC ring should have:
- one producer only
- one consumer only
- fixed storage
- atomic read/write indices
- no locks
- no allocation

Use `static_assert(std::atomic<uint32_t>::is_always_lock_free)` where a lock-free implementation is required.

C++ memory ordering controls CPU-visible ordering. It does not substitute for M7 D-cache maintenance when DMA is involved.

## 7. State machines over blocking workflows

Peripheral and storage operations should generally be modeled as state machines.

Prefer:

    IDLE -> START_READ -> WAIT_DMA -> COMPLETE -> READY

over:

    start_operation();
    while (!done) {}
    process_result();

Audio code should never wait for another subsystem.

SD/sample streaming should maintain sufficient read-ahead buffering that temporary storage latency does not enter the audio deadline.

## 8. CMSIS-DSP policy

Prefer CMSIS-DSP primitives where they provide a tested and optimized equivalent of custom DSP loops.

For STM32H750, `float32_t` / `float` is normally the default DSP representation because Cortex-M7 provides a single-precision FPU.

Initialize CMSIS-DSP structures outside the audio callback.

Keep persistent filter/FFT/state buffers allocated permanently.

For performance builds, CMSIS-DSP currently recommends:

    -O3 -ffast-math

and explicitly recommends against `-fno-builtin` / `-ffreestanding` for performance-sensitive builds.

Ensure the build actually targets and enables the Cortex-M7 FPU rather than silently using software floating point.

For FFTs, prefer size-specific initialization such as:

    arm_rfft_fast_init_1024_f32()

instead of a generic initializer where the FFT size is known at compile time. This allows unused tables/code to be removed by the linker.

Place very hot non-DMA DSP state in DTCM where measurements justify it.

CMSIS-DSP explicitly recommends optimized compilation, fast memory, enabled cache, and DTCM for performance-sensitive data.

## 9. FreeRTOS, if used

Do not turn the audio callback into a normal RTOS task merely because FreeRTOS is available.

Keep the actual audio servicing in the libDaisy/SAI DMA interrupt path.

Use tasks for:
- SD-card streaming
- file management
- user-interface processing
- background sample preparation
- MIDI parsing where latency permits
- control-plane communication

ISR/task handoffs should be short and bounded.

Never take a normal mutex from the audio ISR.

## 10. Profiling is mandatory for performance claims

Do not optimize based solely on intuition.

Measure:
- worst-case audio callback cycles
- average callback cycles
- maximum callback duration
- DMA completion latency
- SD read latency distribution
- sample-buffer low-water marks
- queue overruns
- underruns

Useful techniques:
- toggle a GPIO/test point around the audio callback and measure with a scope/logic analyzer
- use Cortex-M7 DWT cycle counter for fine-grained timing
- maintain high-water/worst-case counters
- benchmark Release builds

Do not log continuously from the audio callback to measure performance; the measurement mechanism must not become the performance problem.

## 11. Code-review checklist

Before accepting real-time Daisy code, verify:

- No allocation occurs in audio/ISR context.
- No blocking operation occurs in audio/ISR context.
- No filesystem or logging occurs there.
- Audio processing has a measurable worst-case runtime below its deadline with margin.
- DMA buffers are in memory accessible by the selected DMA controller.
- DTCM is not accidentally used for DMA1/DMA2.
- DMA/cache ownership is explicit.
- Cacheable DMA buffers are cache-line aligned.
- Correct clean/invalidate operations surround CPU/DMA ownership transitions.
- Atomics or another legitimate synchronization mechanism protect shared CPU state.
- `volatile` is not being misused as synchronization.
- Queues/rings are bounded.
- DSP objects and buffers are initialized before audio starts.
- CMSIS-DSP routines are used where appropriate.
- Performance-sensitive changes include measurements.
- Failure modes produce silence/drop data/recover rather than blocking the audio path.
