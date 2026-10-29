#include "plugins/autotune.h"
#include "processor_registry.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {

constexpr float kPi = 3.14159265358979323846f;

unsigned key_index_from_name(const std::string& name) {
    static const char* const names[] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    for (unsigned i = 0; i < 12; ++i) {
        if (name == names[i])
            return i;
    }

    return 0;
}

unsigned scale_index_from_name(const std::string& name) {
    if (name == "major")
        return 1;

    if (name == "minor")
        return 2;

    return 0;
}

} // namespace


AutotuneProcessor::AutotuneProcessor(
    float speed,
    float sensitivity,
    unsigned key,
    unsigned scale)
    : m_speed(std::clamp(speed, 0.0f, 1.0f)),
      m_sensitivity(std::clamp(sensitivity, 0.0f, 1.0f)),
      m_key(key % 12),
      m_scale(std::min(scale, 2u)) {}


// ============================================================================
// Pitch detection
//
// Uses a YIN-style difference function and cumulative mean normalized
// difference (CMND). The analysis buffer is stored as a circular buffer,
// so it is first reconstructed into chronological order.
//
// No dynamic allocation occurs here.
// ============================================================================

float AutotuneProcessor::detect_pitch() {
    if (m_analysis_count < ANALYSIS_WINDOW)
        return 0.0f;

    float mean = 0.0f;

    // m_analysis_idx points to the next slot to be written.
    // Once full, that slot contains the oldest sample.
    for (size_t i = 0; i < ANALYSIS_WINDOW; ++i) {
        const size_t source =
            (m_analysis_idx + i) % ANALYSIS_WINDOW;

        m_analysis_work[i] = m_analysis_buffer[source];
        mean += m_analysis_work[i];
    }

    mean /= static_cast<float>(ANALYSIS_WINDOW);

    // Remove DC offset and measure signal energy.
    float energy = 0.0f;

    for (size_t i = 0; i < ANALYSIS_WINDOW; ++i) {
        m_analysis_work[i] -= mean;

        energy +=
            m_analysis_work[i] * m_analysis_work[i];
    }

    // Ignore silence and extremely quiet input.
    if (energy / static_cast<float>(ANALYSIS_WINDOW) < 1.0e-8f)
        return 0.0f;

    // Calculate the squared difference function for each candidate lag.
    for (size_t lag = 1; lag <= MAX_LAG; ++lag) {
        float sum = 0.0f;

        const size_t count = ANALYSIS_WINDOW - lag;

        for (size_t i = 0; i < count; ++i) {
            const float delta =
                m_analysis_work[i] -
                m_analysis_work[i + lag];

            sum += delta * delta;
        }

        m_yin_difference[lag] = sum;
    }

    // Cumulative mean normalized difference.
    float running_sum = 0.0f;

    m_yin_cmnd[0] = 1.0f;

    for (size_t lag = 1; lag <= MAX_LAG; ++lag) {
        running_sum += m_yin_difference[lag];

        m_yin_cmnd[lag] =
            running_sum > 1.0e-12f
                ? m_yin_difference[lag] *
                      static_cast<float>(lag) / running_sum
                : 1.0f;
    }

    // Higher sensitivity accepts less-perfect periodicity.
    const float sensitivity =
        m_sensitivity.load(std::memory_order_relaxed);

    const float threshold = 0.12f + 0.18f * sensitivity;

    size_t best_lag = 0;

    for (size_t lag = MIN_LAG; lag < MAX_LAG; ++lag) {
        if (m_yin_cmnd[lag] < threshold) {
            // Follow the first convincing trough to its minimum.
            // This avoids selecting a later harmonic unnecessarily.
            while (lag + 1 <= MAX_LAG &&
                   m_yin_cmnd[lag + 1] < m_yin_cmnd[lag]) {
                ++lag;
            }

            best_lag = lag;
            break;
        }
    }

    if (best_lag == 0)
        return 0.0f;

    // Parabolic interpolation improves the lag estimate.
    float refined_lag = static_cast<float>(best_lag);

    if (best_lag > MIN_LAG && best_lag < MAX_LAG) {
        const float left =
            m_yin_cmnd[best_lag - 1];

        const float center =
            m_yin_cmnd[best_lag];

        const float right =
            m_yin_cmnd[best_lag + 1];

        const float denom =
            left - 2.0f * center + right;

        if (std::fabs(denom) > 1.0e-9f) {
            const float offset =
                0.5f * (left - right) / denom;

            refined_lag +=
                std::clamp(offset, -0.5f, 0.5f);
        }
    }

    // Analysis samples are at 24 kHz because of decimation.
    return (SAMPLE_RATE * 0.5f) / refined_lag;
}


// ============================================================================
// Musical note selection
//
// Scale index:
//   0 = chromatic
//   1 = major
//   2 = natural minor
//
// Key index:
//   C=0, C#=1, D=2, ... B=11
// ============================================================================

float AutotuneProcessor::get_nearest_note_freq(float freq) const {
    if (!(freq > 0.0f) || !std::isfinite(freq))
        return 0.0f;

    const float midi =
        69.0f + 12.0f * std::log2(freq / 440.0f);

    int target_note =
        static_cast<int>(std::lround(midi));

    // Chromatic mode: nearest equal-tempered semitone.
    if (m_scale != 0) {
        float best_distance = 1.0e9f;

        const int center =
            static_cast<int>(std::lround(midi));

        // Search nearby notes for the closest note in the scale.
        for (int note = center - 6; note <= center + 6; ++note) {
            int pitch_class =
                (note - static_cast<int>(m_key)) % 12;

            if (pitch_class < 0)
                pitch_class += 12;

            bool allowed = false;

            if (m_scale == 1) {
                // Major scale: 0, 2, 4, 5, 7, 9, 11.
                allowed =
                    pitch_class == 0  ||
                    pitch_class == 2  ||
                    pitch_class == 4  ||
                    pitch_class == 5  ||
                    pitch_class == 7  ||
                    pitch_class == 9  ||
                    pitch_class == 11;
            } else {
                // Natural minor: 0, 2, 3, 5, 7, 8, 10.
                allowed =
                    pitch_class == 0  ||
                    pitch_class == 2  ||
                    pitch_class == 3  ||
                    pitch_class == 5  ||
                    pitch_class == 7  ||
                    pitch_class == 8  ||
                    pitch_class == 10;
            }

            if (allowed) {
                const float distance =
                    std::fabs(midi - static_cast<float>(note));

                if (distance < best_distance) {
                    best_distance = distance;
                    target_note = note;
                }
            }
        }
    }

    return 440.0f *
           std::pow(
               2.0f,
               (static_cast<float>(target_note) - 69.0f) / 12.0f);
}


// ============================================================================
// Fractional delay-line reader
//
// Linear interpolation avoids integer-only read positions and reduces
// quantization distortion as the effective playback rate changes.
// ============================================================================

float AutotuneProcessor::read_delay(float delay) const {
    float position =
        static_cast<float>(m_write_pos) - delay;

    if (position < 0.0f)
        position += static_cast<float>(DELAY_BUFFER_SIZE);

    if (position >= static_cast<float>(DELAY_BUFFER_SIZE))
        position -= static_cast<float>(DELAY_BUFFER_SIZE);

    const size_t index0 =
        static_cast<size_t>(position);

    const size_t index1 =
        (index0 + 1) % DELAY_BUFFER_SIZE;

    const float fraction =
        position - static_cast<float>(index0);

    const float a = m_delay_buffer[index0];
    const float b = m_delay_buffer[index1];

    return a + fraction * (b - a);
}


// ============================================================================
// Audio processing
//
// - Assumes the engine's documented 48 kHz sample rate.
// - Uses two overlapping delay-line read heads.
// - Crossfades the heads to hide their delay resets.
// - Uses persistent counters, independent of block size.
// - Does not allocate, lock, perform I/O, or throw in process().
//
// As documented for CM5Audio, route processors currently operate on a
// single channel. If multiple channels are supplied, this implementation
// processes the first input channel and duplicates its output.
// ============================================================================

void AutotuneProcessor::process(
    float* buffer,
    size_t frames,
    unsigned channels) {

    if (buffer == nullptr || frames == 0 || channels == 0)
        return;

    const float speed =
        m_speed.load(std::memory_order_relaxed);

    // Map Snap Speed to a response time between approximately 252 ms
    // at 0 and 2 ms at 1. Higher values snap more aggressively.
    const float slow_component = 1.0f - speed;

    const float time_constant =
        0.002f + slow_component * slow_component * 0.25f;

    const float smoothing_alpha =
        1.0f - std::exp(
            -1.0f / (SAMPLE_RATE * time_constant));

    for (size_t i = 0; i < frames; ++i) {
        const float input = buffer[i * channels];

        // Two-point averaging followed by decimation.
        if (m_decimation_phase == 0) {
            m_decimator_sample = input;
            m_decimation_phase = 1;
        } else {
            const float downsampled =
                0.5f * (m_decimator_sample + input);

            m_analysis_buffer[m_analysis_idx] = downsampled;

            m_analysis_idx =
                (m_analysis_idx + 1) % ANALYSIS_WINDOW;

            if (m_analysis_count < ANALYSIS_WINDOW)
                ++m_analysis_count;

            m_decimation_phase = 0;
        }

        // Persistent timing: the analysis hop is independent of the
        // size of the block passed to process().
        if (++m_samples_since_detection >= DETECTION_HOP) {
            m_samples_since_detection = 0;

            const float detected_hz = detect_pitch();

            if (detected_hz > 0.0f) {
                const float target_hz =
                    get_nearest_note_freq(detected_hz);

                m_current_pitch_ratio =
                    target_hz > 0.0f
                        ? std::clamp(
                              target_hz / detected_hz,
                              0.5f,
                              2.0f)
                        : 1.0f;
            } else {
                // Unvoiced material and silence return toward unity pitch.
                m_current_pitch_ratio = 1.0f;
            }
        }

        // Smooth the ratio instead of changing it abruptly.
        m_smoothed_pitch_ratio +=
            smoothing_alpha *
            (m_current_pitch_ratio - m_smoothed_pitch_ratio);

        const float ratio =
            std::clamp(m_smoothed_pitch_ratio, 0.5f, 2.0f);

        // Determine shift direction. Mirror the phase on direction changes
        // so the two read heads retain their current delays rather than
        // jumping to the opposite ends of the delay sweep.
        int new_direction = m_shift_direction;

        if (ratio > 1.0005f)
            new_direction = 1;
        else if (ratio < 0.9995f)
            new_direction = -1;

        if (new_direction != m_shift_direction) {
            m_phase =
                (m_phase > 0.0f) ? 1.0f - m_phase : 0.0f;

            if (m_phase >= 1.0f)
                m_phase = 0.0f;

            m_shift_direction = new_direction;
        }

        // Store the newest input before reading older samples.
        m_delay_buffer[m_write_pos] = input;

        float phase1 = m_phase;
        float phase2 = phase1 + 0.5f;

        if (phase2 >= 1.0f)
            phase2 -= 1.0f;

        const float maximum_delay =
            MIN_DELAY + DELAY_SPAN;

        // For a higher pitch, delay decreases.
        // For a lower pitch, delay increases.
        const float delay1 =
            m_shift_direction > 0
                ? maximum_delay - phase1 * DELAY_SPAN
                : MIN_DELAY + phase1 * DELAY_SPAN;

        const float delay2 =
            m_shift_direction > 0
                ? maximum_delay - phase2 * DELAY_SPAN
                : MIN_DELAY + phase2 * DELAY_SPAN;

        const float weight1 =
            std::sin(kPi * phase1);

        const float weight2 =
            std::sin(kPi * phase2);

        const float weight_sum =
            weight1 + weight2;

        const float sample1 = read_delay(delay1);
        const float sample2 = read_delay(delay2);

        const float output =
            weight_sum > 1.0e-6f
                ? (sample1 * weight1 +
                   sample2 * weight2) / weight_sum
                : sample2;

        for (unsigned c = 0; c < channels; ++c)
            buffer[i * channels + c] = output;

        m_write_pos =
            (m_write_pos + 1) % DELAY_BUFFER_SIZE;

        // The delay slope produces the required pitch ratio:
        // read speed = 1 - d(delay)/d(sample).
        m_phase +=
            std::fabs(ratio - 1.0f) / DELAY_SPAN;

        if (m_phase >= 1.0f)
            m_phase -= 1.0f;
    }
}


// ============================================================================
// Processor registration
//
// All effect metadata and defaults remain backend-owned.
// ============================================================================

void register_autotune_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "autotune",
        "Autotune",
        "Monophonic pitch correction with chromatic, major, or minor note snapping.",
        {
            {
                "speed", "Snap Speed", "number",
                0.85, 0.0, 1.0, 0.01, "linear", {}
            },
            {
                "sensitivity", "Sensitivity", "number",
                0.25, 0.0, 1.0, 0.01, "linear", {}
            },
            {
                "key", "Key", "enum",
                "C", {}, {}, {}, "",
                {
                    {"C", "C"},
                    {"C#", "C#"},
                    {"D", "D"},
                    {"D#", "D#"},
                    {"E", "E"},
                    {"F", "F"},
                    {"F#", "F#"},
                    {"G", "G"},
                    {"G#", "G#"},
                    {"A", "A"},
                    {"A#", "A#"},
                    {"B", "B"}
                }
            },
            {
                "scale", "Scale", "enum",
                "chromatic", {}, {}, {}, "",
                {
                    {"Chromatic", "chromatic"},
                    {"Major", "major"},
                    {"Minor", "minor"}
                }
            }
        },
        [](const nlohmann::json& params) {
            return std::make_unique<AutotuneProcessor>(
                params.at("speed").get<float>(),
                params.at("sensitivity").get<float>(),
                key_index_from_name(
                    params.at("key").get<std::string>()),
                scale_index_from_name(
                    params.at("scale").get<std::string>())
            );
        }
    });
}
