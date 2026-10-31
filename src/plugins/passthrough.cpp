#include "plugins/passthrough.h"
#include "processor_registry.h"
#include <memory>

void register_passthrough_processor(ProcessorRegistry& registry) {
    ProcessorDescriptor descriptor{"passthrough", "Passthrough",
        "Passes audio through unchanged.", {},
        [](const nlohmann::json&) { return std::make_unique<PassthroughProcessor>(); }};
    descriptor.audio_input = {1, 8, 2};
    descriptor.chain_channel_mode = ProcessorChainChannelMode::MULTICHANNEL;
    descriptor.audio_output = {1, 8, 2};
    registry.register_processor(std::move(descriptor));
}
