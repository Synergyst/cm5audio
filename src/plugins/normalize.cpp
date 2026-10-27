#include "plugins/normalize.h"
#include "processor_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>

NormalizeProcessor::NormalizeProcessor(float target, float speed, float max_gain)
    : m_target(target), m_speed(speed), m_max_gain(max_gain) {}

void NormalizeProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float target = m_target.load(std::memory_order_relaxed);
    const float speed = m_speed.load(std::memory_order_relaxed);
    const float max_gain = m_max_gain.load(std::memory_order_relaxed);

    // Use double for the coefficient calculation to maintain precision, then cast to float
    // Sample rate is 48kHz
    const float alpha = static_cast<float>(1.0 / (48000.0 * speed));

    for (size_t i = 0; i < frames; ++i) {
        float peak = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            peak = std::max(peak, std::abs(buffer[i * channels + c]));
        }

        m_envelope = m_envelope + alpha * (peak - m_envelope);

        float desired_gain = target / (m_envelope + 0.001f);
        desired_gain = std::clamp(desired_gain, 0.1f, max_gain);
        m_current_gain = m_current_gain + alpha * (desired_gain - m_current_gain);

        for (unsigned c = 0; c < channels; ++c) {
            float sample = buffer[i * channels + c] * m_current_gain;
            buffer[i * channels + c] = std::clamp(sample, -1.0f, 1.0f);
        }
    }
}

void register_normalize_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "normalize", "Normalize", "Automatic gain control for consistent VOIP levels.",
        {
            {"target", "Target Level", "number", 0.5, 0.01, 1.0, 0.01, "linear", {}},
            {"speed", "Adaptation Speed", "number", 0.1, 0.001, 1.0, 0.001, "s", {}},
            {"max_gain", "Max Boost", "number", 4.0, 1.0, 10.0, 0.1, "linear", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<NormalizeProcessor>(
                params.at("target").get<float>(),
                params.at("speed").get<float>(),
                params.at("max_gain").get<float>()
            );
        }
    });
}
