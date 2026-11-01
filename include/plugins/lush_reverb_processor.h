#ifndef CM5AUDIO_PLUGIN_LUSH_REVERB_H
#define CM5AUDIO_PLUGIN_LUSH_REVERB_H

#include "audio_processor.h"
#include <atomic>
#include <string>
#include <vector>
#include <cmath>

class LushReverbProcessor : public AudioProcessor {
public:
    LushReverbProcessor(float decay = 0.5f, float diffusion = 0.5f, float mix = 0.3f, float sampleRate = 48000.0f);

    void set_decay(float decay) { m_decay.store(decay, std::memory_order_relaxed); }
    void set_diffusion(float diff) { m_diffusion.store(diff, std::memory_order_relaxed); }
    void set_mix(float mix) { m_mix.store(mix, std::memory_order_relaxed); }

    void process(float* buffer, size_t frames, unsigned channels) override;

    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "decay" || id == "diffusion" || id == "mix";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "decay") m_decay.store(value, std::memory_order_relaxed);
        else if (id == "diffusion") m_diffusion.store(value, std::memory_order_relaxed);
        else if (id == "mix") m_mix.store(value, std::memory_order_relaxed);
        else return false;
        return true;
    }

    const char* parameter_control_type() const noexcept override { return "lush_reverb"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "decay") { value = m_decay.load(std::memory_order_relaxed); return true; }
        if (id == "diffusion") { value = m_diffusion.load(std::memory_order_relaxed); return true; }
        if (id == "mix") { value = m_mix.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    struct ModulatedComb {
        std::vector<float> buffer;
        size_t pos = 0;
        float feedback = 0.0f;
        float lfoPhase = 0.0f;
        float lfoInc = 0.0f;

        float process(float input, float modulation) {
            float mod = std::sin(lfoPhase) * modulation;
            float readPos = static_cast<float>(pos) - mod;
            float sizeF = static_cast<float>(buffer.size());
            while (readPos < 0.0f) readPos += sizeF;
            while (readPos >= sizeF) readPos -= sizeF;
            size_t i0 = static_cast<size_t>(readPos);
            size_t i1 = (i0 + 1) % buffer.size();
            i0 = i0 % buffer.size();
            float frac = readPos - static_cast<float>(i0);
            float output = buffer[i0] * (1.0f - frac) + buffer[i1] * frac;
            float fbVal = input + (output * feedback);
            if (std::isnan(fbVal) || std::isinf(fbVal)) fbVal = 0.0f;
            buffer[pos] = fbVal;
            pos = (pos + 1) % buffer.size();
            lfoPhase += lfoInc;
            if (lfoPhase > 2.0f * 3.14159265f) lfoPhase -= 2.0f * 3.14159265f;
            return output;
        }
    };

    struct AllPass {
        std::vector<float> buffer;
        size_t pos = 0;
        float gain = 0.5f;

        float process(float input) {
            size_t p = pos % buffer.size();
            float bufOut = buffer[p];
            float output = -gain * input + bufOut;
            float newVal = input + (gain * bufOut);
            if (std::isnan(newVal) || std::isinf(newVal)) newVal = 0.0f;
            buffer[p] = newVal;
            pos = (pos + 1) % buffer.size();
            return output;
        }
    };

    std::atomic<float> m_decay;
    std::atomic<float> m_diffusion;
    std::atomic<float> m_mix;
    float m_sampleRate;

    std::vector<ModulatedComb> m_combs;
    std::vector<AllPass> m_allpasses;
};

#endif
