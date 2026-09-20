#pragma once

#include <cstddef>
#include <cstdint>

namespace WaveX::Comm {

// Main-loop-owned, single pending listing. A newer browse or storage-loss
// notification supersedes an unsent listing. The transport copies on success.
class BrowseResponseOutbox {
   public:
    static constexpr size_t kCapacity = 2048;

    uint8_t* Begin() {
        size_ = 0;
        return payload_;
    }

    void Commit(size_t size) { size_ = size <= kCapacity ? static_cast<uint16_t>(size) : 0; }

    template <typename Send>
    void Pump(Send&& send) {
        if (size_ && send(static_cast<const uint8_t*>(payload_), size_) >= 0)
            size_ = 0;
    }

   private:
    uint8_t payload_[kCapacity]{};
    uint16_t size_ = 0;
};

}  // namespace WaveX::Comm
