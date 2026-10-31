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

// Supported UI types: number, integer, boolean, string, enum. Units are display-only:
// the factory always receives the units declared here (no implicit dB conversion).
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

// One interleaved audio bus. Existing processors default to mono until their
// implementation explicitly declares a wider supported layout.
struct ProcessorChannelLayout {
    unsigned minimum = 1;
    unsigned maximum = 1;
    unsigned default_channels = 1;
};

struct ProcessorDescriptor {
    using Factory = std::function<std::unique_ptr<AudioProcessor>(const nlohmann::json&)>;
    std::string type;
    std::string name;
    std::string description;
    std::vector<ProcessorParameter> parameters;
    Factory factory;
    ProcessorChannelLayout audio_input;
    ProcessorChannelLayout audio_output;
};

// Registration is startup-only. Discovery and creation are read-only and safe
// concurrently after built-ins and any application processors have been registered.
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

// Explicitly called by ProcessorRegistry construction; each built-in keeps its
// descriptor and factory alongside its implementation in src/plugins/.
void register_builtin_processors(ProcessorRegistry& registry);

#endif
