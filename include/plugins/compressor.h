#ifndef CM5AUDIO_PLUGIN_COMPRESSOR_H
#define CM5AUDIO_PLUGIN_COMPRESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <string>

// Basic hard-knee, linear-amplitude compressor with smoothed gain reduction.
class CompressorProcessor : public AudioProcessor {
public:
    CompressorProcessor(float threshold = 0.5f, float ratio = 4.0f,
                        float attack = 0.01f, float release = 0.1f,
                        float sampleRate = 48000.0f);

    void set_threshold(float threshold) { m_threshold = threshold; }
    void set_ratio(float ratio) { m_ratio = ratio; }
    float get_threshold() const { return m_threshold; }
    float get_ratio() const { return m_ratio; }
    void process(float* buffer, size_t frames, unsigned channels) override;

    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "threshold" || id == "ratio";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "threshold") m_threshold.store(value, std::memory_order_relaxed);
        else if (id == "ratio") m_ratio.store(value, std::memory_order_relaxed);
        else return false;
        return true;
    }

    const char* parameter_control_type() const noexcept override { return "compressor"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "threshold") { value = m_threshold.load(std::memory_order_relaxed); return true; }
        if (id == "ratio") { value = m_ratio.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_ratio;
    float m_attack;
    float m_release;
    float m_sampleRate;
    float m_attackCoef;
    float m_releaseCoef;
    float m_envelope = 1.0f;
};

#endif
