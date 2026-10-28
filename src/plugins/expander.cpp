#include "plugins/expander.h"
#include "processor_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>

ExpanderProcessor::ExpanderProcessor(float threshold, float ratio, float attack, float release, float range)
    : m_threshold(threshold), m_ratio(ratio), m_attack(attack), m_release(release), m_range(range) {}

void ExpanderProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float threshold = m_threshold.load(std::memory_order_relaxed);
    const float ratio = m_ratio.load(std::memory_order_relaxed);
    const float attack = m_attack.load(std::memory_order_relaxed);
    const float release = m_release.load(std::memory_order_relaxed);
    const float range = m_range.load(std::memory_order_relaxed);

    // Precompute coefficients for the 48kHz sample rate
    const float attackCoef = std::exp(-1.0f / (48000.0f * attack));
    const float releaseCoef = std::exp(-1.0f / (48000.0f * release));

    for (size_t i = 0; i < frames * channels; ++i) {
        float input = std::abs(buffer[i]);
        
        // Update envelope with separate attack/release
        float coef = (input > m_envelope) ? attackCoef : releaseCoef;
        m_envelope = coef * m_envelope + (1.0f - coef) * input;

        float gain = 1.0f;
        if (m_envelope < threshold && threshold > 0.001f) {
            // Downward expansion: reduce gain when signal is below threshold
            // Linear approximation: gain drops from 1.0 (at threshold) down to 1/ratio (at zero)
            float reduction = 1.0f - (1.0f - (1.0f / ratio)) * (1.0f - (m_envelope / threshold));
            
            // Apply range limit (minimum gain)
            gain = std::max(reduction, range);
        }

        buffer[i] *= gain;
    }
}

void register_expander_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "expander", "Expander", "Reduces low-level signals to clean up background noise.",
        {
            {"threshold", "Threshold", "number", 0.1, 0.0, 1.0, 0.01, "linear", {}},
            {"ratio", "Ratio", "number", 2.0, 1.0, 10.0, 0.1, ":1", {}},
            {"attack", "Attack", "number", 0.01, 0.001, 1.0, 0.001, "s", {}},
            {"release", "Release", "number", 0.1, 0.001, 1.0, 0.001, "s", {}},
            {"range", "Min Gain", "number", 0.1, 0.0, 1.0, 0.01, "linear", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<ExpanderProcessor>(
                params.at("threshold").get<float>(),
                params.at("ratio").get<float>(),
                params.at("attack").get<float>(),
                params.at("release").get<float>(),
                params.at("range").get<float>()
            );
        }
    });
}
