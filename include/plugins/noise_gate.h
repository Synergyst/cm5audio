#ifndef NOISE_GATE_PROCESSOR_H
#define NOISE_GATE_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>
#include <string>
class NoiseGateProcessor : public AudioProcessor {
public:
    NoiseGateProcessor(float threshold, float attack, float release, float hold);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "threshold" || id == "attack" || id == "release" || id == "hold";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "threshold") { m_threshold.store(value, std::memory_order_relaxed); return true; }
        if (id == "attack") { m_attack.store(value, std::memory_order_relaxed); return true; }
        if (id == "release") { m_release.store(value, std::memory_order_relaxed); return true; }
        if (id == "hold") { m_hold.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "noise_gate"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "threshold") { value = m_threshold.load(std::memory_order_relaxed); return true; }
        if (id == "attack") { value = m_attack.load(std::memory_order_relaxed); return true; }
        if (id == "release") { value = m_release.load(std::memory_order_relaxed); return true; }
        if (id == "hold") { value = m_hold.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_attack;
    std::atomic<float> m_release;
    std::atomic<float> m_hold;
    float m_current_gain = 1.0f;
    size_t m_hold_frames = 0;
};
#endif
