#ifndef LIMITER_PROCESSOR_H
#define LIMITER_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>
#include <string>
#include <vector>
class LimiterProcessor : public AudioProcessor {
public:
    LimiterProcessor(float threshold, float attack, float release, float lookahead_ms);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "threshold" || id == "attack" || id == "release";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "threshold") { m_threshold.store(value, std::memory_order_relaxed); return true; }
        if (id == "attack") { m_attack.store(value, std::memory_order_relaxed); return true; }
        if (id == "release") { m_release.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "limiter"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "threshold") { value = m_threshold.load(std::memory_order_relaxed); return true; }
        if (id == "attack") { value = m_attack.load(std::memory_order_relaxed); return true; }
        if (id == "release") { value = m_release.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_attack;
    std::atomic<float> m_release;
    float m_envelope = 1.0f;
    std::vector<float> m_delay_buffer;
    size_t m_write_pos = 0;
    size_t m_buffer_size = 0;
};
#endif
