#ifndef NORMALIZE_PROCESSOR_H
#define NORMALIZE_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>
#include <string>
class NormalizeProcessor : public AudioProcessor {
public:
    NormalizeProcessor(float target, float speed, float max_gain);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "target" || id == "speed" || id == "max_gain";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "target") { m_target.store(value, std::memory_order_relaxed); return true; }
        if (id == "speed") { m_speed.store(value, std::memory_order_relaxed); return true; }
        if (id == "max_gain") { m_max_gain.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "normalize"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "target") { value = m_target.load(std::memory_order_relaxed); return true; }
        if (id == "speed") { value = m_speed.load(std::memory_order_relaxed); return true; }
        if (id == "max_gain") { value = m_max_gain.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_target;
    std::atomic<float> m_speed;
    std::atomic<float> m_max_gain;
    float m_envelope = 0.0f;
    float m_current_gain = 1.0f;
};
#endif
