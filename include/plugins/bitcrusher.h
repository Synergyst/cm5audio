#ifndef BITCRUSHER_PROCESSOR_H
#define BITCRUSHER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <string>

class BitcrusherProcessor : public AudioProcessor {
public:
    BitcrusherProcessor(float bits, float sample_rate);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "bits" || id == "sample_rate";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "bits") { m_bits.store(value, std::memory_order_relaxed); return true; }
        if (id == "sample_rate") { m_sample_rate.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "bitcrusher"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "bits") { value = m_bits.load(std::memory_order_relaxed); return true; }
        if (id == "sample_rate") { value = m_sample_rate.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_bits;
    std::atomic<float> m_sample_rate;
    float m_last_samples[8] = {0.0f};
    unsigned int m_sample_counters[8] = {0};
};

#endif
