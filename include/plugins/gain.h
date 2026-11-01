#ifndef CM5AUDIO_PLUGIN_GAIN_H
#define CM5AUDIO_PLUGIN_GAIN_H

#include "audio_processor.h"
#include <atomic>
#include <string>

class GainProcessor : public AudioProcessor {
public:
    explicit GainProcessor(float gain = 1.0f) : m_gain(gain) {}
    void set_gain(float gain) { m_gain = gain; }
    float get_gain() const { return m_gain; }
    void process(float* buffer, size_t frames, unsigned channels) override;

    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "gain";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id != "gain") return false;
        m_gain.store(value, std::memory_order_relaxed);
        return true;
    }

    const char* parameter_control_type() const noexcept override { return "gain"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "gain") { value = m_gain.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_gain;
};

#endif
