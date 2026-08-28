#pragma once

#include <cstdint>

namespace WaveX {
namespace SdramLayout {

// Daisy Seed external SDRAM ownership. Keep this partition centralized so
// sample storage and offline rendering can never silently overlap.
constexpr uintptr_t kBase = 0xC0000000u;
constexpr uint32_t kTotalBytes = 64u * 1024u * 1024u;
constexpr uint32_t kRenderScratchBytes = 4u * 1024u * 1024u;
constexpr uint32_t kSampleArenaBytes = kTotalBytes - kRenderScratchBytes;
constexpr uint32_t kSmallSamplePoolBytes = 256u * 1024u;
constexpr uintptr_t kRenderScratchBase = kBase + kSampleArenaBytes;

static_assert(kSampleArenaBytes + kRenderScratchBytes == kTotalBytes,
              "SDRAM partitions must cover the complete device");
static_assert((kSampleArenaBytes % (64u * 1024u)) == 0,
              "sample arena must align to the extent allocator page size");
static_assert((kSmallSamplePoolBytes % 4096u) == 0,
              "small sample pool must align to the slab page size");

}  // namespace SdramLayout
}  // namespace WaveX
