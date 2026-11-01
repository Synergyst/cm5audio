#ifndef LPF_PROCESSOR_H
#define LPF_PROCESSOR_H
#include "audio_processor.h"
#include <atomic>
#include <string>
#include <vector>
class LpfProcessor : public AudioProcessor {
public:
    LpfProcessor(float cutoff);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override { return id == "cutoff"; }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id != "cutoff") return false;
        m_cutoff.store(value, std::memory_order_relaxed); return true;
    }
    const char* parameter_control_type() const noexcept override { return "lpf"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "cutoff") { value = m_cutoff.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    std::atomic<float> m_cutoff;
    std::vector<float> m_z;
};
#endif
