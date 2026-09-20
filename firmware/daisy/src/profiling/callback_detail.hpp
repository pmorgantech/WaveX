#pragma once

#ifndef WAVEX_PROFILE_CALLBACK_DETAIL
#define WAVEX_PROFILE_CALLBACK_DETAIL 0
#endif

#if WAVEX_PROFILE_CALLBACK_DETAIL
#if !WAVEX_PROFILING_ENABLED
#error "Callback detail requires WAVEX_PROFILING_ENABLED"
#endif
#include "profiling/profiler.h"

#include "audio/snapshot_mailbox.hpp"
#include "profiling/callback_peak.hpp"

namespace WaveX::Profiling {
inline CallbackPeakWindow callback_detail_window;
inline AudioEngine::SnapshotMailbox<CallbackPeak> callback_detail_mailbox;

class CallbackDetailScope {
   public:
    explicit CallbackDetailScope(CallbackStage stage) : stage_(stage), start_(GetCycles()) {}
    ~CallbackDetailScope() { callback_detail_window.Add(stage_, start_, GetCycles()); }
    CallbackDetailScope(const CallbackDetailScope&) = delete;
    CallbackDetailScope& operator=(const CallbackDetailScope&) = delete;

   private:
    CallbackStage stage_;
    uint32_t start_;
};
}  // namespace WaveX::Profiling

#define CALLBACK_DETAIL_SCOPE(stage)                               \
    WaveX::Profiling::CallbackDetailScope callback_detail_##stage( \
        WaveX::Profiling::CallbackStage::stage)
#else
#define CALLBACK_DETAIL_SCOPE(stage)
#endif
