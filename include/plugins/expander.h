#ifndef EXPANDER_PROCESSOR_H
#define EXPANDER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <string>

class ExpanderProcessor : public AudioProcessor {
public:
    ExpanderProcessor(float threshold, float ratio, float attack, float release, float range);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "threshold" || id == "ratio" || id == "attack" || id == "release" || id == "range";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "threshold") { m_threshold.store(value, std::memory_order_relaxed); return true; }
        if (id == "ratio") { m_ratio.store(value, std::memory_order_relaxed); return true; }
        if (id == "attack") { m_attack.store(value, std::memory_order_relaxed); return true; }
        if (id == "release") { m_release.store(value, std::memory_order_relaxed); return true; }
        if (id == "range") { m_range.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "expander"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "threshold") { value = m_threshold.load(std::memory_order_relaxed); return true; }
        if (id == "ratio") { value = m_ratio.load(std::memory_order_relaxed); return true; }
        if (id == "attack") { value = m_attack.load(std::memory_order_relaxed); return true; }
        if (id == "release") { value = m_release.load(std::memory_order_relaxed); return true; }
        if (id == "range") { value = m_range.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_ratio;
    std::atomic<float> m_attack;
    std::atomic<float> m_release;
    std::atomic<float> m_range;
    float m_envelope = 0.0f;
};

#endif
