#pragma once

// Optional bench transport, one foreground writer. Never use RTT from an ISR.
// Both the control block and buffers live in libDaisy's non-cacheable D2 region;
// debugger memory accesses bypass the Cortex-M7 cache. No global cache changes.
#define SEGGER_RTT_SECTION ".sram1_bss"
#define SEGGER_RTT_BUFFER_SECTION ".sram1_bss"
// This entire MPU region is uncached. The library's cache-alias mode bypasses
// its section-placement macros, so use explicit alignment and section mode.
#define SEGGER_RTT_CPU_CACHE_LINE_SIZE 0
#define SEGGER_RTT_ALIGNMENT 32
#define SEGGER_RTT_BUFFER_ALIGNMENT 32
#define SEGGER_RTT_UNCACHED_OFF 0
#define SEGGER_RTT_MAX_NUM_UP_BUFFERS 1
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS 1
#define BUFFER_SIZE_UP 8192
#define BUFFER_SIZE_DOWN 32
#define SEGGER_RTT_MODE_DEFAULT SEGGER_RTT_MODE_NO_BLOCK_TRIM
#define RTT_USE_ASM 0
// Selecting the C writer bypasses SEGGER's architecture auto-detection. Keep
// the M7 publication barrier: buffer bytes must reach RAM before WrOff does.
#define RTT__DMB() __asm volatile("dmb" ::: "memory")
// The caller enforces foreground-only access. Do not mask the audio interrupt
// around a memcpy; the probe owns RdOff and the foreground owns WrOff.
#define SEGGER_RTT_LOCK()
#define SEGGER_RTT_UNLOCK()
