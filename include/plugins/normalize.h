#ifndef NORMALIZE_PROCESSOR_H
#define NORMALIZE_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>

class NormalizeProcessor : public AudioProcessor {
public:
    NormalizeProcessor(float target, float speed, float max_gain);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_target;
    std::atomic<float> m_speed;
    std::atomic<float> m_max_gain;

    float m_envelope = 0.0f;
    float m_current_gain = 1.0f;
};

#endif
