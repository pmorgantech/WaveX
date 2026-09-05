#pragma once

#include <cstdint>

namespace WaveX {
namespace SdramLayout {

// Daisy Seed external SDRAM ownership. Keep this partition centralized so
// sample storage and offline rendering can never silently overlap.
constexpr uintptr_t kBase = 0xC0000000u;
constexpr uint32_t kTotalBytes = 64u * 1024u * 1024u;
// The Sample Pool's records (track-and-patch-model.md §4): 1024 entries of
// ~130 B do not fit internal SRAM, so the registry's record table lives
// here, between the arena and the render scratch. Only its 2 KB id index
// stays in SRAM. Main-loop access only.
constexpr uint32_t kSampleRegistryBytes = 192u * 1024u;
constexpr uint32_t kRenderScratchBytes = 4u * 1024u * 1024u - kSampleRegistryBytes;
constexpr uint32_t kSampleArenaBytes = kTotalBytes - kSampleRegistryBytes - kRenderScratchBytes;
constexpr uint32_t kSmallSamplePoolBytes = 256u * 1024u;
constexpr uint32_t kLargeSamplePoolBytes = kSampleArenaBytes - kSmallSamplePoolBytes;
constexpr uintptr_t kSampleRegistryBase = kBase + kSampleArenaBytes;
constexpr uintptr_t kRenderScratchBase = kSampleRegistryBase + kSampleRegistryBytes;

static_assert(kSampleArenaBytes + kSampleRegistryBytes + kRenderScratchBytes == kTotalBytes,
              "SDRAM partitions must cover the complete device");
static_assert((kSampleArenaBytes % (64u * 1024u)) == 0,
              "sample arena must align to the extent allocator page size");
static_assert((kSmallSamplePoolBytes % 4096u) == 0,
              "small sample pool must align to the slab page size");
static_assert((kLargeSamplePoolBytes % (64u * 1024u)) == 0,
              "large sample pool must align to the extent allocator page size");

}  // namespace SdramLayout
}  // namespace WaveX
