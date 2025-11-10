#ifndef WAVEX_TEST_HELPERS_H
#define WAVEX_TEST_HELPERS_H

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace WaveX {
namespace Test {

// Helper to create test audio buffers
class AudioBufferGenerator {
   public:
    static std::vector<float> GenerateSineWave(size_t length,
                                               float frequency,
                                               float sampleRate,
                                               float amplitude = 1.0f);
    static std::vector<float> GenerateSilence(size_t length);
    static std::vector<float> GenerateNoise(size_t length, float amplitude = 1.0f);
    static std::vector<float> GenerateSquareWave(size_t length,
                                                 float frequency,
                                                 float sampleRate,
                                                 float amplitude = 1.0f);
};

// Helper to create test packets
class PacketGenerator {
   public:
    static std::vector<uint8_t> CreateTestPacket(uint8_t msg_type,
                                                 const void* payload,
                                                 size_t payload_size);
    static std::vector<uint8_t> CreateControlChangePacket(uint8_t parameter,
                                                          uint8_t channel,
                                                          uint16_t value);
    static std::vector<uint8_t> CreateNoteOnPacket(uint8_t note, uint8_t velocity, uint8_t channel);
    static std::vector<uint8_t> CreateHeartbeatPacket(uint32_t uptime_ms,
                                                      uint32_t rx_total,
                                                      uint32_t loop_counter);
};

// CRC test vectors
class CRCTestVectors {
   public:
    struct TestVector {
        const uint8_t* data;
        size_t length;
        uint16_t expected_crc;
    };

    static const TestVector* GetTestVectors();
    static size_t GetTestVectorCount();
};

// Filesystem test fixtures
class FilesystemFixture {
   public:
    static void CreateTestDirectoryStructure(const char* base_path);
    static void CleanupTestDirectoryStructure(const char* base_path);
    static std::vector<uint8_t> CreateTestWavFile(size_t sample_count,
                                                  uint16_t sample_rate = 48000,
                                                  uint8_t channels = 2,
                                                  uint8_t bit_depth = 16);
};

}  // namespace Test
}  // namespace WaveX

#endif  // WAVEX_TEST_HELPERS_H
