# ESP32-P4 / ESP-IDF Embedded C++ Coding Guide

Use this guide when designing or reviewing C++ code for ESP32-P4 and ESP-IDF.

The primary goals are bounded latency, correct SMP synchronization, deliberate memory placement, safe DMA/cache interaction, and clean separation between interrupt and task-level processing.

## 1. Treat ESP32-P4 as a dual-core SMP machine

ESP-IDF FreeRTOS on ESP32-P4 supports symmetric multiprocessing.

Do not reason about concurrency as if this were a single-core microcontroller.

Disabling interrupts only affects the current CPU.

Therefore:

    disable_interrupts();

is NOT sufficient to protect data shared with the other CPU.

Use the appropriate mechanism:
- atomics for simple values/ownership
- SPSC queues for one-producer/one-consumer streams
- FreeRTOS synchronization primitives
- ESP-IDF spinlock critical sections for very short shared critical regions

ESP-IDF SMP critical sections use `portMUX_TYPE` spinlocks.

Keep critical sections extremely short. Do not block, yield, perform I/O, or call arbitrary FreeRTOS APIs while holding one.

## 2. Prefer explicit task architecture

Do not create tasks merely because a subsystem exists.

Prefer a small number of clearly owned execution contexts.

For latency-sensitive subsystems, consider explicitly pinning the relevant task to a core when doing so improves determinism.

Document:
- task name
- priority
- stack size
- core affinity
- producer/consumer relationships
- whether it may block
- maximum expected execution time

Prefer task notifications for simple task wakeups, including ISR-to-task wakeups.

Task notifications are lighter-weight than introducing queues/semaphores merely to signal that work exists.

Use queues when actual messages must be copied/queued.

Use fixed SPSC rings for high-frequency streams where exactly one producer and consumer exist.

## 3. ISR rules

An ISR should:
1. acknowledge/capture hardware state
2. copy minimal required data
3. advance a fixed buffer/ring
4. signal a task if deferred processing is needed
5. return

Do not:
- allocate
- perform filesystem access
- log heavily
- wait
- take ordinary mutexes
- execute complicated UI/business logic

Keep interrupt execution bounded.

## 4. IRAM-safe means the entire dependency graph is safe

If an interrupt is registered as `ESP_INTR_FLAG_IRAM`, placing the top-level ISR in IRAM is not sufficient.

Every function it can call must be accessible while flash cache is disabled.

Every datum it accesses must also be accessible.

An IRAM-safe ISR must not indirectly depend upon:
- code in flash
- constants in flash/DROM
- objects in PSRAM
- functions whose implementation eventually enters flash code

Use internal IRAM for code and internal DRAM for data.

Be particularly careful with C++ virtual calls: ESP-IDF places vtables in flash, so virtual dispatch must not be used from IRAM-safe interrupt handlers.

Do not mark an ISR IRAM-safe until its complete call graph and data graph have been audited.

## 5. Understand internal RAM vs PSRAM

Do not treat all pointers as equivalent.

Use capability-based allocation where memory properties matter.

Examples:

Internal DMA-capable memory:

    heap_caps_malloc(size,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

External PSRAM:

    heap_caps_malloc(size,
        MALLOC_CAP_SPIRAM);

For ESP32-P4 peripherals/EDMA that explicitly support DMA to PSRAM:

    heap_caps_malloc(size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);

Use appropriate alignment where required, including:

    MALLOC_CAP_CACHE_ALIGNED

or `heap_caps_aligned_alloc()` when a peripheral has stricter requirements.

`MALLOC_CAP_DMA` by itself normally selects DMA-capable internal memory. ESP32-P4 additionally supports DMA-capable external PSRAM for supported EDMA paths when the appropriate capabilities are requested.

Prefer static/fixed DMA buffers when practical.

Do not put DMA buffers casually on a task stack.

## 6. DMA and cache coherency

ESP32-P4 does not provide a hardware cache-coherent interconnect between CPU caches and DMA.

Therefore CPU and DMA can disagree about the contents of the same cached memory.

For raw shared DMA buffers outside guarantees provided by a particular ESP-IDF driver:

### CPU -> DMA

After CPU modifies data and before DMA reads it, synchronize cache to memory:

    esp_cache_msync(...,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

### DMA -> CPU

After DMA writes memory and before CPU reads it:

    esp_cache_msync(...,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

Respect cache alignment requirements.

Do not casually use the unaligned synchronization option. Synchronizing an unaligned range can affect adjacent cache-line contents and can silently destroy dirty data.

Prefer allocating buffers with the correct DMA/cache alignment in the first place.

First consult the individual peripheral driver's contract: many ESP-IDF drivers handle portions of the DMA/cache problem internally. Do not add redundant cache operations without understanding the driver's ownership semantics.

ESP-IDF documents `esp_cache_msync()` specifically for CPU/DMA coherency on ESP32-P4.

## 7. Buffer ownership must still be explicit

Even when the driver performs cache maintenance, software ownership remains your responsibility.

Prefer:

    FREE
    PRODUCER_FILLING
    READY
    DMA_ACTIVE
    CONSUMER_PROCESSING

over multiple subsystems sharing mutable buffers.

For SPI/display/storage/control traffic, use double or triple buffering where appropriate.

Never modify a buffer while hardware is transmitting from it unless the peripheral API explicitly permits this.

## 8. Embedded C++ policy

Prefer:
- `constexpr`
- templates for fixed configuration
- `std::array`
- `std::span`
- `std::optional`
- strongly typed enums
- fixed-capacity queues
- RAII wrappers around ESP-IDF handles
- clear ownership

Use dynamic allocation primarily during initialization.

Avoid continual heap churn in long-running embedded systems.

Be suspicious of hidden allocation in:
- `std::vector`
- `std::string`
- `std::function`
- shared ownership
- complicated STL graphs

ESP-IDF supports modern C++, but support does not imply suitability for deterministic paths.

Explicitly record the project's selected C++ standard rather than relying on whatever default a future ESP-IDF release chooses.

### Exceptions

ESP-IDF disables C++ exceptions by default.

Do not enable them merely for convenience.

Never throw exceptions from real-time critical code. ESP-IDF explicitly notes that exception handling/unwinding is orders of magnitude slower than ordinary error handling.

Prefer `esp_err_t`, status enums, `std::optional`, or expected/result-style return objects for normal embedded failures.

### RTTI

RTTI is disabled by default and increases binary size substantially when enabled.

Prefer static polymorphism/templates or explicit variants where practical.

### Virtual functions

Virtual dispatch is acceptable in ordinary task context if architecture warrants it.

Do not use virtual dispatch from IRAM-safe ISRs because vtables reside in flash.

## 9. Atomics, fences and cache operations solve different problems

Use `std::atomic` to synchronize CPU execution contexts.

Do not use `volatile` for task/task or task/ISR synchronization.

Acquire/release ordering is usually sufficient for producer/consumer publication patterns.

For example:

Producer:

    fill(buffer);
    ready.store(true, std::memory_order_release);

Consumer:

    if (ready.load(std::memory_order_acquire))
        consume(buffer);

However:

    std::atomic_thread_fence(...)

does NOT perform DMA cache maintenance.

Atomics solve CPU concurrency.

`esp_cache_msync()` solves CPU-cache/DMA coherency.

Do not confuse them.

## 10. Prefer state machines over blocking code

Peripheral code should generally operate as explicit asynchronous state machines.

Prefer:

    IDLE
      -> REQUESTED
      -> DMA_ACTIVE
      -> COMPLETE
      -> PROCESS
      -> IDLE

Avoid polling loops and arbitrary sleeps.

Do not use:

    while (!finished) {}

inside high-priority tasks.

Block on a notification/event when waiting is appropriate, or allow another subsystem to run.

## 11. Timers

Use the timing mechanism appropriate for the required determinism.

`esp_timer` is useful for software timers and general scheduling.

It is not the preferred mechanism for strict waveform-generation or tighter hardware-real-time requirements.

ESP-IDF recommends GPTimer when better real-time performance or hardware timer features are required.

Do not make timer callbacks perform substantial work. Signal a task where appropriate.

## 12. SPI and other peripheral DMA

Follow each driver's documented DMA requirements.

For SPI Slave specifically:
- maintain queued transactions ahead of demand
- use the documented host/device ready handshake when necessary
- maintain clear ownership of TX/RX buffers
- use DMA-aligned memory
- do not allow the host to begin before the slave is ready

ESP-IDF's SPI Slave documentation explicitly recommends a separate GPIO handshake when host/device readiness needs synchronization and supports DMA from appropriately allocated PSRAM on ESP32-P4.

## 13. Profiling

Do not optimize from guesses.

Measure.

Useful ESP-IDF facilities include:
- `esp_timer_get_time()`
- `cpu_hal_get_cycle_count()`
- FreeRTOS runtime statistics
- application tracing
- SEGGER SystemView
- GPIO timing with an oscilloscope/logic analyzer
- task stack high-water marks

`cpu_hal_get_cycle_count()` is per-core; use it from an ISR or task pinned to a known core when interpreting short measurements.

Measure:
- ISR maximum execution duration
- task runtime
- queue depths/high-water marks
- SPI transaction latency
- display update time
- DMA completion latency
- dropped events
- memory usage
- task stack margins

ESP-IDF specifically recommends measurement before optimization and documents cycle-count and task-runtime profiling facilities.

## 14. Code-review checklist

Before accepting ESP32-P4 embedded code, verify:

- Execution context for every function is understood: ISR, task, timer callback, startup, etc.
- ISR work is minimal and bounded.
- IRAM-safe ISRs have a completely IRAM/DRAM-safe transitive call/data graph.
- No virtual dispatch occurs from IRAM-safe ISRs.
- Shared dual-core state has legitimate synchronization.
- Interrupt disabling is not incorrectly used as cross-core synchronization.
- `volatile` is not being misused.
- High-rate producer/consumer paths are bounded.
- DMA buffers have the required memory capabilities and alignment.
- PSRAM DMA is used only on a path that supports it.
- CPU/DMA ownership is explicit.
- Cache synchronization is correct where the driver does not provide it.
- Heap allocation is absent from latency-critical paths.
- Exceptions are absent from real-time paths.
- Blocking operations do not occur in high-priority or interrupt contexts.
- Critical sections are very short.
- Timing/performance claims are backed by measurements.
