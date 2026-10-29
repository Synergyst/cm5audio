#ifndef VOWEL_FILTER_PROCESSOR_H
#define VOWEL_FILTER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <string>

class VowelFilterProcessor : public AudioProcessor {
public:
    VowelFilterProcessor(int vowel_idx, float resonance, float mix);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;

        void setBandPass(float freq, float Q, float sampleRate);
        float process(float in);
    };

    std::atomic<int> m_vowel_idx;
    std::atomic<float> m_resonance;
    std::atomic<float> m_mix;

    Biquad m_f1;
    Biquad m_f2;
    int m_last_vowel_idx = -1;
    void updateCoefficients();
};

#endif
