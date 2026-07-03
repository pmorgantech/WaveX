#pragma once

// Stage A CV backend: a single MCP4728 I2C quad-DAC (architecture.md §5.3).
// One physical chip serves WAVEX_ANALOG_CV_GROUPS=1 group directly (3 of its
// 4 channels: cutoff, resonance, VCA; the 4th is spare) - Stage A is fixed
// at exactly one group by construction, since chaining multiple MCP4728s
// for more groups is Stage B's job (MCP48CMB28 SPI chain, see
// mcp48_backend.hpp), not this backend's.
//
// This supersedes the former CvBus class (src/cv_bus.hpp, now removed):
// same calibration math and MCP4728 fast-write protocol, but QueueGroup()
// actually stages the requested group's channels instead of ignoring the
// group argument, and Flush() sends whatever was last staged instead of
// being permanently hardcoded to slot 0.

#include "config/hardware_config.h"
#include "daisy_seed.h"

#include "cv_cal.hpp"
#include <array>
#include <cstdint>

namespace WaveX {
namespace Cv {

class Mcp4728Backend {
   public:
    void Init(uint8_t i2c_addr = 0x60) {
        addr_ = i2c_addr;
        daisy::I2CHandle::Config cfg;
        cfg.periph = daisy::I2CHandle::Config::Peripheral::I2C_1;
        cfg.mode = daisy::I2CHandle::Config::Mode::I2C_MASTER;
        cfg.speed = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
        i2c_.Init(cfg);
    }

    void SetGroupCal(uint8_t group, const CvCal& c) {
        if (group < cal_.size())
            cal_[group] = c;
    }

    // Callback-safe: only stages values into slot_.
    void QueueGroup(uint8_t group, float cutoff, float resonance, float vca) {
        if (group >= cal_.size())
            return;
        const CvCal& c = cal_[group];
        float cut = CvClamp01(c.vcf_cut_gain * CvShapeCutoff(CvClamp01(cutoff), c.cutoff_k) +
                              c.vcf_cut_off);
        float q = CvClamp01(c.vcf_q_gain * CvClamp01(resonance) + c.vcf_q_off);
        float vca_inv = 1.0f - CvClamp01(vca);
        float va = CvClamp01(c.vca_gain * vca_inv + c.vca_off);
        cutoff_dac_ = (uint16_t)lrintf(cut * 4095.0f);
        res_dac_ = (uint16_t)lrintf(q * 4095.0f);
        vca_dac_ = (uint16_t)lrintf(va * 4095.0f);
    }

    // Main-loop only: performs the blocking I2C fast-write transaction. Per
    // docs/features/analog-voice-board.md §0, this is ~225us @400kHz and
    // must never run in the audio callback.
    void Flush() { WriteFastWrite(cutoff_dac_, res_dac_, vca_dac_, 0); }

   private:
    void WriteFastWrite(uint16_t ch0, uint16_t ch1, uint16_t ch2, uint16_t ch3) {
        uint8_t buf[9];
        size_t k = 0;
        buf[k++] = 0x00;  // Fast Write
        auto emit = [&](uint16_t v) {
            buf[k++] = (uint8_t)((v >> 8) & 0x0F);
            buf[k++] = (uint8_t)(v & 0xFF);
        };
        emit(ch0);
        emit(ch1);
        emit(ch2);
        emit(ch3);
        i2c_.TransmitBlocking((uint16_t)(addr_ << 1), buf, k, 1);
    }

    uint8_t addr_ = 0x60;
    daisy::I2CHandle i2c_;
    uint16_t cutoff_dac_ = 0;
    uint16_t res_dac_ = 0;
    uint16_t vca_dac_ = 0;
    // Calibration is per analog-voice-group and sized for the full 8-group
    // board even though Stage A only ever queries index 0, so the same
    // stored calibration table (SD-persisted) survives the Stage A -> B
    // transition without reformatting.
    std::array<CvCal, WAVEX_ANALOG_CV_GROUPS_MAX> cal_{};
};

}  // namespace Cv
}  // namespace WaveX
