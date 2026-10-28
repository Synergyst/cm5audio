#ifndef EXPANDER_PROCESSOR_H
#define EXPANDER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>

class ExpanderProcessor : public AudioProcessor {
public:
    ExpanderProcessor(float threshold, float ratio, float attack, float release, float range);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_ratio;
    std::atomic<float> m_attack;
    std::atomic<float> m_release;
    std::atomic<float> m_range;

    float m_envelope = 0.0f;
};

#endif
