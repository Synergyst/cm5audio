#ifndef SOFT_CLIPPER_PROCESSOR_H
#define SOFT_CLIPPER_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>
#include <string>
class SoftClipperProcessor : public AudioProcessor {
public:
    SoftClipperProcessor(float drive, float ceiling);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override { return id == "drive" || id == "ceiling"; }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "drive") { m_drive.store(value, std::memory_order_relaxed); return true; }
        if (id == "ceiling") { m_ceiling.store(value, std::memory_order_relaxed); return true; }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "soft_clipper"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "drive") { value = m_drive.load(std::memory_order_relaxed); return true; }
        if (id == "ceiling") { value = m_ceiling.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_drive;
    std::atomic<float> m_ceiling;
};
#endif
