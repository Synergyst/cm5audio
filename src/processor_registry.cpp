#include "processor_registry.h"
#include "constants.h"
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
    std::set<std::string> ids;
    for (const auto& parameter : descriptor.parameters) {
        if (parameter.id.empty() || !ids.insert(parameter.id).second)
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
    if (!validLayout(descriptor.audio_input) || !validLayout(descriptor.audio_output))
        throw std::invalid_argument("Invalid audio channel layout: " + descriptor.type);

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
        nlohmann::json audioInputs = nlohmann::json::array();
        audioInputs.push_back({
            {"id", "audio_in"}, {"name", "Audio Input"},
            {"min_channels", descriptor.audio_input.minimum},
            {"max_channels", descriptor.audio_input.maximum},
            {"default_channels", descriptor.audio_input.default_channels}
        });
        nlohmann::json audioOutputs = nlohmann::json::array();
        audioOutputs.push_back({
            {"id", "audio_out"}, {"name", "Audio Output"},
            {"min_channels", descriptor.audio_output.minimum},
            {"max_channels", descriptor.audio_output.maximum},
            {"default_channels", descriptor.audio_output.default_channels}
        });
        processors.push_back({{"type", descriptor.type}, {"name", descriptor.name},
                              {"description", descriptor.description}, {"defaults", defaults},
                              {"parameters", parameters},
                              {"audio", {{"inputs", std::move(audioInputs)},
                                         {"outputs", std::move(audioOutputs)}}}});
    }
    return {{"schema_version", 1}, {"max_chain_length", CM5_MAX_PROCESSORS_PER_ROUTE},
            {"processors", processors}};
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
