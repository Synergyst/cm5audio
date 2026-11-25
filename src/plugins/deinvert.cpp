/*
 * deinvert DSP port for CM5Audio.
 *
 * Original project: https://github.com/windytan/deinvert
 * Original author: Oona Räisänen (OH2EIQ)
 * Original copyright: Copyright (c) 2017 Oona Räisänen
 *
 * This implementation adapts the simple and split-band frequency-inversion
 * algorithms to CM5Audio's native AudioProcessor interface. The Kaiser-window
 * FIR design follows the algorithm used by liquid-dsp, but is implemented here
 * directly so CM5Audio has no liquid-dsp dependency.
 *
 * MIT License
 *
 * Copyright (c) 2017 Oona Räisänen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "plugins/deinvert.h"
#include "processor_registry.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

float bessel_i0(float value) noexcept {
    // Kaiser-window arguments used here are small enough for this
    // convergent series to be accurate during construction.
    const double x = static_cast<double>(value) * 0.5;
    const double xSquared = x * x;

    double term = 1.0;
    double sum = 1.0;

    for (unsigned k = 1; k <= 80; ++k) {
        const double divisor =
            static_cast<double>(k) * static_cast<double>(k);

        term *= xSquared / divisor;
        sum += term;

        if (term <= sum * 1.0e-15)
            break;
    }

    return static_cast<float>(sum);
}

float kaiser_beta(float attenuationDb) noexcept {
    if (attenuationDb > 50.0f)
        return 0.1102f * (attenuationDb - 8.7f);

    if (attenuationDb > 21.0f) {
        return 0.5842f * std::pow(attenuationDb - 21.0f, 0.4f) +
               0.07886f * (attenuationDb - 21.0f);
    }

    return 0.0f;
}

float sinc(float value) noexcept {
    if (std::fabs(value) < 1.0e-8f)
        return 1.0f;

    const float x = kPi * value;
    return std::sin(x) / x;
}

size_t filter_length_for_quality(unsigned quality) noexcept {
    // Original deinvert filter durations at 48 kHz.
    static constexpr float lengthsSeconds[] = {
        0.0f,
        0.0006f,
        0.0024f,
        0.0064f
    };

    if (quality == 0)
        return 1;

    const auto halfLength = static_cast<size_t>(
        std::lround(48000.0f * lengthsSeconds[quality]));

    return std::min<size_t>(2 * halfLength + 1, 615u);
}

size_t dc_length_for_quality(unsigned quality) noexcept {
    return std::min<size_t>(
        static_cast<size_t>(quality) * 96u,
        288u);
}

} // namespace

// ============================================================================
// DC REMOVER
// ============================================================================

void DeinvertProcessor::DcRemover::configure(
    size_t windowLength) noexcept {

    length = std::min(windowLength, history.size());

    writePosition = 0;
    sampleCount = 0;
    sum = 0.0f;

    history.fill(0.0f);
}

float DeinvertProcessor::DcRemover::process(float sample) noexcept {
    if (length == 0)
        return sample;

    if (sampleCount == length)
        sum -= history[writePosition];
    else
        ++sampleCount;

    history[writePosition] = sample;
    sum += sample;

    writePosition = (writePosition + 1) % length;

    return sample - sum / static_cast<float>(sampleCount);
}

// ============================================================================
// KAISER-WINDOWED FIR FILTER
// ============================================================================

void DeinvertProcessor::FirFilter::configure(
    float cutoffHz,
    unsigned quality) noexcept {

    history.fill(0.0f);
    coefficients.fill(0.0f);

    writePosition = 0;
    scale = 1.0f;

    if (quality == 0) {
        length = 0;
        return;
    }

    length = filter_length_for_quality(quality);

    if (length == 0 || length > coefficients.size()) {
        length = 0;
        return;
    }

    const float normalizedCutoff = std::clamp(
        cutoffHz / kSampleRate,
        1.0e-5f,
        0.499f);

    const float beta = kaiser_beta(
        quality == 3 ? 80.0f : 60.0f);

    const float windowDenominator = bessel_i0(kPi * beta);

    const float half =
        static_cast<float>(length - 1) * 0.5f;

    for (size_t i = 0; i < length; ++i) {
        const float t = static_cast<float>(i) - half;

        const float normalizedPosition =
            t / (static_cast<float>(length) * 0.5f);

        const float windowArgument = std::sqrt(
            std::max(
                0.0f,
                1.0f - normalizedPosition * normalizedPosition));

        const float window =
            bessel_i0(kPi * beta * windowArgument) /
            windowDenominator;

        coefficients[i] =
            sinc(2.0f * normalizedCutoff * t) * window;
    }

    // Original deinvert applies this passband scaling.
    scale = 2.0f * normalizedCutoff;
}

float DeinvertProcessor::FirFilter::process(float sample) noexcept {
    if (length == 0)
        return sample;

    history[writePosition] = sample;

    float output = 0.0f;
    size_t readPosition = writePosition;

    for (size_t tap = 0; tap < length; ++tap) {
        output += coefficients[tap] * history[readPosition];

        readPosition = (readPosition == 0)
            ? length - 1
            : readPosition - 1;
    }

    writePosition = (writePosition + 1) % length;

    return output * scale;
}

// ============================================================================
// FREQUENCY INVERTER
// ============================================================================

void DeinvertProcessor::Inverter::configure(
    float prefilterHz,
    float shiftHz,
    float postfilterHz,
    unsigned quality) noexcept {

    prefilter.configure(prefilterHz, quality);
    postfilter.configure(postfilterHz, quality);

    phase = 0.0f;
    phaseIncrement = kTwoPi * shiftHz / kSampleRate;

    filteringEnabled = quality > 0;
}

float DeinvertProcessor::Inverter::process(float sample) noexcept {
    // Match deinvert's NCO sequence: advance the carrier, then mix.
    phase += phaseIncrement;

    if (phase >= kTwoPi)
        phase -= kTwoPi;

    if (!filteringEnabled)
        return sample * std::cos(phase);

    const float filteredInput = prefilter.process(sample);
    const float mixed = filteredInput * std::cos(phase);

    return postfilter.process(mixed);
}

// ============================================================================
// PROCESSOR CONSTRUCTION
// ============================================================================

DeinvertProcessor::DeinvertProcessor(
    float carrierFrequency,
    float splitFrequency,
    unsigned filterQuality,
    bool splitBand)
    : m_splitBand(splitBand) {

    filterQuality = std::min(filterQuality, 3u);

    // Keep configurable frequencies inside the valid 48 kHz range.
    const float carrier = std::clamp(
        carrierFrequency,
        500.0f,
        12000.0f);

    const float maximumSplit = std::max(
        100.0f,
        carrier - 100.0f);

    const float split = std::clamp(
        splitFrequency,
        100.0f,
        maximumSplit);

    static constexpr float simpleGain[] = {
        1.0f, 1.4f, 1.8f, 1.8f
    };

    static constexpr float splitGain[] = {
        0.5f, 1.4f, 1.8f, 1.8f
    };

    m_outputGain = splitBand
        ? splitGain[filterQuality]
        : simpleGain[filterQuality];

    for (ChannelState& channel : m_channels) {
        channel.dcRemover.configure(
            dc_length_for_quality(filterQuality));

        if (splitBand) {
            // Lower-band inverter.
            channel.firstInverter.configure(
                split,
                split,
                split,
                filterQuality);

            // Upper-band inverter.
            channel.secondInverter.configure(
                carrier,
                split + carrier,
                carrier,
                filterQuality);
        } else {
            // Ordinary frequency inversion.
            channel.firstInverter.configure(
                carrier,
                carrier,
                carrier,
                filterQuality);
        }
    }
}

// ============================================================================
// AUDIO PROCESSING
// ============================================================================

void DeinvertProcessor::process(
    float* buffer,
    size_t frames,
    unsigned channels) {

    if (!buffer || frames == 0 || channels == 0 ||
        channels > kMaxChannels) {
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t frameOffset =
            frame * static_cast<size_t>(channels);

        for (unsigned channelIndex = 0;
             channelIndex < channels;
             ++channelIndex) {

            ChannelState& channel = m_channels[channelIndex];

            const size_t sampleIndex =
                frameOffset + channelIndex;

            const float dcRemoved =
                channel.dcRemover.process(buffer[sampleIndex]);

            float output =
                channel.firstInverter.process(dcRemoved);

            if (m_splitBand) {
                output +=
                    channel.secondInverter.process(dcRemoved);
            }

            buffer[sampleIndex] = output * m_outputGain;
        }
    }
}

// ============================================================================
// PROCESSOR REGISTRATION
// ============================================================================

void register_deinvert_processor(ProcessorRegistry& registry) {
    ProcessorDescriptor descriptor;

    descriptor.type = "deinvert";
    descriptor.name = "Frequency Inverter";
    descriptor.description =
        "Simple or split-band voice frequency inversion / descrambling.";

    descriptor.parameters = {
        {
            "mode",
            "Mode",
            "enum",
            "simple",
            {},
            {},
            {},
            "",
            {
                {"Simple", "simple"},
                {"Split Band", "split_band"}
            }
        },
        {
            "carrier_frequency",
            "Carrier Frequency",
            "number",
            2632.0,
            500.0,
            12000.0,
            1.0,
            "Hz",
            {}
        },
        {
            "split_frequency",
            "Split Frequency",
            "number",
            1200.0,
            100.0,
            6000.0,
            1.0,
            "Hz",
            {}
        },
        {
            "quality",
            "Filter Quality",
            "integer",
            2,
            0.0,
            3.0,
            1.0,
            "",
            {}
        }
    };

    descriptor.factory = [](const nlohmann::json& params) {
        const std::string mode =
            params.at("mode").get<std::string>();

        return std::make_unique<DeinvertProcessor>(
            params.at("carrier_frequency").get<float>(),
            params.at("split_frequency").get<float>(),
            params.at("quality").get<unsigned>(),
            mode == "split_band");
    };

    descriptor.audio_input = {1, 8, 2};
    descriptor.audio_output = {1, 8, 2};

    // One instance handles the interleaved bus, maintaining
    // independent filter and oscillator state for each channel.
    descriptor.chain_channel_mode =
        ProcessorChainChannelMode::MULTICHANNEL;

    registry.register_processor(std::move(descriptor));
}
