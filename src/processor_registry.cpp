#include "processor_registry.h"
#include "constants.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

ProcessorRegistry& ProcessorRegistry::instance() {
    static ProcessorRegistry registry;
    return registry;
}

ProcessorRegistry::ProcessorRegistry() {
    register_builtin_processors(*this);
}

bool ProcessorRegistry::register_processor(ProcessorDescriptor descriptor) {
    if (descriptor.type.empty() || descriptor.name.empty() || !descriptor.factory)
        throw std::invalid_argument("Processor registration requires type, name and factory");
    if (m_processors.count(descriptor.type))
        throw std::invalid_argument("Duplicate processor type: " + descriptor.type);

    std::set<std::string> parameterIds;
    for (const auto& parameter : descriptor.parameters) {
        if (parameter.id.empty() || !parameterIds.insert(parameter.id).second)
            throw std::invalid_argument("Empty or duplicate processor parameter: " + parameter.id);
        if (parameter.type != "number" && parameter.type != "integer" && parameter.type != "boolean" &&
            parameter.type != "string" && parameter.type != "enum")
            throw std::invalid_argument("Unsupported parameter type: " + parameter.type);
        const bool numeric = parameter.type == "number" || parameter.type == "integer";
        if ((parameter.min || parameter.max || parameter.step) && !numeric)
            throw std::invalid_argument("Numeric constraints on nonnumeric parameter: " + parameter.id);
        if ((parameter.min && !std::isfinite(*parameter.min)) ||
            (parameter.max && !std::isfinite(*parameter.max)) ||
            (parameter.step && (!std::isfinite(*parameter.step) || *parameter.step <= 0)) ||
            (parameter.min && parameter.max && *parameter.min > *parameter.max))
            throw std::invalid_argument("Invalid numeric constraints: " + parameter.id);
        if (parameter.type == "enum") {
            if (parameter.choices.empty())
                throw std::invalid_argument("Empty enum choices: " + parameter.id);
            for (size_t i = 0; i < parameter.choices.size(); ++i) {
                const auto& value = parameter.choices[i].value;
                if (value.is_null() || !value.is_primitive() ||
                    (value.is_number() && !std::isfinite(value.get<double>())))
                    throw std::invalid_argument("Enum choices must be finite scalar values: " + parameter.id);
                for (size_t j = 0; j < i; ++j) {
                    if (parameter.choices[j].value == value)
                        throw std::invalid_argument("Duplicate enum choice: " + parameter.id);
                }
            }
        } else if (!parameter.choices.empty()) {
            throw std::invalid_argument("Choices require enum type: " + parameter.id);
        }
        validate_value(parameter, parameter.default_value);
    }

    const auto validLayout = [](const ProcessorChannelLayout& layout) {
        return layout.minimum >= 1 && layout.maximum >= layout.minimum &&
               layout.maximum <= CM5_MAX_CHANNELS &&
               layout.default_channels >= layout.minimum &&
               layout.default_channels <= layout.maximum;
    };

    // Existing built-ins use the legacy descriptor fields. Normalize them to
    // two ordinary typed ports so the graph and UI have one canonical model.
    if (descriptor.ports.empty()) {
        descriptor.ports.push_back({"audio_in", "Audio Input", ProcessorPortType::AUDIO,
                                    ProcessorPortDirection::INPUT, descriptor.audio_input, {}});
        descriptor.ports.push_back({"audio_out", "Audio Output", ProcessorPortType::AUDIO,
                                    ProcessorPortDirection::OUTPUT, descriptor.audio_output, {}});
    }

    if (descriptor.ports.size() > CM5_MAX_PLUGIN_PORTS)
        throw std::invalid_argument("Plugin declares more than CM5_MAX_PLUGIN_PORTS ports: " + descriptor.type);

    std::set<std::string> portIds;
    bool hasAudioInput = false;
    bool hasAudioOutput = false;
    for (const auto& port : descriptor.ports) {
        if (port.id.empty() || !portIds.insert(port.id).second)
            throw std::invalid_argument("Empty or duplicate plugin port ID: " + port.id);
        if (port.name.empty())
            throw std::invalid_argument("Plugin port requires a name: " + port.id);
        if (port.type == ProcessorPortType::AUDIO) {
            if (!validLayout(port.channels))
                throw std::invalid_argument("Invalid audio port channel layout: " + descriptor.type + "/" + port.id);
            if (port.direction == ProcessorPortDirection::INPUT && port.id == "audio_in") {
                descriptor.audio_input = port.channels;
                hasAudioInput = true;
            }
            if (port.direction == ProcessorPortDirection::OUTPUT && port.id == "audio_out") {
                descriptor.audio_output = port.channels;
                hasAudioOutput = true;
            }
        } else {
            if (!port.source_parameter.empty()) {
                const auto found = std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                    [&](const ProcessorParameter& parameter) { return parameter.id == port.source_parameter; });
                if (found == descriptor.parameters.end() ||
                    (found->type != "string" && found->type != "enum"))
                    throw std::invalid_argument("MIDI source parameter must reference a string or enum parameter: " + port.id);
            }
        }
    }

    // Keep the old field defaults valid for APIs and older callers even when a
    // processor declares only auxiliary buses or MIDI ports.
    if (!hasAudioInput) descriptor.audio_input = ProcessorChannelLayout{1, 1, 1};
    if (!hasAudioOutput) descriptor.audio_output = ProcessorChannelLayout{1, 1, 1};

    const auto type = descriptor.type;
    m_processors.emplace(type, std::move(descriptor));
    return true;
}

std::unique_ptr<AudioProcessor> ProcessorRegistry::create(
    const std::string& type, const nlohmann::json& params, nlohmann::json& normalized) const {
    const auto it = m_processors.find(type);
    if (it == m_processors.end())
        throw std::invalid_argument("Unknown processor type: " + type);
    if (!params.is_object())
        throw std::invalid_argument("Processor params must be an object");
    const auto& descriptor = it->second;
    normalized = nlohmann::json::object();
    for (const auto& parameter : descriptor.parameters) {
        const auto found = params.find(parameter.id);
        const auto& value = found == params.end() ? parameter.default_value : *found;
        validate_value(parameter, value);
        normalized[parameter.id] = value;
    }
    for (auto parameter = params.begin(); parameter != params.end(); ++parameter) {
        if (!normalized.contains(parameter.key()))
            throw std::invalid_argument("Unknown parameter for " + type + ": " + parameter.key());
    }
    auto processor = descriptor.factory(normalized);
    if (!processor)
        throw std::runtime_error("Processor factory returned null: " + type);
    return processor;
}

const ProcessorDescriptor* ProcessorRegistry::find(const std::string& type) const {
    const auto it = m_processors.find(type);
    return it == m_processors.end() ? nullptr : &it->second;
}

nlohmann::json ProcessorRegistry::discovery() const {
    nlohmann::json processors = nlohmann::json::array();
    for (const auto& item : m_processors) {
        const auto& descriptor = item.second;
        nlohmann::json parameters = nlohmann::json::array();
        nlohmann::json defaults = nlohmann::json::object();
        nlohmann::json ports = nlohmann::json::array();
        nlohmann::json audioInputs = nlohmann::json::array();
        nlohmann::json audioOutputs = nlohmann::json::array();

        for (const auto& parameter : descriptor.parameters) {
            nlohmann::json value = {{"id", parameter.id}, {"name", parameter.name},
                                    {"type", parameter.type}, {"default", parameter.default_value}};
            if (parameter.min) value["min"] = *parameter.min;
            if (parameter.max) value["max"] = *parameter.max;
            if (parameter.step) value["step"] = *parameter.step;
            if (!parameter.unit.empty()) value["unit"] = parameter.unit;
            if (!parameter.choices.empty()) {
                value["choices"] = nlohmann::json::array();
                for (const auto& choice : parameter.choices)
                    value["choices"].push_back({{"label", choice.label}, {"value", choice.value}});
            }
            parameters.push_back(std::move(value));
            defaults[parameter.id] = parameter.default_value;
        }

        for (const auto& port : descriptor.ports) {
            nlohmann::json value = {
                {"id", port.id}, {"name", port.name},
                {"type", processor_port_type_name(port.type)},
                {"direction", processor_port_direction_name(port.direction)}
            };
            if (port.type == ProcessorPortType::AUDIO) {
                value["min_channels"] = port.channels.minimum;
                value["max_channels"] = port.channels.maximum;
                value["default_channels"] = port.channels.default_channels;
                if (port.direction == ProcessorPortDirection::INPUT)
                    audioInputs.push_back(value);
                else
                    audioOutputs.push_back(value);
            } else if (!port.source_parameter.empty()) {
                value["source_parameter"] = port.source_parameter;
            }
            ports.push_back(std::move(value));
        }

        processors.push_back({{"type", descriptor.type}, {"name", descriptor.name},
                              {"description", descriptor.description}, {"defaults", defaults},
                              {"parameters", parameters}, {"ports", std::move(ports)},
                              {"chain_channel_mode", processor_chain_channel_mode_name(descriptor.chain_channel_mode)},
                              {"chain_channel_mode_description", processor_chain_channel_mode_description(descriptor.chain_channel_mode)},
                              {"audio", {{"inputs", std::move(audioInputs)},
                                         {"outputs", std::move(audioOutputs)}}}});
    }
    return {{"schema_version", 2}, {"max_chain_length", CM5_MAX_PROCESSORS_PER_ROUTE},
            {"max_plugin_ports", CM5_MAX_PLUGIN_PORTS},
            {"max_chain_channels", CM5_MAX_CHAIN_CHANNELS}, {"processors", processors}};
}

void ProcessorRegistry::validate_value(const ProcessorParameter& parameter,
                                       const nlohmann::json& value) {
    bool valid = false;
    if (parameter.type == "number" || parameter.type == "integer") {
        if (value.is_number()) {
            const double number = value.get<double>();
            valid = std::isfinite(number) && (!parameter.min || number >= *parameter.min) &&
                    (!parameter.max || number <= *parameter.max) &&
                    (parameter.type != "integer" || std::trunc(number) == number);
        }
    } else if (parameter.type == "boolean") {
        valid = value.is_boolean();
    } else if (parameter.type == "string") {
        valid = value.is_string();
    } else if (parameter.type == "enum") {
        for (const auto& choice : parameter.choices) {
            if (choice.value == value) valid = true;
        }
    }
    if (!valid)
        throw std::invalid_argument("Invalid value for parameter '" + parameter.id + "' (" + parameter.type + ")");
}
