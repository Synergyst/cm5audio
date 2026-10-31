#include "plugins/compressor.h"
#include "processor_registry.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <memory>

CompressorProcessor::CompressorProcessor(float threshold, float ratio,
                                         float attack, float release,
                                         float sampleRate)
    : m_threshold(threshold), m_ratio(ratio), m_attack(attack), m_release(release),
      m_sampleRate(sampleRate) {
    m_attackCoef = std::exp(-1.0f / (m_sampleRate * m_attack));
    m_releaseCoef = std::exp(-1.0f / (m_sampleRate * m_release));
}

void CompressorProcessor::process(float* buffer, size_t frames, unsigned channels) {
    for (size_t i = 0; i < frames * channels; ++i) {
        float input = buffer[i];
        float absInput = std::abs(input);
        float targetGain = 1.0f;
        if (absInput > m_threshold) {
            targetGain = m_threshold + (absInput - m_threshold) / m_ratio;
            targetGain /= absInput;
        }
        float coef = (targetGain < m_envelope) ? m_attackCoef : m_releaseCoef;
        m_envelope = coef * m_envelope + (1.0f - coef) * targetGain;
        buffer[i] *= m_envelope;
    }
}

namespace {
ProcessorParameter threshold_parameter() {
    return {"threshold", "Threshold", "number", 0.5, 0.0, 1.0, 0.01, "linear", {}};
}
ProcessorParameter ratio_parameter() {
    return {"ratio", "Ratio", "number", 4.0, 1.0, 100.0, 0.1, ":1", {}};
}
ProcessorParameter attack_parameter() {
    return {"attack", "Attack", "number", 0.01, 0.0001, 10.0, 0.0001, "s", {}};
}
ProcessorParameter release_parameter() {
    return {"release", "Release", "number", 0.1, 0.0001, 10.0, 0.0001, "s", {}};
}
}

void register_compressor_processor(ProcessorRegistry& registry) {
    ProcessorDescriptor descriptor{"compressor", "Compressor",
        "Hard-knee compressor. Threshold is linear amplitude, not dBFS; channels share gain reduction.",
        {threshold_parameter(), ratio_parameter(), attack_parameter(), release_parameter()},
        [](const nlohmann::json& params) {
            return std::make_unique<CompressorProcessor>(
                params.at("threshold").get<float>(), params.at("ratio").get<float>(),
                params.at("attack").get<float>(), params.at("release").get<float>());
        }};
    descriptor.audio_input = {1, 8, 2};
    descriptor.audio_output = {1, 8, 2};
    registry.register_processor(std::move(descriptor));
}
