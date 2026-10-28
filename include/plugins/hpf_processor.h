#ifndef HPF_PROCESSOR_H
#define HPF_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>

class HpfProcessor : public AudioProcessor {
public:
    HpfProcessor(float cutoff);
    void process(float* buffer, size_t frames, unsigned channels) override;
private:
    std::atomic<float> m_cutoff;
    float m_z1 = 0.0f; // previous input
    float m_z2 = 0.0f; // previous output
};
#endif
