#ifndef WAVEX_DAISY_MOCKS_H
#define WAVEX_DAISY_MOCKS_H

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Mock implementation of DaisySeed for unit testing
// This provides a minimal interface that matches what audio_engine.cpp uses

namespace daisy {
class System {
   public:
    static uint32_t GetTick() {
        static uint32_t tick = 0;
        return ++tick;
    }

    static uint32_t GetNow() {
        static uint32_t now = 0;
        return ++now;
    }
};

class DaisySeed {
   public:
    DaisySeed() = default;
    ~DaisySeed() = default;

    void Init(bool boost = false) {
        (void)boost;  // Suppress unused parameter warning
        initialized_ = true;
    }

    void PrintLine(const char* format, ...) {
        // Mock implementation - can capture output if needed
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        printf("\n");
    }

    bool IsInitialized() const { return initialized_; }

    // Capture printed lines for testing
    std::vector<std::string> GetPrintedLines() const { return printed_lines_; }
    void ClearPrintedLines() { printed_lines_.clear(); }

   private:
    bool initialized_ = false;
    std::vector<std::string> printed_lines_;
};

// Mock I2CHandle for CV bus testing
// Uses static storage so all instances share the same transmission log
class I2CHandle {
   public:
    struct Config {
        enum class Peripheral { I2C_1, I2C_2, I2C_3, I2C_4 };
        enum class Mode { I2C_MASTER, I2C_SLAVE };
        enum class Speed { I2C_100KHZ, I2C_400KHZ, I2C_1MHZ };

        Peripheral periph = Peripheral::I2C_1;
        Mode mode = Mode::I2C_MASTER;
        Speed speed = Speed::I2C_400KHZ;
    };

    enum class Result { OK, ERR };

    // Test helpers - static storage shared by all instances
    struct TransmittedData {
        uint16_t address;
        std::vector<uint8_t> data;
    };

    static std::vector<TransmittedData>& GetTransmittedData() {
        static std::vector<TransmittedData> transmitted_data;
        return transmitted_data;
    }

    static void ClearTransmittedData() { GetTransmittedData().clear(); }

    I2CHandle() : initialized_(false) {}

    Result Init(const Config& config) {
        config_ = config;
        initialized_ = true;
        return Result::OK;
    }

    const Config& GetConfig() const { return config_; }

    Result TransmitBlocking(uint16_t address, uint8_t* data, uint16_t size, uint32_t timeout) {
        (void)timeout;  // Suppress unused parameter

        if (!initialized_) {
            return Result::ERR;
        }

        // Store transmitted data in static storage for verification
        TransmittedData tx;
        tx.address = address;
        tx.data.assign(data, data + size);
        GetTransmittedData().push_back(tx);

        return Result::OK;
    }

    bool IsInitialized() const { return initialized_; }

   private:
    bool initialized_ = false;
    Config config_;
};
}  // namespace daisy

// Mock for AudioHandle types (if not already defined)
#ifndef UNIT_TEST_AUDIO_HANDLE_DEFINED
namespace daisy {
namespace AudioHandle {
typedef float* const* InputBuffer;
typedef float* const* OutputBuffer;
}  // namespace AudioHandle
}  // namespace daisy
#define UNIT_TEST_AUDIO_HANDLE_DEFINED
#endif

#endif  // WAVEX_DAISY_MOCKS_H
