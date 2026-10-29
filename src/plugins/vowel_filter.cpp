#include "plugins/vowel_filter.h"
#include "processor_registry.h"
#include <cmath>
#include <algorithm>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void VowelFilterProcessor::Biquad::setBandPass(float freq, float Q, float sampleRate) {
    float omega = 2.0f * (float)M_PI * freq / sampleRate;
    float sn = std::sin(omega);
    float cs = std::cos(omega);
    float alpha = sn / (2.0f * Q);

    float a0 = 1.0f + alpha;
    b0 = alpha / a0;
    b1 = 0;
    b2 = -alpha / a0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

float VowelFilterProcessor::Biquad::process(float in) {
    float out = b0 * in + z1;
    z1 = b1 * in - a1 * out + z2;
    z2 = b2 * in - a2 * out;
    return out;
}

VowelFilterProcessor::VowelFilterProcessor(int vowel_idx, float resonance, float mix)
    : m_vowel_idx(vowel_idx), m_resonance(resonance), m_mix(mix) {}

void VowelFilterProcessor::updateCoefficients() {
    int current = m_vowel_idx.load(std::memory_order_relaxed);
    if (current == m_last_vowel_idx) return;

    float f1 = 0, f2 = 0;
    switch(current) {
        case 0: f1 = 700.0f; f2 = 1220.0f; break; // a
        case 1: f1 = 500.0f; f2 = 1780.0f; break; // e
        case 2: f1 = 300.0f; f2 = 2250.0f; break; // i
        case 3: f1 = 400.0f; f2 = 800.0f;  break; // o
        case 4: f1 = 300.0f; f2 = 620.0f;  break; // u
        default: f1 = 500.0f; f2 = 1500.0f; break;
    }

    float Q = m_resonance.load(std::memory_order_relaxed);
    m_f1.setBandPass(f1, Q, 48000.0f);
    m_f2.setBandPass(f2, Q, 48000.0f);
    m_last_vowel_idx = current;
}

void VowelFilterProcessor::process(float* buffer, size_t frames, unsigned channels) {
    updateCoefficients();
    const float mix = m_mix.load(std::memory_order_relaxed);

    for (size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            float in = buffer[i * channels + c];
            float filtered = m_f1.process(in) + m_f2.process(in);
            buffer[i * channels + c] = (in * (1.0f - mix)) + (filtered * mix);
        }
    }
}

void register_vowel_filter_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "vowel_filter", "Vowel Filter", "Simulates vocal tract formants (Talkbox effect).",
        {
            {"vowel", "Vowel", "enum", "a", {}, {}, {}, "", {{"A", "a"}, {"E", "e"}, {"I", "i"}, {"O", "o"}, {"U", "u"}}},
            {"resonance", "Resonance", "number", 10.0, 1.0, 50.0, 0.1, "Q", {}},
            {"mix", "Mix", "number", 0.8, 0.0, 1.0, 0.01, "linear", {}}
        },
        [](const nlohmann::json& params) {
            std::string v = params.at("vowel").get<std::string>();
            int idx = 5;
            if (v == "a") idx = 0;
            else if (v == "e") idx = 1;
            else if (v == "i") idx = 2;
            else if (v == "o") idx = 3;
            else if (v == "u") idx = 4;
            
            return std::make_unique<VowelFilterProcessor>(
                idx,
                params.at("resonance").get<float>(),
                params.at("mix").get<float>()
            );
        }
    });
}
