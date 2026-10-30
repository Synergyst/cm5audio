#include "audio_processor.h"
#include "processor_registry.h"

#include <rnnoise.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

constexpr size_t FRAME_SIZE = 480;
constexpr unsigned MAX_CHANNELS = 8;

constexpr float PCM_SCALE = 32768.0f;
constexpr float PCM_MIN = -32768.0f;
constexpr float PCM_MAX = 32767.0f;

enum class VadMode {
    Disabled,
    Attenuate,
    Mute
};

enum class ModelMode {
    Builtin,
    Custom
};

static float dbToLinear(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

static float timeCoefficient(float milliseconds) noexcept
{
    if (milliseconds <= 0.0f)
        return 1.0f;

    const float seconds = milliseconds / 1000.0f;

    return 1.0f - std::exp(
        -1.0f / (seconds * 48000.0f));
}

class RNNoiseProcessor final : public AudioProcessor {
public:
    RNNoiseProcessor(float mix,
                     float inputGainDb,
                     float outputGainDb,
                     VadMode vadMode,
                     float vadThreshold,
                     float vadAttenuationDb,
                     float vadAttackMs,
                     float vadReleaseMs,
                     ModelMode modelMode,
                     const std::string& modelPath)
        : mix_(mix),
          inputGain_(dbToLinear(inputGainDb)),
          outputGain_(dbToLinear(outputGainDb)),
          vadMode_(vadMode),
          vadThreshold_(vadThreshold),
          vadAttenuation_(dbToLinear(vadAttenuationDb)),
          vadAttackCoefficient_(timeCoefficient(vadAttackMs)),
          vadReleaseCoefficient_(timeCoefficient(vadReleaseMs))
    {
        if (modelMode == ModelMode::Custom) {
            if (modelPath.empty()) {
                throw std::runtime_error(
                    "RNNoise: Custom Model selected, "
                    "but Custom Model Path is empty");
            }

            model_ = rnnoise_model_from_filename(
                modelPath.c_str());

            if (!model_) {
                throw std::runtime_error(
                    "RNNoise: unable to load custom model: " +
                    modelPath);
            }
        }

        for (unsigned channel = 0;
             channel < MAX_CHANNELS;
             ++channel) {

            states_[channel] = rnnoise_create(model_);

            if (!states_[channel]) {
                destroyStates();

                if (model_) {
                    rnnoise_model_free(model_);
                    model_ = nullptr;
                }

                throw std::runtime_error(
                    "RNNoise: failed to create denoiser state");
            }

            gateGain_[channel] = 1.0f;
            previousVad_[channel] = 0.0f;
        }
    }

    ~RNNoiseProcessor() override
    {
        destroyStates();

        // A model must outlive every DenoiseState using it.
        if (model_) {
            rnnoise_model_free(model_);
            model_ = nullptr;
        }
    }

    RNNoiseProcessor(const RNNoiseProcessor&) = delete;
    RNNoiseProcessor& operator=(const RNNoiseProcessor&) = delete;

    void process(float* buffer,
                 size_t frames,
                 unsigned channels) override
    {
        if (!buffer || frames == 0 || channels == 0)
            return;

        // True zero-latency bypass when no processing is requested.
        if (mix_ <= 0.0f &&
            vadMode_ == VadMode::Disabled &&
            outputGain_ == 1.0f) {
            return;
        }

        const unsigned processChannels =
            std::min(channels, MAX_CHANNELS);

        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t position = framePosition_;

            for (unsigned channel = 0;
                 channel < processChannels;
                 ++channel) {

                const size_t index =
                    frame * channels + channel;

                // Preserve the original sample before replacing
                // the route buffer with delayed processed audio.
                const float input = buffer[index];

                inputFrame_[channel][position] = input;

                // RNNoise is frame-based. Output the completed
                // frame from the previous processing interval.
                buffer[index] =
                    outputFrame_[channel][position];
            }

            // Channels beyond MAX_CHANNELS remain passthrough.

            ++framePosition_;

            if (framePosition_ >= FRAME_SIZE) {
                processFrame(processChannels);
                framePosition_ = 0;
            }
        }
    }

private:
    float mix_ = 1.0f;

    float inputGain_ = 1.0f;
    float outputGain_ = 1.0f;

    VadMode vadMode_ = VadMode::Disabled;

    float vadThreshold_ = 0.5f;
    float vadAttenuation_ = 0.0158489f;

    float vadAttackCoefficient_ = 1.0f;
    float vadReleaseCoefficient_ = 0.000173f;

    RNNModel* model_ = nullptr;

    std::array<DenoiseState*, MAX_CHANNELS> states_{};

    // Original normalized route input.
    std::array<std::array<float, FRAME_SIZE>, MAX_CHANNELS>
        inputFrame_{};

    // Input scaled for RNNoise's signed-16-bit amplitude convention.
    std::array<std::array<float, FRAME_SIZE>, MAX_CHANNELS>
        rnnoiseInput_{};

    // RNNoise output, still in signed-16-bit amplitude scale.
    std::array<std::array<float, FRAME_SIZE>, MAX_CHANNELS>
        rnnoiseOutput_{};

    // RNNoise synthesis has a one-frame delay. Keep the matching
    // dry signal and VAD probability for the output it produces.
    std::array<std::array<float, FRAME_SIZE>, MAX_CHANNELS>
        previousDryFrame_{};

    std::array<std::array<float, FRAME_SIZE>, MAX_CHANNELS>
        outputFrame_{};

    std::array<float, MAX_CHANNELS> previousVad_{};
    std::array<float, MAX_CHANNELS> gateGain_{};

    size_t framePosition_ = 0;

    void destroyStates() noexcept
    {
        for (auto*& state : states_) {
            if (state) {
                rnnoise_destroy(state);
                state = nullptr;
            }
        }
    }

    float getVadTarget(float probability) const noexcept
    {
        switch (vadMode_) {
        case VadMode::Disabled:
            return 1.0f;

        case VadMode::Attenuate:
            return probability >= vadThreshold_
                ? 1.0f
                : vadAttenuation_;

        case VadMode::Mute:
            return probability >= vadThreshold_
                ? 1.0f
                : 0.0f;
        }

        return 1.0f;
    }

    void processFrame(unsigned channels)
    {
        for (unsigned channel = 0;
             channel < channels;
             ++channel) {

            // Convert normalized audio to RNNoise's PCM16-scale
            // floating-point input convention.
            for (size_t i = 0; i < FRAME_SIZE; ++i) {
                const float scaled =
                    inputFrame_[channel][i] *
                    inputGain_ *
                    PCM_SCALE;

                rnnoiseInput_[channel][i] = std::clamp(
                    scaled,
                    PCM_MIN,
                    PCM_MAX);
            }

            // RNNoise returns the VAD probability for this input.
            // Its output synthesis is one frame behind the input.
            const float currentVad = rnnoise_process_frame(
                states_[channel],
                rnnoiseOutput_[channel].data(),
                rnnoiseInput_[channel].data());

            // Match the returned audio to the preceding input
            // frame's dry audio and VAD probability.
            const float targetGate =
                getVadTarget(previousVad_[channel]);

            float gate = gateGain_[channel];

            const float coefficient =
                targetGate > gate
                    ? vadAttackCoefficient_
                    : vadReleaseCoefficient_;

            for (size_t i = 0; i < FRAME_SIZE; ++i) {
                const float dry =
                    previousDryFrame_[channel][i];

                // Convert output back to normalized float audio.
                // Compensate for the input gain so that changing
                // input gain does not itself change output level.
                const float wet =
                    rnnoiseOutput_[channel][i] /
                    (PCM_SCALE * inputGain_);

                const float mixed =
                    dry + (wet - dry) * mix_;

                // Smooth VAD gating at sample rate.
                gate += (targetGate - gate) * coefficient;

                outputFrame_[channel][i] =
                    mixed * gate * outputGain_;
            }

            // Preserve the input frame for RNNoise's next output.
            previousDryFrame_[channel] =
                inputFrame_[channel];

            previousVad_[channel] = std::clamp(
                currentVad,
                0.0f,
                1.0f);

            gateGain_[channel] = gate;
        }
    }
};

} // namespace

void register_rnnoise_processor(ProcessorRegistry& registry)
{
    registry.register_processor({
        "rnnoise",
        "RNNoise",
        "RNNoise speech noise suppression with optional VAD gating "
        "and selectable built-in or custom models.",

        {
            {
                "mix",
                "Wet Mix",
                "number",
                1.0,
                0.0,
                1.0,
                0.01,
                "",
                {}
            },
            {
                "input_gain_db",
                "Input Gain",
                "number",
                0.0,
                -24.0,
                24.0,
                0.5,
                "dB",
                {}
            },
            {
                "output_gain_db",
                "Output Gain",
                "number",
                0.0,
                -24.0,
                24.0,
                0.5,
                "dB",
                {}
            },
            {
                "vad_mode",
                "VAD Gate Mode",
                "enum",
                "disabled",
                {},
                {},
                {},
                "",
                {
                    {"Disabled", "disabled"},
                    {"Attenuate", "attenuate"},
                    {"Mute", "mute"}
                }
            },
            {
                "vad_threshold",
                "VAD Threshold",
                "number",
                0.5,
                0.0,
                1.0,
                0.01,
                "",
                {}
            },
            {
                "vad_attenuation_db",
                "VAD Attenuation",
                "number",
                -36.0,
                -80.0,
                0.0,
                1.0,
                "dB",
                {}
            },
            {
                "vad_attack_ms",
                "VAD Attack",
                "number",
                5.0,
                0.0,
                500.0,
                1.0,
                "ms",
                {}
            },
            {
                "vad_release_ms",
                "VAD Release",
                "number",
                120.0,
                0.0,
                2000.0,
                1.0,
                "ms",
                {}
            },
            {
                "model_mode",
                "RNNoise Model",
                "enum",
                "builtin",
                {},
                {},
                {},
                "",
                {
                    {"Built-in Model", "builtin"},
                    {"Custom Model", "custom"}
                }
            },
            {
                "model_path",
                "Custom Model Path",
                "string",
                "",
                {},
                {},
                {},
                "",
                {}
            }
        },

        [](const nlohmann::json& params) {
            const float mix =
                params.at("mix").get<float>();

            const float inputGainDb =
                params.at("input_gain_db").get<float>();

            const float outputGainDb =
                params.at("output_gain_db").get<float>();

            const std::string vadModeString =
                params.at("vad_mode").get<std::string>();

            const float vadThreshold =
                params.at("vad_threshold").get<float>();

            const float vadAttenuationDb =
                params.at("vad_attenuation_db").get<float>();

            const float vadAttackMs =
                params.at("vad_attack_ms").get<float>();

            const float vadReleaseMs =
                params.at("vad_release_ms").get<float>();

            const std::string modelModeString =
                params.at("model_mode").get<std::string>();

            const std::string modelPath =
                params.at("model_path").get<std::string>();

            VadMode vadMode = VadMode::Disabled;

            if (vadModeString == "attenuate") {
                vadMode = VadMode::Attenuate;
            }
            else if (vadModeString == "mute") {
                vadMode = VadMode::Mute;
            }

            ModelMode modelMode = ModelMode::Builtin;

            if (modelModeString == "custom") {
                modelMode = ModelMode::Custom;
            }

            return std::make_unique<RNNoiseProcessor>(
                mix,
                inputGainDb,
                outputGainDb,
                vadMode,
                vadThreshold,
                vadAttenuationDb,
                vadAttackMs,
                vadReleaseMs,
                modelMode,
                modelPath);
        }
    });
}
