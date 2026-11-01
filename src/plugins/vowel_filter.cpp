#include "plugins/vowel_filter.h"
#include "processor_registry.h"
#include "constants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDefaultSampleRate = 48000.0f;

// Approximate fixed vocal formant centres (Hz), not a speech-dependent tracker.
struct VowelFormants {
    float f1;
    float f2;
    float f3;
};

constexpr std::array<VowelFormants, 5> kVowelFormants{{
    {700.0f, 1220.0f, 2600.0f}, // A
    {500.0f, 1780.0f, 2550.0f}, // E
    {300.0f, 2250.0f, 3000.0f}, // I
    {400.0f,  800.0f, 2600.0f}, // O
    {300.0f,  620.0f, 2400.0f}  // U
}};

} // namespace

void VowelFilterProcessor::Biquad::setBandPass(
    float frequency, float q, float sampleRate) noexcept
{
    if (!(sampleRate > 0.0f) || !std::isfinite(sampleRate))
        sampleRate = kDefaultSampleRate;

    const float maximumFrequency = std::max(20.0f, sampleRate * 0.45f);
    frequency = std::clamp(frequency, 20.0f, maximumFrequency);
    q = std::clamp(q, 1.0f, 50.0f);

    const float omega = 2.0f * kPi * frequency / sampleRate;
    const float sine = std::sin(omega);
    const float cosine = std::cos(omega);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    // RBJ constant-0dB-peak band-pass: Q changes bandwidth, not peak gain.
    b0 = alpha / a0;
    b1 = 0.0f;
    b2 = -alpha / a0;
    a1 = (-2.0f * cosine) / a0;
    a2 = (1.0f - alpha) / a0;
}

float VowelFilterProcessor::Biquad::process(float input) noexcept
{
    const float output = b0 * input + z1;
    z1 = b1 * input - a1 * output + z2;
    z2 = b2 * input - a2 * output;
    return output;
}

VowelFilterProcessor::VowelFilterProcessor(
    int vowelIndex, float resonance, float mix)
    : m_vowel_idx(std::clamp(vowelIndex, 0, 4)),
      m_resonance(std::clamp(resonance, 1.0f, 50.0f)),
      m_mix(std::clamp(mix, 0.0f, 1.0f))
{
}

void VowelFilterProcessor::updateCoefficients(float sampleRate) noexcept
{
    if (!(sampleRate > 0.0f) || !std::isfinite(sampleRate))
        sampleRate = kDefaultSampleRate;

    const int vowel = std::clamp(m_vowel_idx.load(std::memory_order_relaxed), 0, 4);
    const float resonance = std::clamp(
        m_resonance.load(std::memory_order_relaxed), 1.0f, 50.0f);

    if (vowel == m_last_vowel_idx &&
        resonance == m_last_resonance &&
        sampleRate == m_last_sample_rate)
        return;

    const VowelFormants& formants = kVowelFormants[static_cast<size_t>(vowel)];
    for (auto& channel : m_channels) {
        channel.formants[0].setBandPass(formants.f1, resonance, sampleRate);
        channel.formants[1].setBandPass(formants.f2, resonance, sampleRate);
        channel.formants[2].setBandPass(formants.f3, resonance, sampleRate);
    }

    m_last_vowel_idx = vowel;
    m_last_resonance = resonance;
    m_last_sample_rate = sampleRate;
}

void VowelFilterProcessor::processBuffer(
    float* buffer, size_t frames, unsigned channels, float sampleRate) noexcept
{
    if (!buffer || frames == 0 || channels == 0 ||
        channels > CM5_MAX_CHAIN_CHANNELS)
        return;

    if (!(sampleRate > 0.0f) || !std::isfinite(sampleRate))
        sampleRate = kDefaultSampleRate;

    updateCoefficients(sampleRate);

    const float mix = std::clamp(
        m_mix.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float dry = 1.0f - mix;

    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t frameOffset = frame * static_cast<size_t>(channels);
        for (unsigned channel = 0; channel < channels; ++channel) {
            const size_t index = frameOffset + channel;
            const float input = buffer[index];
            auto& filters = m_channels[channel].formants;
            const float filtered =
                filters[0].process(input) * 0.45f +
                filters[1].process(input) * 0.35f +
                filters[2].process(input) * 0.20f;
            buffer[index] = input * dry + filtered * mix;
        }
    }
}

void VowelFilterProcessor::process(
    float* buffer, size_t frames, unsigned channels)
{
    processBuffer(buffer, frames, channels, kDefaultSampleRate);
}

void VowelFilterProcessor::process(AudioProcessContext& context)
{
    if (!context.audio_outputs || context.audio_output_count == 0) {
        if (context.output && context.output_channels)
            processBuffer(context.output, context.frames, context.output_channels,
                          static_cast<float>(context.sample_rate));
        return;
    }

    AudioBusBuffer& output = context.audio_outputs[0];
    if (!output.output || output.channels == 0 || context.frames == 0)
        return;

    const AudioBusBuffer* input =
        context.audio_inputs && context.audio_input_count
            ? &context.audio_inputs[0] : nullptr;

    if (!input || !input->input || input->channels == 0) {
        std::fill_n(output.output,
                    context.frames * static_cast<size_t>(output.channels), 0.0f);
    } else if (input->input != output.output || input->channels != output.channels) {
        const unsigned copiedChannels = std::min(input->channels, output.channels);
        for (size_t frame = 0; frame < context.frames; ++frame) {
            const float* source = input->input + frame * static_cast<size_t>(input->channels);
            float* destination = output.output + frame * static_cast<size_t>(output.channels);
            for (unsigned channel = 0; channel < copiedChannels; ++channel)
                destination[channel] = source[channel];
            for (unsigned channel = copiedChannels; channel < output.channels; ++channel)
                destination[channel] = 0.0f;
        }
    }

    processBuffer(output.output, context.frames, output.channels,
                  static_cast<float>(context.sample_rate));
}

void register_vowel_filter_processor(ProcessorRegistry& registry)
{
    ProcessorDescriptor descriptor{
        "vowel_filter",
        "Vowel Filter",
        "Three-formant resonant filter emphasizing the spectral regions associated with A, E, I, O, or U. This is a static formant-colouring effect, not a physical talkbox or vocoder.",
        {
            {"vowel", "Vowel", "enum", "a", {}, {}, {}, "",
             {{"A", "a"}, {"E", "e"}, {"I", "i"}, {"O", "o"}, {"U", "u"}},
             "Selects a fixed set of first-, second-, and third-formant centre frequencies."},
            {"resonance", "Resonance", "number", 10.0, 1.0, 50.0, 0.1, "Q", {},
             "Sets the Q factor of all three formant bands. Higher Q produces narrower, more selective resonances; lower Q produces broader bands."},
            {"mix", "Mix", "number", 0.8, 0.0, 1.0, 0.01, "linear", {},
             "Blends the original signal with the formant-filtered signal. 0 is fully dry; 1 is fully wet."}
        },
        [](const nlohmann::json& params) {
            const std::string vowel = params.at("vowel").get<std::string>();
            int index = 0;
            if (vowel == "e") index = 1;
            else if (vowel == "i") index = 2;
            else if (vowel == "o") index = 3;
            else if (vowel == "u") index = 4;

            return std::make_unique<VowelFilterProcessor>(
                index,
                params.at("resonance").get<float>(),
                params.at("mix").get<float>());
        }
    };

    descriptor.audio_input = {1, 8, 2};
    descriptor.audio_output = {1, 8, 2};
    descriptor.chain_channel_mode = ProcessorChainChannelMode::MULTICHANNEL;
    registry.register_processor(std::move(descriptor));
}
