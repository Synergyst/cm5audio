#ifndef NOISE_GATE_PROCESSOR_H
#define NOISE_GATE_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>

class NoiseGateProcessor : public AudioProcessor {
public:
    NoiseGateProcessor(float threshold, float attack, float release, float hold);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_attack;
    std::atomic<float> m_release;
    std::atomic<float> m_hold;

    float m_current_gain = 1.0f;
    size_t m_hold_frames = 0;
};

#endif
