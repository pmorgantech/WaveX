#include "test_helpers.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

namespace WaveX {
namespace Test {

std::vector<float> AudioBufferGenerator::GenerateSineWave(size_t length,
                                                          float frequency,
                                                          float sampleRate,
                                                          float amplitude) {
    std::vector<float> buffer(length);
    const float phase_increment = 2.0f * M_PI * frequency / sampleRate;

    for (size_t i = 0; i < length; ++i) {
        buffer[i] = amplitude * std::sin(phase_increment * i);
    }

    return buffer;
}

std::vector<float> AudioBufferGenerator::GenerateSilence(size_t length) {
    return std::vector<float>(length, 0.0f);
}

std::vector<float> AudioBufferGenerator::GenerateNoise(size_t length, float amplitude) {
    std::vector<float> buffer(length);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(-amplitude, amplitude);

    for (size_t i = 0; i < length; ++i) {
        buffer[i] = dis(gen);
    }

    return buffer;
}

std::vector<float> AudioBufferGenerator::GenerateSquareWave(size_t length,
                                                            float frequency,
                                                            float sampleRate,
                                                            float amplitude) {
    std::vector<float> buffer(length);
    const float period = sampleRate / frequency;

    for (size_t i = 0; i < length; ++i) {
        float phase = std::fmod(i, period) / period;
        buffer[i] = (phase < 0.5f) ? amplitude : -amplitude;
    }

    return buffer;
}

std::vector<uint8_t> PacketGenerator::CreateTestPacket(uint8_t msg_type,
                                                       const void* payload,
                                                       size_t payload_size) {
    // Simplified packet creation for testing
    std::vector<uint8_t> packet;
    packet.reserve(8 + payload_size);  // Header + payload

    // Simple header: [msg_type][payload_size][payload...]
    packet.push_back(msg_type);
    packet.push_back(static_cast<uint8_t>(payload_size & 0xFF));
    packet.push_back(static_cast<uint8_t>((payload_size >> 8) & 0xFF));

    if (payload && payload_size > 0) {
        const uint8_t* payload_bytes = static_cast<const uint8_t*>(payload);
        packet.insert(packet.end(), payload_bytes, payload_bytes + payload_size);
    }

    return packet;
}

std::vector<uint8_t> PacketGenerator::CreateControlChangePacket(uint8_t parameter,
                                                                uint8_t channel,
                                                                uint16_t value) {
    struct {
        uint8_t parameter;
        uint8_t channel;
        uint16_t value;
    } payload = {parameter, channel, value};

    return CreateTestPacket(0x01, &payload, sizeof(payload));
}

std::vector<uint8_t> PacketGenerator::CreateNoteOnPacket(uint8_t note,
                                                         uint8_t velocity,
                                                         uint8_t channel) {
    struct {
        uint8_t note;
        uint8_t velocity;
        uint8_t channel;
        uint8_t reserved;
    } payload = {note, velocity, channel, 0};

    return CreateTestPacket(0x02, &payload, sizeof(payload));
}

std::vector<uint8_t> PacketGenerator::CreateHeartbeatPacket(uint32_t uptime_ms,
                                                            uint32_t rx_total,
                                                            uint32_t loop_counter) {
    struct {
        uint32_t uptime_ms;
        uint32_t rx_total;
        uint32_t loop_counter;
        uint16_t cpu_avg_percent;
        uint16_t cpu_min_percent;
        uint16_t cpu_max_percent;
    } payload = {uptime_ms, rx_total, loop_counter, 0, 0, 0};

    return CreateTestPacket(0x12, &payload, sizeof(payload));
}

// CRC test vectors
static const CRCTestVectors::TestVector crc_test_vectors[] = {
    {(const uint8_t*)"", 0, 0xFFFF},
    {(const uint8_t*)"A", 1, 0x538D},
    {(const uint8_t*)"123456789", 9, 0x29B1},
    {(const uint8_t*)"Hello, World!", 13, 0xE5CC},
    {(const uint8_t*)"\x00\x01\x02\x03", 4, 0x89C3},
};

const CRCTestVectors::TestVector* CRCTestVectors::GetTestVectors() {
    return crc_test_vectors;
}

size_t CRCTestVectors::GetTestVectorCount() {
    return sizeof(crc_test_vectors) / sizeof(crc_test_vectors[0]);
}

void FilesystemFixture::CreateTestDirectoryStructure(const char* base_path) {
    namespace fs = std::filesystem;
    try {
        fs::create_directories(base_path);
        fs::create_directories(fs::path(base_path) / "samples");
        fs::create_directories(fs::path(base_path) / "presets");
    } catch (...) {
        // Ignore errors in test environment
    }
}

void FilesystemFixture::CleanupTestDirectoryStructure(const char* base_path) {
    namespace fs = std::filesystem;
    try {
        fs::remove_all(base_path);
    } catch (...) {
        // Ignore errors in test environment
    }
}

std::vector<uint8_t> FilesystemFixture::CreateTestWavFile(size_t sample_count,
                                                          uint16_t sample_rate,
                                                          uint8_t channels,
                                                          uint8_t bit_depth) {
    std::vector<uint8_t> wav_data;

    // WAV header (simplified)
    const size_t data_size = sample_count * channels * (bit_depth / 8);
    const size_t file_size = 36 + data_size;

    // RIFF header
    wav_data.insert(wav_data.end(), {'R', 'I', 'F', 'F'});
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&file_size),
                    reinterpret_cast<const uint8_t*>(&file_size) + 4);
    wav_data.insert(wav_data.end(), {'W', 'A', 'V', 'E'});

    // fmt chunk
    wav_data.insert(wav_data.end(), {'f', 'm', 't', ' '});
    uint32_t fmt_size = 16;
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&fmt_size),
                    reinterpret_cast<const uint8_t*>(&fmt_size) + 4);
    uint16_t audio_format = 1;  // PCM
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&audio_format),
                    reinterpret_cast<const uint8_t*>(&audio_format) + 2);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&channels),
                    reinterpret_cast<const uint8_t*>(&channels) + 1);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&channels) + 1,
                    reinterpret_cast<const uint8_t*>(&channels) + 2);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&sample_rate),
                    reinterpret_cast<const uint8_t*>(&sample_rate) + 2);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&sample_rate) + 2,
                    reinterpret_cast<const uint8_t*>(&sample_rate) + 4);
    uint16_t block_align = channels * (bit_depth / 8);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&block_align),
                    reinterpret_cast<const uint8_t*>(&block_align) + 2);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&bit_depth),
                    reinterpret_cast<const uint8_t*>(&bit_depth) + 1);
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&bit_depth) + 1,
                    reinterpret_cast<const uint8_t*>(&bit_depth) + 2);

    // data chunk
    wav_data.insert(wav_data.end(), {'d', 'a', 't', 'a'});
    wav_data.insert(wav_data.end(),
                    reinterpret_cast<const uint8_t*>(&data_size),
                    reinterpret_cast<const uint8_t*>(&data_size) + 4);

    // Sample data (zeros for now)
    wav_data.resize(wav_data.size() + data_size, 0);

    return wav_data;
}

}  // namespace Test
}  // namespace WaveX
