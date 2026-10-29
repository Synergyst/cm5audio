#ifndef SOFT_CLIPPER_PROCESSOR_H
#define SOFT_CLIPPER_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>

class SoftClipperProcessor : public AudioProcessor {
public:
    SoftClipperProcessor(float drive, float ceiling);
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_drive;
    std::atomic<float> m_ceiling;
};

#endif
