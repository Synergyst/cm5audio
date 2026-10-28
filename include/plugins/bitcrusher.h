#ifndef BITCRUSHER_PROCESSOR_H
#define BITCRUSHER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>

class BitcrusherProcessor : public AudioProcessor {
public:
    BitcrusherProcessor(float bits, float sample_rate);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_bits;
    std::atomic<float> m_sample_rate;

    float m_last_samples[8] = {0.0f};
    unsigned int m_sample_counters[8] = {0};
};

#endif
