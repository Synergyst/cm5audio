#ifndef DEESSER_PROCESSOR_H
#define DEESSER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>

class DeEsserProcessor : public AudioProcessor {
public:
    DeEsserProcessor(float threshold, float frequency, float reduction, float speed);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_frequency;
    std::atomic<float> m_reduction;
    std::atomic<float> m_speed;

    float m_lpf_state = 0.0f;
    float m_envelope = 0.0f;
    float m_current_gain = 1.0f;
};

#endif
