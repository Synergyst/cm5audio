#include "plugins/lpf_processor.h"
#include "processor_registry.h"
#include <cmath>
#include <memory>

LpfProcessor::LpfProcessor(float cutoff) : m_cutoff(cutoff) {}

void LpfProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float cutoff = m_cutoff.load(std::memory_order_relaxed);
    const float rc = 1.0f / (2.0f * 3.14159265f * cutoff);
    const float dt = 1.0f / 48000.0f;
    const float alpha = dt / (rc + dt);

    if (m_z.size() != channels) m_z.assign(channels, 0.0f);

    for (size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            float x = buffer[i * channels + c];
            float y = m_z[c] + alpha * (x - m_z[c]);
            m_z[c] = y;
            buffer[i * channels + c] = y;
        }
    }
}

void register_lpf_processor(ProcessorRegistry& registry) {
    registry.register_processor({"lpf", "Low-Pass Filter", "Removes high-frequency hiss.",
        {{"cutoff", "Cutoff Frequency", "number", 15000.0, 1000.0, 20000.0, 1.0, "Hz", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<LpfProcessor>(params.at("cutoff").get<float>());
        }});
}
