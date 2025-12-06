#include "profiling/profiler.h"

#include <cstring>

namespace WaveX {
namespace Profiling {

void InitHardware() {
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    }

    DWT->CYCCNT = 0;

#if defined(DWT_CTRL_CYCCNTENA_Msk)
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif

#if WAVEX_PROFILING_ENABLED
    DWT->CTRL |= DWT_CTRL_EXCEVTENA_Msk | DWT_CTRL_LSUEVTENA_Msk | DWT_CTRL_SLEEPEVTENA_Msk;

#if WAVEX_PROFILING_ITM_ENABLED
    ITM->LAR = 0xC5ACCE55;
    ITM->TER = 0xFFFFFFFF;
    ITM->TCR = ITM_TCR_ITMENA_Msk;
#endif
#endif
}

uint32_t GetCycles() {
#if defined(DWT)
    return DWT->CYCCNT;
#else
    return 0;
#endif
}

float CyclesToMicroseconds(uint32_t cycles) {
    return cycles / ((float)SystemCoreClock / 1000000.0f);
}

void ProfileZone::Reset() {
    total_cycles = 0;
    min_cycles = UINT32_MAX;
    max_cycles = 0;
    entry_count = 0;
    last_cycles = 0;
}

uint32_t ProfileZone::GetAvgCycles() const {
    return entry_count ? static_cast<uint32_t>(total_cycles / entry_count) : 0;
}

void ProfileZone::GetStats(float& avg_us, float& min_us, float& max_us) const {
    avg_us = CyclesToMicroseconds(GetAvgCycles());
    min_us = min_cycles == UINT32_MAX ? 0.0f : CyclesToMicroseconds(min_cycles);
    max_us = CyclesToMicroseconds(max_cycles);
}

#if WAVEX_PROFILING_ENABLED
ProfileZone Profiler::zones_[PROFILE_MAX_ZONES];
uint32_t Profiler::zone_count_ = 0;
#endif

uint32_t Profiler::RegisterZone(const char* name) {
#if WAVEX_PROFILING_ENABLED
    if (zone_count_ >= PROFILE_MAX_ZONES) {
        return UINT32_MAX;
    }
    ProfileZone& zone = zones_[zone_count_];
    zone.name = name;
    zone.Reset();
    return zone_count_++;
#else
    (void)name;
    return 0;
#endif
}

uint32_t Profiler::Begin() {
#if WAVEX_PROFILING_ENABLED
    return GetCycles();
#else
    return 0;
#endif
}

void Profiler::End(uint32_t zone_id, uint32_t start_cycles) {
#if WAVEX_PROFILING_ENABLED
    if (zone_id >= zone_count_) {
        return;
    }

    uint32_t end_cycles = GetCycles();
    uint32_t elapsed = end_cycles - start_cycles;

    ProfileZone& zone = zones_[zone_id];
    zone.last_cycles = elapsed;
    zone.total_cycles += elapsed;
    zone.entry_count++;

    if (elapsed < zone.min_cycles) {
        zone.min_cycles = elapsed;
    }
    if (elapsed > zone.max_cycles) {
        zone.max_cycles = elapsed;
    }

#if WAVEX_PROFILING_ITM_ENABLED
    if ((ITM->TCR & ITM_TCR_ITMENA_Msk) && (ITM->TER & (1U << 0))) {
        while (ITM->PORT[0].u32 == 0) {
        }
        ITM->PORT[0].u32 = (zone_id << 24) | (elapsed & 0xFFFFFF);
    }
#endif
#else
    (void)zone_id;
    (void)start_cycles;
#endif
}

const ProfileZone* Profiler::GetZone(uint32_t zone_id) {
#if WAVEX_PROFILING_ENABLED
    if (zone_id >= zone_count_) {
        return nullptr;
    }
    return &zones_[zone_id];
#else
    (void)zone_id;
    return nullptr;
#endif
}

void Profiler::ResetAll() {
#if WAVEX_PROFILING_ENABLED
    for (uint32_t i = 0; i < zone_count_; ++i) {
        zones_[i].Reset();
    }
#endif
}

uint32_t Profiler::GetZoneCount() {
#if WAVEX_PROFILING_ENABLED
    return zone_count_;
#else
    return 0;
#endif
}

ProfileScope::ProfileScope(uint32_t zone_id)
#if WAVEX_PROFILING_ENABLED
    : zone_id_(zone_id), start_cycles_(Profiler::Begin())
#endif
{
}

ProfileScope::~ProfileScope() {
#if WAVEX_PROFILING_ENABLED
    Profiler::End(zone_id_, start_cycles_);
#endif
}

}  // namespace Profiling
}  // namespace WaveX



