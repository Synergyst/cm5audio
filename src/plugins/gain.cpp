#include "plugins/gain.h"
#include "processor_registry.h"
#include <memory>

void GainProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float gain = m_gain.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames * channels; ++i) buffer[i] *= gain;
}

void register_gain_processor(ProcessorRegistry& registry) {
    ProcessorDescriptor descriptor{"gain", "Gain",
        "Linear amplitude multiplier (1 = unity, 0 = silence).",
        {{"gain", "Gain", "number", 1.0, 0.0, 8.0, 0.01, "linear", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<GainProcessor>(params.at("gain").get<float>());
        }};
    descriptor.audio_input = {1, 8, 2};
    descriptor.chain_channel_mode = ProcessorChainChannelMode::MULTICHANNEL;
    descriptor.audio_output = {1, 8, 2};
    registry.register_processor(std::move(descriptor));
}
