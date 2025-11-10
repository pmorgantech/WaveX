#ifndef WAVEX_BUFFER_H
#define WAVEX_BUFFER_H

#include <stddef.h>
#include <stdint.h>

namespace WaveX {
namespace Utils {

class CircularBuffer {
   public:
    CircularBuffer(uint8_t* buffer, size_t size);

    bool Push(const uint8_t* data, size_t length);
    bool Pop(uint8_t* data, size_t length);

    size_t GetAvailableData() const;
    size_t GetAvailableSpace() const;

    void Clear();

   private:
    uint8_t* buffer_;
    size_t size_;
    size_t head_;
    size_t tail_;
    size_t count_;
};

}  // namespace Utils
}  // namespace WaveX

#endif  // WAVEX_BUFFER_H
