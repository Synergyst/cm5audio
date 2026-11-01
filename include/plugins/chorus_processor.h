#ifndef CM5AUDIO_PLUGIN_CHORUS_H
#define CM5AUDIO_PLUGIN_CHORUS_H

#include "audio_processor.h"
#include <atomic>
#include <string>
#include <vector>

class ChorusProcessor : public AudioProcessor {
public:
    ChorusProcessor(float rate_hz = 1.5f, float depth = 0.3f, float mix = 0.5f, float sampleRate = 48000.0f);

    void set_rate_hz(float rate) { m_rateHz = rate; }
    void set_depth(float depth) { m_depth = depth; }
    void set_mix(float mix) { m_mix = mix; }

    void process(float* buffer, size_t frames, unsigned channels) override;

    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "rate_hz" || id == "depth" || id == "mix";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "rate_hz") m_rateHz.store(value, std::memory_order_relaxed);
        else if (id == "depth") m_depth.store(value, std::memory_order_relaxed);
        else if (id == "mix") m_mix.store(value, std::memory_order_relaxed);
        else return false;
        return true;
    }

    const char* parameter_control_type() const noexcept override { return "chorus"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "rate_hz") { value = m_rateHz.load(std::memory_order_relaxed); return true; }
        if (id == "depth") { value = m_depth.load(std::memory_order_relaxed); return true; }
        if (id == "mix") { value = m_mix.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_rateHz;
    std::atomic<float> m_depth;
    std::atomic<float> m_mix;
    float m_sampleRate;

    std::vector<float> m_buffer;
    size_t m_writePos = 0;
    float m_lfoPhase = 0.0f;
};

#endif
