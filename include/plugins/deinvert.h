#ifndef CM5AUDIO_PLUGIN_DEINVERT_H
#define CM5AUDIO_PLUGIN_DEINVERT_H

/*
 * deinvert DSP port for CM5Audio.
 *
 * Original project: https://github.com/windytan/deinvert
 * Original author: Oona Räisänen (OH2EIQ)
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

#include "audio_processor.h"

#include <array>
#include <cstddef>

class ProcessorRegistry;

class DeinvertProcessor final : public AudioProcessor {
public:
    DeinvertProcessor(float carrierFrequency = 2632.0f,
                      float splitFrequency = 1200.0f,
                      unsigned filterQuality = 2,
                      bool splitBand = false);

    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    static constexpr float kSampleRate = 48000.0f;
    static constexpr unsigned kMaxChannels = 8;
    static constexpr size_t kMaxFilterLength = 615;
    static constexpr size_t kMaxDcLength = 288;

    struct DcRemover {
        std::array<float, kMaxDcLength> history{};
        size_t length = 0;
        size_t writePosition = 0;
        size_t sampleCount = 0;
        float sum = 0.0f;

        void configure(size_t windowLength) noexcept;
        float process(float sample) noexcept;
    };

    struct FirFilter {
        std::array<float, kMaxFilterLength> coefficients{};
        std::array<float, kMaxFilterLength> history{};
        size_t length = 0;
        size_t writePosition = 0;
        float scale = 1.0f;

        void configure(float cutoffHz, unsigned quality) noexcept;
        float process(float sample) noexcept;
    };

    struct Inverter {
        FirFilter prefilter;
        FirFilter postfilter;

        float phase = 0.0f;
        float phaseIncrement = 0.0f;

        bool filteringEnabled = false;

        void configure(float prefilterHz,
                       float shiftHz,
                       float postfilterHz,
                       unsigned quality) noexcept;

        float process(float sample) noexcept;
    };

    struct ChannelState {
        DcRemover dcRemover;
        Inverter firstInverter;
        Inverter secondInverter;
    };

    std::array<ChannelState, kMaxChannels> m_channels{};

    float m_outputGain = 1.0f;
    bool m_splitBand = false;
};

void register_deinvert_processor(ProcessorRegistry& registry);

#endif
