#include "plugins/hpf_processor.h"
#include "processor_registry.h"
#include <cmath>
#include <memory>

HpfProcessor::HpfProcessor(float cutoff) : m_cutoff(cutoff) {}

void HpfProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float cutoff = m_cutoff.load(std::memory_order_relaxed);
    const float rc = 1.0f / (2.0f * 3.14159265f * cutoff);
    const float dt = 1.0f / 48000.0f;
    const float alpha = dt / (rc + dt);

    if (m_z.size() != channels) m_z.assign(channels, 0.0f);

    for (size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            float x = buffer[i * channels + c];
            // HPF is simply: Original - LPF(Original)
            float lpf = m_z[c] + alpha * (x - m_z[c]);
            m_z[c] = lpf;
            buffer[i * channels + c] = x - lpf;
        }
    }
}

void register_hpf_processor(ProcessorRegistry& registry) {
    registry.register_processor({"hpf", "High-Pass Filter", "Removes low-frequency rumble.",
        {{"cutoff", "Cutoff Frequency", "number", 80.0, 20.0, 2000.0, 1.0, "Hz", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<HpfProcessor>(params.at("cutoff").get<float>());
        }});
}
