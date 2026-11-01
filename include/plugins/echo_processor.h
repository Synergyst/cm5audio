#ifndef CM5AUDIO_PLUGIN_ECHO_H
#define CM5AUDIO_PLUGIN_ECHO_H

#include "audio_processor.h"
#include <atomic>
#include <string>
#include <vector>

class EchoProcessor : public AudioProcessor {
public:
    EchoProcessor(float delay_ms = 500.0f, float feedback = 0.5f, float mix = 0.5f, float sampleRate = 48000.0f);

    void set_delay_ms(float ms) { m_delayMs = ms; }
    void set_feedback(float fb) { m_feedback = fb; }
    void set_mix(float mix) { m_mix = mix; }

    void process(float* buffer, size_t frames, unsigned channels) override;

    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "delay_ms" || id == "feedback" || id == "mix";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "delay_ms") m_delayMs.store(value, std::memory_order_relaxed);
        else if (id == "feedback") m_feedback.store(value, std::memory_order_relaxed);
        else if (id == "mix") m_mix.store(value, std::memory_order_relaxed);
        else return false;
        return true;
    }

    const char* parameter_control_type() const noexcept override { return "echo"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "delay_ms") { value = m_delayMs.load(std::memory_order_relaxed); return true; }
        if (id == "feedback") { value = m_feedback.load(std::memory_order_relaxed); return true; }
        if (id == "mix") { value = m_mix.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_delayMs;
    std::atomic<float> m_feedback;
    std::atomic<float> m_mix;
    float m_sampleRate;

    std::vector<float> m_buffer;
    size_t m_writePos = 0;
};

#endif
