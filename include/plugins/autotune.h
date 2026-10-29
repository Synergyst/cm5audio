#ifndef AUTOTUNE_PROCESSOR_H
#define AUTOTUNE_PROCESSOR_H

#include "audio_processor.h"
#include <atomic>
#include <cstddef>

class AutotuneProcessor : public AudioProcessor {
public:
    AutotuneProcessor(float speed, float sensitivity,
                      unsigned key, unsigned scale);
    ~AutotuneProcessor() override = default;

    void process(float* buffer, size_t frames,
                 unsigned channels) override;

private:
    static constexpr float SAMPLE_RATE = 48000.0f;

    // Pitch detection: downsampled 48 kHz -> 24 kHz.
    static constexpr size_t ANALYSIS_WINDOW = 1024;
    static constexpr size_t MIN_LAG = 24;
    static constexpr size_t MAX_LAG = 370;
    static constexpr size_t DETECTION_HOP = 240;

    // Dual-head delay-line pitch shifter.
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

    // Circular pitch-analysis window.
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

    // Fixed-size delay line.
    float m_delay_buffer[DELAY_BUFFER_SIZE]{};

    size_t m_write_pos = 0;
    float m_phase = 0.0f;

    int m_shift_direction = 1;
};

#endif
