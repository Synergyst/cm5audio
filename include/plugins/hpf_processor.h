#ifndef HPF_PROCESSOR_H
#define HPF_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>
#include <vector>

class HpfProcessor : public AudioProcessor {
public:
    HpfProcessor(float cutoff);
    void process(float* buffer, size_t frames, unsigned channels) override;
private:
    std::atomic<float> m_cutoff;
    std::vector<float> m_z; // Per-channel LPF state
};
#endif
