#include "plugins/limiter.h"
#include "processor_registry.h"
#include <algorithm>
#include <cmath>
#include <memory>

LimiterProcessor::LimiterProcessor(float threshold, float attack, float release, float lookahead_ms)
    : m_threshold(threshold), m_attack(attack), m_release(release) {
    
    // Calculate buffer size based on 48kHz sample rate
    // We allocate enough for the lookahead time across 8 possible channels
    size_t samples_needed = static_cast<size_t>(48000.0 * (lookahead_ms / 1000.0));
    m_buffer_size = samples_needed * 8; 
    m_delay_buffer.assign(m_buffer_size, 0.0f);
}

void LimiterProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float threshold = m_threshold.load(std::memory_order_relaxed);
    const float attack = m_attack.load(std::memory_order_relaxed);
    const float release = m_release.load(std::memory_order_relaxed);
    
    const float attackCoef = std::exp(-1.0f / (48000.0f * attack));
    const float releaseCoef = std::exp(-1.0f / (48000.0f * release));
    
    // Calculate the stride based on the requested lookahead (defaulting to 10ms if not provided)
    // For a production plugin, we'd store the requested lookahead samples in the class.
    // Here we assume the buffer size was allocated for the desired lookahead.
    const size_t lookahead_samples = m_buffer_size / 8; 
    const size_t stride = lookahead_samples * channels;

    for (size_t i = 0; i < frames; ++i) {
        // 1. Detect peak in the CURRENT signal to calculate gain reduction
        float peak = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            peak = std::max(peak, std::abs(buffer[i * channels + c]));
        }

        // 2. Calculate target gain
        float targetGain = 1.0f;
        if (peak > threshold) {
            targetGain = threshold / peak;
        }

        // 3. Smooth the gain envelope
        float coef = (targetGain < m_envelope) ? attackCoef : releaseCoef;
        m_envelope = coef * m_envelope + (1.0f - coef) * targetGain;

        // 4. Apply gain to the DELAYED signal and write current signal to buffer
        for (unsigned c = 0; c < channels; ++c) {
            // Read delayed sample
            size_t read_pos = (m_write_pos + m_buffer_size - stride + c) % m_buffer_size;
            float delayed_sample = m_delay_buffer[read_pos];
            
            // Save the current sample into the buffer before we overwrite the output
            float current_sample = buffer[i * channels + c];
            m_delay_buffer[(m_write_pos + c) % m_buffer_size] = current_sample;
            
            // Apply the smoothed gain to the delayed sample
            buffer[i * channels + c] = std::clamp(delayed_sample * m_envelope, -1.0f, 1.0f);
        }
        
        m_write_pos = (m_write_pos + channels) % m_buffer_size;
    }
}

void register_limiter_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "limiter", "Look-ahead Limiter", "Prevents clipping by analyzing peaks before they hit the output.",
        {
            {"threshold", "Threshold", "number", 0.9, 0.0, 1.0, 0.01, "linear", {}},
            {"attack", "Attack", "number", 0.005, 0.0001, 0.1, 0.0001, "s", {}},
            {"release", "Release", "number", 0.05, 0.001, 1.0, 0.001, "s", {}},
            {"lookahead", "Look-ahead", "number", 10.0, 1.0, 50.0, 1.0, "ms", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<LimiterProcessor>(
                params.at("threshold").get<float>(),
                params.at("attack").get<float>(),
                params.at("release").get<float>(),
                params.at("lookahead").get<float>()
            );
        }
    });
}
