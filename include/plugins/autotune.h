#ifndef AUTOTUNE_PROCESSOR_H
#define AUTOTUNE_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <cstddef>
#include <string>

class AutotuneProcessor : public AudioProcessor {
public:
    AutotuneProcessor(float speed, float sensitivity, unsigned key, unsigned scale);
    ~AutotuneProcessor() override = default;
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        return id == "speed" || id == "sensitivity";
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        if (id == "speed") { m_speed.store(value, std::memory_order_relaxed); return true; }
        if (id == "sensitivity") { m_sensitivity.store(value, std::memory_order_relaxed); return true; }
        return false;
    }

    const char* parameter_control_type() const noexcept override { return "autotune"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        if (id == "speed") { value = m_speed.load(std::memory_order_relaxed); return true; }
        if (id == "sensitivity") { value = m_sensitivity.load(std::memory_order_relaxed); return true; }
        return false;
    }

private:
    static constexpr float SAMPLE_RATE = 48000.0f;
    static constexpr size_t ANALYSIS_WINDOW = 1024;
    static constexpr size_t MIN_LAG = 24;
    static constexpr size_t MAX_LAG = 370;
    static constexpr size_t DETECTION_HOP = 240;
    static constexpr size_t DELAY_BUFFER_SIZE = 8192;
    static constexpr float MIN_DELAY = 256.0f;
    static constexpr float DELAY_SPAN = 2048.0f;
    float detect_pitch();
    float get_nearest_note_freq(float freq) const;
    float read_delay(float delay) const;
    std::atomic<float> m_speed;
    std::atomic<float> m_sensitivity;
    unsigned m_key;
    unsigned m_scale;
    float m_analysis_buffer[ANALYSIS_WINDOW]{};
    float m_analysis_work[ANALYSIS_WINDOW]{};
    float m_yin_difference[MAX_LAG + 1]{};
    float m_yin_cmnd[MAX_LAG + 1]{};
    size_t m_analysis_idx = 0;
    size_t m_analysis_count = 0;
    unsigned m_decimation_phase = 0;
    float m_decimator_sample = 0.0f;
    size_t m_samples_since_detection = 0;
    float m_current_pitch_ratio = 1.0f;
    float m_smoothed_pitch_ratio = 1.0f;
    float m_delay_buffer[DELAY_BUFFER_SIZE]{};
    size_t m_write_pos = 0;
    float m_phase = 0.0f;
    int m_shift_direction = 1;
};

#endif
