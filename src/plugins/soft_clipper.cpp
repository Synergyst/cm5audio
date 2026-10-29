#include "plugins/soft_clipper.h"
#include "processor_registry.h"
#include <cmath>
#include <algorithm>
#include <memory>

SoftClipperProcessor::SoftClipperProcessor(float drive, float ceiling)
    : m_drive(drive), m_ceiling(ceiling) {}

void SoftClipperProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float drive = m_drive.load(std::memory_order_relaxed);
    const float ceiling = m_ceiling.load(std::memory_order_relaxed);

    for (size_t i = 0; i < frames * channels; ++i) {
        // Apply drive to push the signal into the non-linear region of tanh
        float x = buffer[i] * drive;
        
        // tanh(x) creates a smooth S-curve that naturally caps at +/- 1.0
        float clipped = std::tanh(x);
        
        // Scale the result to the user-defined ceiling
        buffer[i] = clipped * ceiling;
    }
}

void register_soft_clipper_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "soft_clipper", "Soft Clipper", "Smooth saturation to prevent harsh digital peaking.",
        {
            {"drive", "Drive", "number", 1.0, 0.1, 20.0, 0.1, "linear", {}},
            {"ceiling", "Ceiling", "number", 1.0, 0.1, 1.0, 0.01, "linear", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<SoftClipperProcessor>(
                params.at("drive").get<float>(),
                params.at("ceiling").get<float>()
            );
        }
    });
}
