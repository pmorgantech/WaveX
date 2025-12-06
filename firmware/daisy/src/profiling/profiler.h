#pragma once

#include "stm32h7xx.h"
#include <cstddef>
#include <cstdint>

// ============================================================================ //
// Profiling configuration
// ============================================================================ //

#ifndef WAVEX_PROFILING_ENABLED
#define WAVEX_PROFILING_ENABLED 0
#endif

#ifndef WAVEX_PROFILING_ITM_ENABLED
#define WAVEX_PROFILING_ITM_ENABLED 0
#endif

#define PROFILE_MAX_ZONES 32

namespace WaveX {
namespace Profiling {

void InitHardware();
uint32_t GetCycles();
float CyclesToMicroseconds(uint32_t cycles);

struct ProfileZone {
    const char* name = nullptr;
    uint64_t total_cycles = 0;
    uint32_t min_cycles = UINT32_MAX;
    uint32_t max_cycles = 0;
    uint32_t entry_count = 0;
    uint32_t last_cycles = 0;

    void Reset();
    uint32_t GetAvgCycles() const;
    void GetStats(float& avg_us, float& min_us, float& max_us) const;
};

class Profiler {
public:
    static uint32_t RegisterZone(const char* name);
    static uint32_t Begin();
    static void End(uint32_t zone_id, uint32_t start_cycles);
    static const ProfileZone* GetZone(uint32_t zone_id);
    static void ResetAll();
    static uint32_t GetZoneCount();

private:
#if WAVEX_PROFILING_ENABLED
    static ProfileZone zones_[PROFILE_MAX_ZONES];
    static uint32_t zone_count_;
#endif
};

class ProfileScope {
public:
    explicit ProfileScope(uint32_t zone_id);
    ~ProfileScope();
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
#if WAVEX_PROFILING_ENABLED
    uint32_t zone_id_;
    uint32_t start_cycles_;
#endif
};

}  // namespace Profiling
}  // namespace WaveX

// ============================================================================ //
// Profiling macros
// ============================================================================ //

#if WAVEX_PROFILING_ENABLED

#define PROFILE_CONCAT_IMPL(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_IMPL(a, b)

#define PROFILE_DEFINE_ZONE(name) \
    static uint32_t g_profile_zone_##name = UINT32_MAX

#define PROFILE_REGISTER_ZONE(name) \
    g_profile_zone_##name = WaveX::Profiling::Profiler::RegisterZone(#name)

#define PROFILE_SCOPE(name) \
    WaveX::Profiling::ProfileScope PROFILE_CONCAT(profile_scope_, name)(g_profile_zone_##name)

#define PROFILE_BEGIN(name) \
    uint32_t PROFILE_CONCAT(profile_start_, name) = WaveX::Profiling::Profiler::Begin()

#define PROFILE_END(name) \
    WaveX::Profiling::Profiler::End(g_profile_zone_##name, PROFILE_CONCAT(profile_start_, name))

#else

#define PROFILE_DEFINE_ZONE(name)
#define PROFILE_REGISTER_ZONE(name)
#define PROFILE_SCOPE(name)
#define PROFILE_BEGIN(name)
#define PROFILE_END(name)

#endif


