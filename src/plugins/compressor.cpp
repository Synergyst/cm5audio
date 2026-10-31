#include "plugins/compressor.h"
#include "processor_registry.h"
#include <algorithm>
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
    if (!buffer || frames == 0 || channels == 0) return;

    const float threshold = m_threshold.load(std::memory_order_relaxed);
    const float ratio = std::max(1.0f, m_ratio.load(std::memory_order_relaxed));

    // Detect the peak across the complete frame, then apply one shared gain
    // value to every channel in that frame. This is linked multichannel
    // compression: a transient on either side cannot pull the stereo image
    // toward that channel by advancing the detector state separately per
    // interleaved sample. For mono, the result is the original algorithm.
    for (size_t frame = 0; frame < frames; ++frame) {
        float framePeak = 0.0f;
        const size_t frameOffset = frame * static_cast<size_t>(channels);
        for (unsigned channel = 0; channel < channels; ++channel)
            framePeak = std::max(framePeak, std::abs(buffer[frameOffset + channel]));

        float targetGain = 1.0f;
        if (framePeak > threshold) {
            targetGain = threshold + (framePeak - threshold) / ratio;
            targetGain /= framePeak;
        }

        const float coefficient = (targetGain < m_envelope)
            ? m_attackCoef : m_releaseCoef;
        m_envelope = coefficient * m_envelope +
                     (1.0f - coefficient) * targetGain;

        for (unsigned channel = 0; channel < channels; ++channel)
            buffer[frameOffset + channel] *= m_envelope;
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
    descriptor.chain_channel_mode = ProcessorChainChannelMode::MULTICHANNEL;
    descriptor.audio_output = {1, 8, 2};
    registry.register_processor(std::move(descriptor));
}
