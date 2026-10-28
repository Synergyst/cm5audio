#ifndef LPF_PROCESSOR_H
#define LPF_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>

class LpfProcessor : public AudioProcessor {
public:
    LpfProcessor(float cutoff);
    void process(float* buffer, size_t frames, unsigned channels) override;
private:
    std::atomic<float> m_cutoff;
    float m_z = 0.0f; // previous output
};
#endif
