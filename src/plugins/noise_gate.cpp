#include "plugins/noise_gate.h"
#include "processor_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>

NoiseGateProcessor::NoiseGateProcessor(float threshold, float attack, float release, float hold)
    : m_threshold(threshold), m_attack(attack), m_release(release), m_hold(hold) {}

void NoiseGateProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float threshold = m_threshold.load(std::memory_order_relaxed);
    const float attack = m_attack.load(std::memory_order_relaxed);
    const float release = m_release.load(std::memory_order_relaxed);
    const float hold = m_hold.load(std::memory_order_relaxed);

    const float attackCoef = static_cast<float>(1.0 / (48000.0 * attack));
    const float releaseCoef = static_cast<float>(1.0 / (48000.0 * release));
    const size_t holdFrames = static_cast<size_t>(hold * 48000.0);

    for (size_t i = 0; i < frames; ++i) {
        float peak = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            peak = std::max(peak, std::abs(buffer[i * channels + c]));
        }

        if (peak > threshold) {
            m_hold_frames = holdFrames;
        } else if (m_hold_frames > 0) {
            m_hold_frames--;
        }

        float targetGain = (peak > threshold || m_hold_frames > 0) ? 1.0f : 0.0f;
        float coef = (targetGain > m_current_gain) ? attackCoef : releaseCoef;
        
        m_current_gain = m_current_gain + coef * (targetGain - m_current_gain);

        for (unsigned c = 0; c < channels; ++c) {
            buffer[i * channels + c] *= m_current_gain;
        }
    }
}

void register_noise_gate_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "noise_gate", "Noise Gate", "Mutes audio below a certain threshold to remove background noise.",
        {
            {"threshold", "Threshold", "number", 0.01, 0.0, 1.0, 0.001, "linear", {}},
            {"attack", "Attack", "number", 0.01, 0.001, 0.5, 0.001, "s", {}},
            {"release", "Release", "number", 0.1, 0.001, 1.0, 0.001, "s", {}},
            {"hold", "Hold", "number", 0.05, 0.0, 0.5, 0.001, "s", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<NoiseGateProcessor>(
                params.at("threshold").get<float>(),
                params.at("attack").get<float>(),
                params.at("release").get<float>(),
                params.at("hold").get<float>()
            );
        }
    });
}
