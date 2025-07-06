#include "buffer.h"
#include <string.h>

namespace WaveX {
namespace Utils {

CircularBuffer::CircularBuffer(uint8_t* buffer, size_t size) 
    : buffer_(buffer), size_(size), head_(0), tail_(0), count_(0) 
{
}

bool CircularBuffer::Push(const uint8_t* data, size_t length)
{
    if (length > GetAvailableSpace()) {
        return false;
    }
    
    for (size_t i = 0; i < length; i++) {
        buffer_[head_] = data[i];
        head_ = (head_ + 1) % size_;
        count_++;
    }
    
    return true;
}

bool CircularBuffer::Pop(uint8_t* data, size_t length)
{
    if (length > count_) {
        return false;
    }
    
    for (size_t i = 0; i < length; i++) {
        data[i] = buffer_[tail_];
        tail_ = (tail_ + 1) % size_;
        count_--;
    }
    
    return true;
}

size_t CircularBuffer::GetAvailableData() const
{
    return count_;
}

size_t CircularBuffer::GetAvailableSpace() const
{
    return size_ - count_;
}

void CircularBuffer::Clear()
{
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

} // namespace Utils
} // namespace WaveX 