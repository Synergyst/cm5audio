#include "processor_registry.h"
#include "constants.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace {

std::string builtinParameterDescription(const std::string& type, const std::string& id)
{
    if (type == "autotune") {
        if (id == "speed") return "Controls how quickly detected notes move toward the selected scale tone. Higher values correct more rapidly; lower values glide more gradually.";
        if (id == "sensitivity") return "Controls how readily the pitch detector accepts a detected fundamental. Higher values accept less-perfect periodicity.";
        if (id == "key") return "Selects the tonic used when Major or Minor scale snapping is enabled.";
        if (id == "scale") return "Selects chromatic, major, or natural-minor note snapping.";
    } else if (type == "bitcrusher") {
        if (id == "bits") return "Sets the quantizer bit depth. Lower values create coarser amplitude steps and stronger digital distortion.";
        if (id == "sample_rate") return "Sets the effective sample-and-hold rate. Lower values reduce temporal resolution and add aliasing.";
    } else if (type == "chorus") {
        if (id == "rate_hz") return "Sets the speed of the low-frequency oscillator that modulates the delay.";
        if (id == "depth") return "Sets the amount of delay modulation. Higher values create a wider, more pronounced pitch shimmer.";
        if (id == "mix") return "Blends the delayed chorus signal with the original input.";
    } else if (type == "compressor") {
        if (id == "threshold") return "Sets the linear-amplitude level where compression begins. This is a 0–1 signal amplitude, not a dBFS value.";
        if (id == "ratio") return "Sets how strongly levels above the threshold are reduced. For example, 4:1 allows a 1 dB output rise for every 4 dB input rise above threshold.";
        if (id == "attack") return "Sets how quickly gain reduction responds when the signal exceeds the threshold.";
        if (id == "release") return "Sets how quickly gain returns toward unity after the signal falls below the threshold.";
    } else if (type == "deesser") {
        if (id == "threshold") return "Sets the detector threshold for high-frequency sibilance.";
        if (id == "frequency") return "Sets the detector's high-pass corner frequency, focusing gain reduction on the sibilance region.";
        if (id == "reduction") return "Sets the minimum gain applied when sibilance is detected. Lower values produce stronger attenuation.";
        if (id == "speed") return "Sets the envelope and gain-smoothing response time.";
    } else if (type == "echo") {
        if (id == "delay_ms") return "Sets the time between the original signal and each echo.";
        if (id == "feedback") return "Sets how much delayed output is fed back to create subsequent repeats. Higher values produce longer echo trails.";
        if (id == "mix") return "Blends the delayed signal with the original input.";
    } else if (type == "eq24") {
        if (id.size() > 2 && id[0] == 'b') {
            const auto separator = id.find('_');
            if (separator != std::string::npos && separator + 1 < id.size()) {
                const std::string control = id.substr(separator + 1);
                if (control == "freq") return "Sets the centre frequency of this parametric EQ band.";
                if (control == "gain") return "Boosts or cuts this band's centre frequency. 0 dB leaves the band unchanged.";
                if (control == "q") return "Sets this band's Q (bandwidth). Higher values make the boost or cut narrower.";
            }
        }
    } else if (type == "expander") {
        if (id == "threshold") return "Sets the linear-amplitude level below which downward expansion is applied.";
        if (id == "ratio") return "Controls how strongly the level is reduced as it falls below the threshold.";
        if (id == "attack") return "Sets how quickly the gain reduction responds to a falling signal.";
        if (id == "release") return "Sets how quickly gain recovers as the signal rises.";
        if (id == "range") return "Sets the minimum allowed gain, limiting the maximum amount of attenuation.";
    } else if (type == "gain") {
        if (id == "gain") return "Sets the linear amplitude multiplier. 1 is unity, 0 is silence, and values above 1 amplify.";
    } else if (type == "hpf") {
        if (id == "cutoff") return "Sets the high-pass cutoff frequency. Frequencies below this point are attenuated.";
    } else if (type == "limiter") {
        if (id == "threshold") return "Sets the maximum linear signal level targeted by the limiter.";
        if (id == "attack") return "Sets how quickly the limiter gain responds to peaks.";
        if (id == "release") return "Sets how quickly gain recovers after peaks subside.";
        if (id == "lookahead") return "Sets how far ahead the limiter analyses the signal before applying gain reduction.";
    } else if (type == "lpf") {
        if (id == "cutoff") return "Sets the low-pass cutoff frequency. Frequencies above this point are attenuated.";
    } else if (type == "lush_reverb") {
        if (id == "decay") return "Controls the length of the reverb tail. Higher values produce longer, denser decay.";
        if (id == "diffusion") return "Controls the amount of modulation and diffusion in the reverb network.";
        if (id == "mix") return "Blends the reverberated signal with the original input.";
    } else if (type == "midi_harmonizer") {
        if (id == "midi_channel") return "Selects the MIDI channel to receive. Channel 1–16 listens to that channel only.";
        if (id == "reference_note") return "Sets the MIDI note used as the pitch reference for the incoming audio.";
        if (id == "max_voices") return "Sets the maximum number of simultaneous harmony voices.";
        if (id == "pitch_range") return "Limits the allowed pitch distance of generated voices from the reference note.";
        if (id == "mix") return "Sets the level of generated harmony voices relative to the dry input.";
        if (id == "auto_connect") return "Enables or disables automatic connection to the selected PipeWire MIDI source.";
        if (id == "midi_output") return "Selects the external PipeWire MIDI source used to control the harmonizer. Manual mode expects graph/QPWGraph routing.";
    } else if (type == "midi_pitch_shifter") {
        if (id == "listen_channel") return "Selects the MIDI channel whose pitch-bend messages control this processor.";
        if (id == "pitch_range") return "Sets the maximum upward or downward pitch shift at the full MIDI pitch-bend range.";
        if (id == "auto_connect") return "Enables or disables automatic connection to the selected PipeWire MIDI source.";
        if (id == "midi_output") return "Selects the external PipeWire MIDI source used for pitch bend. Manual mode expects graph/QPWGraph routing.";
    } else if (type == "midi_sample_player") {
        if (id == "sample_asset") return "Names the uploaded audio asset to play. Upload the file in Audio Assets first; a filename, display name, or SHA-256 identifier may be used.";
        if (id == "midi_channel") return "Selects the incoming MIDI channel. 0 accepts all channels; 1–16 restricts playback to one channel.";
        if (id == "root_note") return "Sets the MIDI note that plays the sample at its original pitch. Other notes transpose relative to this note.";
        if (id == "max_voices") return "Sets the maximum number of overlapping sample voices.";
        if (id == "gain") return "Sets the linear output amplitude multiplier for sample playback.";
        if (id == "loop") return "When enabled, each triggered sample repeats until its MIDI note is released.";
        if (id == "full_length_sustain") return "When enabled, note-off lets the sample finish rather than stopping playback immediately.";
        if (id == "fade_on_release") return "In full-length mode, fades the released voice toward silence while it continues playing to the sample end.";
        if (id == "attack_ms") return "Sets the fade-in duration at the start of each triggered sample.";
        if (id == "release_ms") return "Sets the fade-out duration for note release or the configured release fade.";
        if (id == "auto_connect") return "Enables or disables automatic connection to the selected PipeWire MIDI source.";
        if (id == "midi_output") return "Selects the external PipeWire MIDI source. Manual mode expects graph/QPWGraph routing.";
    } else if (type == "noise_gate") {
        if (id == "threshold") return "Sets the linear-amplitude level below which the gate closes.";
        if (id == "attack") return "Sets how quickly the gate opens when the input rises above threshold.";
        if (id == "release") return "Sets how quickly the gate closes after the signal falls below threshold.";
        if (id == "hold") return "Sets how long the gate remains open after the detector falls below threshold.";
    } else if (type == "normalize") {
        if (id == "target") return "Sets the desired linear signal level that the automatic gain controller tries to maintain.";
        if (id == "speed") return "Sets how quickly the automatic gain controller adapts to changes in input level.";
        if (id == "max_gain") return "Limits the maximum amplification the automatic gain controller may apply.";
    } else if (type == "rnnoise") {
        if (id == "mix") return "Blends the RNNoise output with the dry input. 0 bypasses the denoised signal; 1 uses it fully.";
        if (id == "input_gain_db") return "Applies gain before RNNoise analysis. Useful when the incoming signal is very quiet.";
        if (id == "output_gain_db") return "Applies gain after denoising.";
        if (id == "vad_mode") return "Selects whether voice activity detection is disabled, attenuates non-speech, or mutes non-speech.";
        if (id == "vad_threshold") return "Sets the voice-activity probability required for the gate to treat a frame as speech.";
        if (id == "vad_attenuation_db") return "Sets the attenuation applied to frames classified as non-speech when VAD mode is Attenuate.";
        if (id == "vad_attack_ms") return "Sets how quickly the VAD gate opens when speech is detected.";
        if (id == "vad_release_ms") return "Sets how quickly the VAD gate closes when speech is no longer detected.";
        if (id == "model_mode") return "Chooses the built-in RNNoise model or a custom model loaded from a file.";
        if (id == "model_path") return "Filesystem path to the custom RNNoise model. Used only when Custom Model is selected.";
    } else if (type == "soft_clipper") {
        if (id == "drive") return "Sets pre-clipping amplification. Higher drive adds stronger saturation and harmonics.";
        if (id == "ceiling") return "Sets the output ceiling for the soft-clipping curve.";
    } else if (type == "reverb") {
        if (id == "room_size") return "Controls the effective delay lengths and perceived size of the simulated room.";
        if (id == "damping") return "Controls high-frequency damping in the reverb feedback network. Higher values produce darker tails.";
        if (id == "mix") return "Blends the reverberated signal with the original input.";
    } else if (type == "vowel_filter") {
        if (id == "vowel") return "Selects the target vowel by choosing its fixed F1, F2, and F3 formant frequencies.";
        if (id == "resonance") return "Sets the Q factor of the three formant bands. Higher values make the resonances narrower.";
        if (id == "mix") return "Blends the original signal with the vowel-shaped signal.";
    }
    return {};
}

} // namespace

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
    for (auto& parameter : descriptor.parameters) {
        if (parameter.description.empty())
            parameter.description = builtinParameterDescription(descriptor.type, parameter.id);
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
        throw std::invalid_argument("Unknown plugin type: " + type);
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
            if (!parameter.description.empty()) value["description"] = parameter.description;
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
