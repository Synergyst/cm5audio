#ifndef DEESSER_PROCESSOR_H
#define DEESSER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <string>

class DeEsserProcessor : public AudioProcessor {
public:
    DeEsserProcessor(float threshold, float frequency, float reduction, float speed);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "threshold" || id == "frequency" || id == "reduction" || id == "speed";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "threshold") { m_threshold.store(value, std::memory_order_relaxed); return true; }
        if (id == "frequency") { m_frequency.store(value, std::memory_order_relaxed); return true; }
        if (id == "reduction") { m_reduction.store(value, std::memory_order_relaxed); return true; }
        if (id == "speed") { m_speed.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "deesser"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "threshold") { value = m_threshold.load(std::memory_order_relaxed); return true; }
        if (id == "frequency") { value = m_frequency.load(std::memory_order_relaxed); return true; }
        if (id == "reduction") { value = m_reduction.load(std::memory_order_relaxed); return true; }
        if (id == "speed") { value = m_speed.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_frequency;
    std::atomic<float> m_reduction;
    std::atomic<float> m_speed;
    float m_lpf_state = 0.0f;
    float m_envelope = 0.0f;
    float m_current_gain = 1.0f;
};

#endif
