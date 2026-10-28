#include "plugins/deesser.h"
#include "processor_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>

DeEsserProcessor::DeEsserProcessor(float threshold, float frequency, float reduction, float speed)
    : m_threshold(threshold), m_frequency(frequency), m_reduction(reduction), m_speed(speed) {}

void DeEsserProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float threshold = m_threshold.load(std::memory_order_relaxed);
    const float frequency = m_frequency.load(std::memory_order_relaxed);
    const float reduction = m_reduction.load(std::memory_order_relaxed);
    const float speed = m_speed.load(std::memory_order_relaxed);

    // Calculate coefficients for a simple 1-pole High Pass (implemented as Signal - LPF)
    // Sample rate = 48000Hz
    const float alpha_hpf = static_cast<float>(1.0 / (1.0 + (2.0 * M_PI * frequency / 48000.0)));
    const float alpha_env = static_cast<float>(1.0 / (48000.0 * speed));

    for (size_t i = 0; i < frames; ++i) {
        // Use the first channel for the side-chain trigger
        float sample = buffer[i * channels];
        
        // Simple High Pass Filter: High-passed = Original - Low-passed
        m_lpf_state = m_lpf_state + alpha_hpf * (sample - m_lpf_state);
        float high_passed = sample - m_lpf_state;

        // Envelope follower for the high-frequency content
        float abs_high = std::abs(high_passed);
        m_envelope = m_envelope + alpha_env * (abs_high - m_envelope);

        // Determine target gain based on threshold
        float target_gain = 1.0f;
        if (m_envelope > threshold) {
            // Linear interpolation of reduction based on how far over the threshold we are
            float over = (m_envelope - threshold) / (1.0f - threshold);
            target_gain = 1.0f - (over * (1.0f - reduction));
            target_gain = std::max(target_gain, reduction);
        }

        // Smooth the gain transition
        m_current_gain = m_current_gain + alpha_env * (target_gain - m_current_gain);

        // Apply gain to all channels in the frame
        for (unsigned c = 0; c < channels; ++c) {
            buffer[i * channels + c] *= m_current_gain;
        }
    }
}

void register_deesser_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "deesser", "De-Esser", "Attenuates harsh high-frequency sibilance (S/T sounds).",
        {
            {"threshold", "Threshold", "number", 0.1, 0.01, 1.0, 0.01, "linear", {}},
            {"frequency", "Sibilance Freq", "number", 6000.0, 1000.0, 15000.0, 100.0, "Hz", {}},
            {"reduction", "Reduction", "number", 0.5, 0.1, 1.0, 0.05, "linear", {}},
            {"speed", "Response Speed", "number", 0.01, 0.001, 0.1, 0.001, "s", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<DeEsserProcessor>(
                params.at("threshold").get<float>(),
                params.at("frequency").get<float>(),
                params.at("reduction").get<float>(),
                params.at("speed").get<float>()
            );
        }
    });
}
