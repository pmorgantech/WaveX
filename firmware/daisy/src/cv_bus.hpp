#pragma once

#include <cstdint>
#include <stdint.h>
#include <array>
#include <math.h>
#include <cmath>
#include "daisy_seed.h"

struct CvCal {
    float vcf_cut_gain = 1.0f, vcf_cut_off = 0.0f;
    float vcf_q_gain = 1.0f, vcf_q_off = 0.0f;
    float vca_gain = 1.0f, vca_off = 0.0f;
    float cutoff_k = 3.0f;
};

class CvBus {
  public:
    void Init(uint8_t i2c_addr = 0x60) {
        addr_ = i2c_addr;
        daisy::I2CHandle::Config cfg;
        cfg.periph = daisy::I2CHandle::Config::Peripheral::I2C_1;
        cfg.mode = daisy::I2CHandle::Config::Mode::I2C_MASTER;
        cfg.speed = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
        i2c_.Init(cfg);
    }

    void SetVoiceCal(uint8_t v, const CvCal& c) { if(v < cal_.size()) cal_[v] = c; }

    void QueueVoice(uint8_t v, float cutoff, float res, float vca) {
        if(v >= max_voices_) return;
        const auto& C = cal_[v];
        auto clamp01 = [](float x){ return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
        auto shaped = [&](float x){
            float e = expf(C.cutoff_k * x) - 1.0f;
            float d = expf(C.cutoff_k) - 1.0f;
            float y = d > 0.0f ? (e / d) : x;
            return y;
        };
        float cut = clamp01(C.vcf_cut_gain * shaped(clamp01(cutoff)) + C.vcf_cut_off);
        float q = clamp01(C.vcf_q_gain * clamp01(res) + C.vcf_q_off);
        float vca_inv = 1.0f - clamp01(vca);
        float va = clamp01(C.vca_gain * vca_inv + C.vca_off);
        slot_[v * 3 + 0] = (uint16_t)lrintf(cut * 4095.0f);
        slot_[v * 3 + 1] = (uint16_t)lrintf(q * 4095.0f);
        slot_[v * 3 + 2] = (uint16_t)lrintf(va * 4095.0f);
    }

    void Flush() { writeMCP4728_Quad(slot_[0], slot_[1], slot_[2], 0); }

  private:
    void writeMCP4728_Quad(uint16_t ch0, uint16_t ch1, uint16_t ch2, uint16_t ch3) {
        uint8_t buf[9];
        size_t k = 0;
        buf[k++] = 0x00; // Fast Write
        auto emit = [&](uint16_t v){
            buf[k++] = (uint8_t)((v >> 8) & 0x0F);
            buf[k++] = (uint8_t)(v & 0xFF);
        };
        emit(ch0); emit(ch1); emit(ch2); emit(ch3);
        i2c_.TransmitBlocking((uint16_t)(addr_ << 1), buf, k, 1);
    }

    uint8_t addr_ = 0x60;
    daisy::I2CHandle i2c_;
    static constexpr uint8_t max_voices_ = 8;
    std::array<CvCal, max_voices_> cal_{};
    std::array<uint16_t, max_voices_ * 3> slot_{};
};


