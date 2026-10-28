#ifndef LIMITER_PROCESSOR_H
#define LIMITER_PROCESSOR_H

#include "audio_processor.h"
#include <vector>
#include <atomic>

class LimiterProcessor : public AudioProcessor {
public:
    LimiterProcessor(float threshold, float attack, float release, float lookahead_ms);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_attack;
    std::atomic<float> m_release;

    float m_envelope = 1.0f;
    
    // Look-ahead delay line
    std::vector<float> m_delay_buffer;
    size_t m_write_pos = 0;
    size_t m_buffer_size = 0;
};

#endif
