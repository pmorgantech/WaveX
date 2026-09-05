/**
 * @file logging_config.h
 * @brief WaveX leveled, per-module debug logging.
 *
 * Two gates stand between a log call and output, so "what is logged" can be
 * tuned finely without rebuilding, while hot paths can still compile logging
 * out entirely:
 *
 *  1. COMPILE-TIME CEILING (`WAVEX_LOG_CEILING_<MODULE>`). Calls above the
 *     ceiling are constant-folded away - zero code, zero cost. The default
 *     ceiling is TRACE (everything present in the binary); lower it per
 *     module only where the disabled-check itself is too expensive, which in
 *     practice means code that rides the audio or link hot path.
 *  2. RUNTIME LEVEL (`WaveX::Log::SetLevel`). A per-module byte checked at
 *     each call site. This is what the "WAVEX-LOG" console commands and
 *     scripts/wavex_log.py adjust, so a deep dive into one subsystem does
 *     not require a reflash and does not drag every other subsystem's
 *     chatter along.
 *
 * Emission is platform-appropriate: ESP32 routes through ESP_LOG (so IDF's
 * own per-tag runtime control and formatting apply, tag "WAVEX-<MODULE>"),
 * the Daisy through the non-blocking log ring (comm/log_ring.h - its
 * main-loop-only context rule applies to log calls exactly as before), and
 * host builds through printf.
 *
 * Levels match esp_log_level_t numerically so the two systems translate 1:1.
 */

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

static_assert(__cplusplus >= 201703L,
              "logging_config.h uses C++17 inline variables; compile first-party code as C++17");

// ============================================================================
// LEVELS (numeric equals esp_log_level_t: NONE..VERBOSE)
// ============================================================================

#define WAVEX_LOG_LEVEL_OFF 0
#define WAVEX_LOG_LEVEL_ERROR 1
#define WAVEX_LOG_LEVEL_WARN 2
#define WAVEX_LOG_LEVEL_INFO 3
#define WAVEX_LOG_LEVEL_DEBUG 4
#define WAVEX_LOG_LEVEL_TRACE 5

// ============================================================================
// MODULE TABLE - the single source of truth.
//
// X(name, default_runtime_level). Adding a module here is all it takes: the
// enum, name string, and default-level tables below are generated from this
// list, and the console parser / configurator enumerate it by name.
// ============================================================================

#define WAVEX_LOG_MODULE_LIST(X)            \
    X(SYSTEM, WAVEX_LOG_LEVEL_INFO)         \
    X(INTER_MCU_LINK, WAVEX_LOG_LEVEL_INFO) \
    X(UART_PROTOCOL, WAVEX_LOG_LEVEL_INFO)  \
    X(UART_PERF, WAVEX_LOG_LEVEL_INFO)      \
    X(SPI_LINK, WAVEX_LOG_LEVEL_INFO)       \
    X(AUDIO_ENGINE, WAVEX_LOG_LEVEL_INFO)   \
    X(STORAGE, WAVEX_LOG_LEVEL_INFO)        \
    X(SD, WAVEX_LOG_LEVEL_INFO)             \
    X(STREAM, WAVEX_LOG_LEVEL_INFO)         \
    X(CV, WAVEX_LOG_LEVEL_INFO)             \
    X(SEQUENCER, WAVEX_LOG_LEVEL_INFO)      \
    X(MIDI, WAVEX_LOG_LEVEL_INFO)

// ============================================================================
// COMPILE-TIME CEILINGS
//
// Per-module override: -DWAVEX_LOG_CEILING_<MODULE>=WAVEX_LOG_LEVEL_x, or
// define it before including this header. X-macros cannot emit #ifndef, so
// the defaults are written out.
// ============================================================================

#ifndef WAVEX_LOG_CEILING_DEFAULT
#define WAVEX_LOG_CEILING_DEFAULT WAVEX_LOG_LEVEL_TRACE
#endif

#ifndef WAVEX_LOG_CEILING_SYSTEM
#define WAVEX_LOG_CEILING_SYSTEM WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_INTER_MCU_LINK
#define WAVEX_LOG_CEILING_INTER_MCU_LINK WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_UART_PROTOCOL
#define WAVEX_LOG_CEILING_UART_PROTOCOL WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_UART_PERF
#define WAVEX_LOG_CEILING_UART_PERF WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_SPI_LINK
#define WAVEX_LOG_CEILING_SPI_LINK WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_AUDIO_ENGINE
#define WAVEX_LOG_CEILING_AUDIO_ENGINE WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_STORAGE
#define WAVEX_LOG_CEILING_STORAGE WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_SD
#define WAVEX_LOG_CEILING_SD WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_STREAM
#define WAVEX_LOG_CEILING_STREAM WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_CV
#define WAVEX_LOG_CEILING_CV WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_SEQUENCER
#define WAVEX_LOG_CEILING_SEQUENCER WAVEX_LOG_CEILING_DEFAULT
#endif
#ifndef WAVEX_LOG_CEILING_MIDI
#define WAVEX_LOG_CEILING_MIDI WAVEX_LOG_CEILING_DEFAULT
#endif

// ============================================================================
// RUNTIME LEVEL TABLE
// ============================================================================

namespace WaveX {
namespace Log {

enum class Module : uint8_t {
#define WAVEX_LOG_X(name, deflvl) name,
    WAVEX_LOG_MODULE_LIST(WAVEX_LOG_X)
#undef WAVEX_LOG_X
        Count
};

constexpr size_t kModuleCount = static_cast<size_t>(Module::Count);

// Written by the control path (Daisy main loop token handler / ESP32 console
// task), read by every log call site. Plain bytes, deliberately not atomics:
// a byte load cannot tear on either target, and the worst a stale read can
// do is emit or drop one more line during the toggle itself. On the Daisy,
// both control and emission are main-loop context anyway (log_ring.h).
inline uint8_t g_module_levels[kModuleCount] = {
#define WAVEX_LOG_X(name, deflvl) deflvl,
    WAVEX_LOG_MODULE_LIST(WAVEX_LOG_X)
#undef WAVEX_LOG_X
};

inline const char* const kModuleNames[kModuleCount] = {
#define WAVEX_LOG_X(name, deflvl) #name,
    WAVEX_LOG_MODULE_LIST(WAVEX_LOG_X)
#undef WAVEX_LOG_X
};

inline uint8_t GetLevel(Module m) {
    return g_module_levels[static_cast<uint8_t>(m)];
}

inline void SetLevel(Module m, uint8_t level) {
    if (level > WAVEX_LOG_LEVEL_TRACE) {
        level = WAVEX_LOG_LEVEL_TRACE;
    }
    g_module_levels[static_cast<uint8_t>(m)] = level;
}

inline void SetAllLevels(uint8_t level) {
    for (size_t i = 0; i < kModuleCount; ++i) {
        SetLevel(static_cast<Module>(i), level);
    }
}

namespace Detail {
// Case-insensitive ASCII equality; console input shouldn't fail on case.
inline bool CiEq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? static_cast<char>(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? static_cast<char>(*b - 32) : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}
}  // namespace Detail

// Case-insensitive module lookup ("inter_mcu_link"). Returns false when the
// name matches no module; "*" is handled by the command parser, not here.
inline bool SetLevelByName(const char* name, uint8_t level) {
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < kModuleCount; ++i) {
        if (Detail::CiEq(kModuleNames[i], name)) {
            SetLevel(static_cast<Module>(i), level);
            return true;
        }
    }
    return false;
}

inline const char* const kLevelNames[6] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

// Level token: a name from kLevelNames (case-insensitive) or a digit 0-5.
// Returns -1 when the token is neither.
inline int ParseLevelToken(const char* s) {
    if (!s || !*s) {
        return -1;
    }
    if (s[0] >= '0' && s[0] <= '5' && s[1] == '\0') {
        return s[0] - '0';
    }
    for (int i = 0; i < 6; ++i) {
        if (Detail::CiEq(kLevelNames[i], s)) {
            return i;
        }
    }
    return -1;
}

// Applies a "WAVEX-LOG" console command's argument string:
//
//   "<MODULE|*> <OFF|ERROR|WARN|INFO|DEBUG|TRACE|0-5>"
//
// Shared verbatim by both MCUs' console channels so the command grammar
// cannot drift between boards, and host-testable for the same reason. The
// reply is a single human-readable line (confirmation or usage); `reply`
// may be null when no channel exists to print it. Returns true only when a
// level was actually applied.
inline bool ApplyLevelCommand(const char* args, char* reply, size_t reply_cap) {
    char mod_tok[32];
    char lvl_tok[8];
    size_t n = 0;

    const char* p = args ? args : "";
    while (*p == ' ') {
        ++p;
    }
    while (*p && *p != ' ' && n + 1 < sizeof(mod_tok)) {
        mod_tok[n++] = *p++;
    }
    mod_tok[n] = '\0';
    while (*p == ' ') {
        ++p;
    }
    n = 0;
    while (*p && *p != ' ' && n + 1 < sizeof(lvl_tok)) {
        lvl_tok[n++] = *p++;
    }
    lvl_tok[n] = '\0';

    const int level = ParseLevelToken(lvl_tok);
    const bool all = mod_tok[0] == '*' && mod_tok[1] == '\0';
    bool ok = false;
    if (mod_tok[0] != '\0' && level >= 0) {
        if (all) {
            SetAllLevels(static_cast<uint8_t>(level));
            ok = true;
        } else {
            ok = SetLevelByName(mod_tok, static_cast<uint8_t>(level));
        }
    }

    if (reply && reply_cap > 0) {
        if (ok) {
            snprintf(reply, reply_cap, "WAVEX-LOG: %s=%s", all ? "*" : mod_tok, kLevelNames[level]);
        } else {
            // The echoed command is bounded so the whole reply has a known
            // maximum (117 bytes); callers size `reply` for that, and the
            // compiler can see it fits rather than warning about truncation.
            snprintf(reply,
                     reply_cap,
                     "WAVEX-LOG: bad command '%.24s' - usage: WAVEX-LOG "
                     "<MODULE|*> <OFF|ERROR|WARN|INFO|DEBUG|TRACE|0-5>",
                     args ? args : "");
        }
    }
    return ok;
}

}  // namespace Log
}  // namespace WaveX

// ============================================================================
// EMISSION (platform-specific back end)
// ============================================================================

#ifdef ESP_PLATFORM

#include "esp_log.h"

// Levels are numerically esp_log_level_t, so IDF's formatter and its own
// per-tag runtime control both apply on top of the module gate.
#define WAVEX_LOG_EMIT(lvl, letter, mod_str, format, ...) \
    ESP_LOG_LEVEL((esp_log_level_t)(lvl), "WAVEX-" mod_str, format, ##__VA_ARGS__)

#elif defined(DAISY_PLATFORM)

// Daisy: route through the non-blocking log ring (comm/log_ring.h) via the
// helpers in firmware/daisy/src/comm/daisy_logging.cpp. libDaisy's own
// Logger blocks unboundedly on the USB host and starves the audio ring -
// see log_ring.h for the full story. Main-loop context only.
void wavex_daisy_log(const char* format, ...);
void wavex_daisy_log_raw(const char* format, ...);

#define WAVEX_LOG_EMIT(lvl, letter, mod_str, format, ...) \
    wavex_daisy_log("[" letter "][" mod_str "] " format, ##__VA_ARGS__)

#else

// Host builds (unit tests, tools).
#define WAVEX_LOG_EMIT(lvl, letter, mod_str, format, ...) \
    printf("[" letter "][" mod_str "] " format "\n", ##__VA_ARGS__)

#endif

// ============================================================================
// LOGGING MACROS
//
// WAVEX_LOGE/W/I/D/T(MODULE, fmt, ...). MODULE is a bare name from the
// module table (token-pasted, so a typo is a compile error, not a silent
// drop). The ceiling comparison is a constant fold; the runtime comparison
// is one byte load when the ceiling admits the call.
// ============================================================================

#define WAVEX_LOG_AT(MOD, lvl, letter, format, ...)                                               \
    do {                                                                                          \
        if ((lvl) <= (WAVEX_LOG_CEILING_##MOD) &&                                                 \
            (lvl) <=                                                                              \
                ::WaveX::Log::g_module_levels[static_cast<uint8_t>(::WaveX::Log::Module::MOD)]) { \
            WAVEX_LOG_EMIT(lvl, letter, #MOD, format, ##__VA_ARGS__);                             \
        }                                                                                         \
    } while (0)

#define WAVEX_LOGE(MOD, format, ...) \
    WAVEX_LOG_AT(MOD, WAVEX_LOG_LEVEL_ERROR, "E", format, ##__VA_ARGS__)
#define WAVEX_LOGW(MOD, format, ...) \
    WAVEX_LOG_AT(MOD, WAVEX_LOG_LEVEL_WARN, "W", format, ##__VA_ARGS__)
#define WAVEX_LOGI(MOD, format, ...) \
    WAVEX_LOG_AT(MOD, WAVEX_LOG_LEVEL_INFO, "I", format, ##__VA_ARGS__)
#define WAVEX_LOGD(MOD, format, ...) \
    WAVEX_LOG_AT(MOD, WAVEX_LOG_LEVEL_DEBUG, "D", format, ##__VA_ARGS__)
#define WAVEX_LOGT(MOD, format, ...) \
    WAVEX_LOG_AT(MOD, WAVEX_LOG_LEVEL_TRACE, "T", format, ##__VA_ARGS__)

// ============================================================================
// LEGACY ALIASES - existing call sites, migrated incrementally.
//
// WAVEX_LOG_DAISY predates levels; its calls are INFO until each site is
// given a real level. The RAW variant bypasses the module prefix but not
// the gates.
// ============================================================================

#define WAVEX_LOG_DAISY(MOD, format, ...) WAVEX_LOGI(MOD, format, ##__VA_ARGS__)

#if !defined(DAISY_PLATFORM)
#define WAVEX_LOG_DAISY_RAW(MOD, format, ...)                                                     \
    do {                                                                                          \
        if (WAVEX_LOG_LEVEL_INFO <= (WAVEX_LOG_CEILING_##MOD) &&                                  \
            WAVEX_LOG_LEVEL_INFO <=                                                               \
                ::WaveX::Log::g_module_levels[static_cast<uint8_t>(::WaveX::Log::Module::MOD)]) { \
            printf(format, ##__VA_ARGS__);                                                        \
        }                                                                                         \
    } while (0)
#else
#define WAVEX_LOG_DAISY_RAW(MOD, format, ...)                                                     \
    do {                                                                                          \
        if (WAVEX_LOG_LEVEL_INFO <= (WAVEX_LOG_CEILING_##MOD) &&                                  \
            WAVEX_LOG_LEVEL_INFO <=                                                               \
                ::WaveX::Log::g_module_levels[static_cast<uint8_t>(::WaveX::Log::Module::MOD)]) { \
            wavex_daisy_log_raw(format, ##__VA_ARGS__);                                           \
        }                                                                                         \
    } while (0)
#endif

// ============================================================================
// FEATURE FLAGS that live here because they follow the debug/release notion
// of the firmware, not because they gate log lines.
// ============================================================================

// Master debug/release switch, set by the build profile
// (README.md#build-profiles). Everything below defaults from it, so one
// -DWAVEX_BUILD_DEBUG=0 turns the whole debug surface off, while each flag
// stays individually overridable for a bring-up image that wants one back.
//
// This cannot be derived from the compiler's own notion of a debug build: the
// ESP32 is built OPTIMIZATION_PERF with assertions disabled in day-to-day use,
// so NDEBUG and -O level say nothing about whether debug *features* are wanted.
#ifndef WAVEX_BUILD_DEBUG
#define WAVEX_BUILD_DEBUG 1
#endif

#ifndef WAVEX_DEBUG_LOGGING_ENABLED
#define WAVEX_DEBUG_LOGGING_ENABLED WAVEX_BUILD_DEBUG
#endif

/**
 * @def WAVEX_DEBUG_HARNESS_ENABLED
 * @brief Host-driven console command channel (both MCUs, debug builds only).
 *
 * Gates the console *command* surface on both boards: the "WAVEX-LOG"
 * runtime level control and the acknowledged "WAVEX-DBG" input-injection /
 * state-query harness of docs/features/debug-harness-and-hil.md (grammar in
 * shared/debug/console_command.h). Costs a console task and a synthetic
 * touch indev (ESP32) or an ISR byte-matcher (Daisy) plus a line buffer.
 *
 * Deliberately does NOT gate "WAVEX-ENTER-DFU". That token is the only reflash
 * path needing no BOOT+RESET, and a release image is the one most likely to
 * need recovering; gating it behind the debug profile is exactly backwards.
 *
 * Note this gates *changing* levels, not logging itself. The runtime level
 * table survives either way - a release build's saving comes from lowering
 * WAVEX_LOG_CEILING_*, which deletes call sites outright.
 */
#ifndef WAVEX_DEBUG_HARNESS_ENABLED
#define WAVEX_DEBUG_HARNESS_ENABLED WAVEX_BUILD_DEBUG
#endif

/**
 * @def WAVEX_ESP_SCREENSHOT_DEBUG
 * @brief Serial screenshot capture (ESP32, debug builds only).
 *
 * The debug console (WAVEX_DEBUG_HARNESS_ENABLED, which carries the listener)
 * accepts "WAVEX-SCREENSHOT" and dumps the active LVGL screen as RLE+base64
 * RGB565 between BEGIN/END markers, decoded by scripts/esp32_screenshot.py.
 * Costs, per capture, a transient ~4.5 MB of PSRAM.
 */
#ifndef WAVEX_ESP_SCREENSHOT_DEBUG
#define WAVEX_ESP_SCREENSHOT_DEBUG WAVEX_DEBUG_HARNESS_ENABLED
#endif

/**
 * @def WAVEX_DAISY_UART_PERF_DEBUG
 * @brief Per-interval UART link cost, throughput and error deltas.
 *
 * Answers the question the existing counters cannot: how much main-loop time
 * the inter-MCU link costs, and therefore whether it is competing with the
 * audio ring refill. This is a compile flag, not a module level, because the
 * MEASUREMENT itself rides the hot path - the cost is incurred whether or
 * not the report line prints. The report goes out at UART_PERF level INFO.
 */
#ifndef WAVEX_DAISY_UART_PERF_DEBUG
#define WAVEX_DAISY_UART_PERF_DEBUG 1
#endif
