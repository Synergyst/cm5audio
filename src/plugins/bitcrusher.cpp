#include "plugins/bitcrusher.h"
#include "processor_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>

BitcrusherProcessor::BitcrusherProcessor(float bits, float sample_rate)
    : m_bits(bits), m_sample_rate(sample_rate) {}

void BitcrusherProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float bits = m_bits.load(std::memory_order_relaxed);
    const float sample_rate = m_sample_rate.load(std::memory_order_relaxed);
    
    const float levels = std::pow(2.0f, bits - 1.0f);
    const float step = 1.0f / levels;
    const float factor = 48000.0f / sample_rate;

    for (size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels && c < 8; ++c) {
            float sample = buffer[i * channels + c];

            if (sample_rate < 48000.0f) {
                if (m_sample_counters[c] == 0) {
                    m_last_samples[c] = sample;
                }
                m_sample_counters[c]++;
                if (m_sample_counters[c] >= static_cast<unsigned int>(factor)) {
                    m_sample_counters[c] = 0;
                }
                sample = m_last_samples[c];
            }

            sample = std::floor(sample / step) * step;
            buffer[i * channels + c] = std::clamp(sample, -1.0f, 1.0f);
        }
    }
}

void register_bitcrusher_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "bitcrusher", "Bitcrusher", "Reduce bit-depth and sample rate for lo-fi effects.",
        {
            {"bits", "Bit Depth", "number", 16.0, 1.0, 32.0, 0.1, "bits", {}},
            {"sample_rate", "Sample Rate", "number", 48000.0, 100.0, 48000.0, 100.0, "Hz", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<BitcrusherProcessor>(
                params.at("bits").get<float>(),
                params.at("sample_rate").get<float>()
            );
        }
    });
}
