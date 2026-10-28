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

    for (size_t i = 0; i < frames * channels; ++i) {
        float x = buffer[i];
        float y = alpha * (m_z2 + x - m_z1);
        m_z1 = x;
        m_z2 = y;
        buffer[i] = y;
    }
}

void register_hpf_processor(ProcessorRegistry& registry) {
    registry.register_processor({"hpf", "High-Pass Filter", "Removes low-frequency rumble.",
        {{"cutoff", "Cutoff Frequency", "number", 80.0, 20.0, 2000.0, 1.0, "Hz", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<HpfProcessor>(params.at("cutoff").get<float>());
        }});
}
