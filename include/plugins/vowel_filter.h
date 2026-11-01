#ifndef VOWEL_FILTER_PROCESSOR_H
#define VOWEL_FILTER_PROCESSOR_H

#include "audio_processor.h"
#include "constants.h"

#include <array>
#include <atomic>
#include <string>

class VowelFilterProcessor : public AudioProcessor {
public:
    VowelFilterProcessor(int vowelIndex, float resonance, float mix);

    void process(float* buffer, size_t frames, unsigned channels) override;
    void process(AudioProcessContext& context) override;

    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "resonance" || id == "mix";
    }

    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "resonance") {
            m_resonance.store(value, std::memory_order_relaxed);
            return true;
        }
        if (id == "mix") {
            m_mix.store(value, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    const char* parameter_control_type() const noexcept override {
        return "vowel_filter";
    }

    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "resonance") {
            value = m_resonance.load(std::memory_order_relaxed);
            return true;
        }
        if (id == "mix") {
            value = m_mix.load(std::memory_order_relaxed);
            return true;
        }
        return false;
    }

private:
    struct Biquad {
        float b0 = 0.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        float z1 = 0.0f;
        float z2 = 0.0f;

        void setBandPass(float frequency, float q, float sampleRate) noexcept;
        float process(float input) noexcept;
    };

    struct ChannelState {
        std::array<Biquad, 3> formants{};
    };

    std::atomic<int> m_vowel_idx;
    std::atomic<float> m_resonance;
    std::atomic<float> m_mix;

    std::array<ChannelState, CM5_MAX_CHAIN_CHANNELS> m_channels{};

    int m_last_vowel_idx = -1;
    float m_last_resonance = -1.0f;
    float m_last_sample_rate = -1.0f;

    void updateCoefficients(float sampleRate) noexcept;
    void processBuffer(float* buffer, size_t frames, unsigned channels,
                       float sampleRate) noexcept;
};

#endif
