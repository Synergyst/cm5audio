#ifndef PROCESSOR_REGISTRY_H
#define PROCESSOR_REGISTRY_H

#include "audio_processor.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ProcessorChoice {
    std::string label;
    nlohmann::json value;
};

// Supported UI types: number, integer, boolean, string, enum. Units are display-only.
struct ProcessorParameter {
    std::string id;
    std::string name;
    std::string type;
    nlohmann::json default_value;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> step;
    std::string unit;
    std::vector<ProcessorChoice> choices;
};

struct ProcessorChannelLayout {
    unsigned minimum = 1;
    unsigned maximum = 1;
    unsigned default_channels = 1;
};

enum class ProcessorPortType { AUDIO, MIDI };
enum class ProcessorPortDirection { INPUT, OUTPUT };

// How a processor participates when embedded in a composite plugin chain.
// PER_CHANNEL creates an independent processor instance/state for every bus
// channel. MULTICHANNEL runs one instance over the complete interleaved bus.
// MONO_ONLY rejects composite chains wider than one channel.
enum class ProcessorChainChannelMode { PER_CHANNEL, MULTICHANNEL, MONO_ONLY };

inline const char* processor_chain_channel_mode_name(ProcessorChainChannelMode mode) noexcept {
    switch (mode) {
        case ProcessorChainChannelMode::MULTICHANNEL: return "multichannel";
        case ProcessorChainChannelMode::MONO_ONLY: return "mono_only";
        case ProcessorChainChannelMode::PER_CHANNEL:
        default: return "per_channel";
    }
}

inline const char* processor_chain_channel_mode_description(ProcessorChainChannelMode mode) noexcept {
    switch (mode) {
        case ProcessorChainChannelMode::MULTICHANNEL:
            return "One processor instance handles the complete interleaved audio bus.";
        case ProcessorChainChannelMode::MONO_ONLY:
            return "This processor is restricted to a one-channel chain.";
        case ProcessorChainChannelMode::PER_CHANNEL:
        default:
            return "An independent mono processor instance and state are used for each channel.";
    }
}

inline const char* processor_port_type_name(ProcessorPortType type) noexcept {
    return type == ProcessorPortType::AUDIO ? "audio" : "midi";
}

inline const char* processor_port_direction_name(ProcessorPortDirection direction) noexcept {
    return direction == ProcessorPortDirection::INPUT ? "input" : "output";
}

// A plugin may declare any bounded number of named ports. Audio ports carry a
// channel layout; MIDI ports carry event streams and therefore have no channel
// count. source_parameter optionally names a string/enum parameter used to
// choose the external MIDI source for an input port (legacy MIDI plugins).
struct ProcessorPortDescriptor {
    std::string id;
    std::string name;
    ProcessorPortType type = ProcessorPortType::AUDIO;
    ProcessorPortDirection direction = ProcessorPortDirection::INPUT;
    ProcessorChannelLayout channels{};
    std::string source_parameter;
};

struct ProcessorDescriptor {
    using Factory = std::function<std::unique_ptr<AudioProcessor>(const nlohmann::json&)>;
    std::string type;
    std::string name;
    std::string description;
    std::vector<ProcessorParameter> parameters;
    Factory factory;

    // Compatibility aliases for the original one-input/one-output processor
    // interface. New code should declare `ports`; these fields are normalized
    // to the audio_in/audio_out ports during registration.
    ProcessorChannelLayout audio_input;
    ProcessorChannelLayout audio_output;
    std::vector<ProcessorPortDescriptor> ports;

    // Defines how this processor is run inside a composite chain. Legacy
    // mono-oriented effects default to independent per-channel instances.
    ProcessorChainChannelMode chain_channel_mode = ProcessorChainChannelMode::PER_CHANNEL;
};

// Registration is startup-only. Discovery and creation are read-only and safe
// concurrently after built-ins and application processors have been registered.
class ProcessorRegistry {
public:
    static ProcessorRegistry& instance();

    bool register_processor(ProcessorDescriptor descriptor);
    std::unique_ptr<AudioProcessor> create(const std::string& type,
                                           const nlohmann::json& params,
                                           nlohmann::json& normalized) const;
    nlohmann::json discovery() const;
    const ProcessorDescriptor* find(const std::string& type) const;

private:
    ProcessorRegistry();
    static void validate_value(const ProcessorParameter& parameter,
                               const nlohmann::json& value);

    std::map<std::string, ProcessorDescriptor> m_processors;
};

void register_builtin_processors(ProcessorRegistry& registry);

#endif
