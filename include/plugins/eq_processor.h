#ifndef CM5AUDIO_PLUGIN_EQ_H
#define CM5AUDIO_PLUGIN_EQ_H

#include "audio_processor.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

class EqProcessor : public AudioProcessor {
public:
    struct Band {
        std::atomic<float> freq;
        std::atomic<float> gain;
        std::atomic<float> q;
        float z1 = 0.0f;
        float z2 = 0.0f;
        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f, b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        void updateCoeffs(float sampleRate);
        Band(const Band& other) : freq(other.freq.load()), gain(other.gain.load()), q(other.q.load()) {}
        Band() = default;
    };
    EqProcessor(float sampleRate = 48000.0f);
    void process(float* buffer, size_t frames, unsigned channels) override;
    bool supports_parameter_control(const std::string& id) const noexcept override {
        unsigned band = 0;
        const char* p = id.c_str();
        if (*p++ != 'b' || *p < '1' || *p > '9') return false;
        band = static_cast<unsigned>(*p++ - '0');
        if (*p >= '0' && *p <= '9') band = band * 10u + static_cast<unsigned>(*p++ - '0');
        if (band < 1 || band > 24 || *p++ != '_') return false;
        return std::strcmp(p, "freq") == 0 || std::strcmp(p, "gain") == 0 || std::strcmp(p, "q") == 0;
    }
    bool set_parameter_control(const std::string& id, float value) noexcept override {
        unsigned band = 0;
        const char* p = id.c_str();
        if (*p++ != 'b' || *p < '1' || *p > '9') return false;
        band = static_cast<unsigned>(*p++ - '0');
        if (*p >= '0' && *p <= '9') band = band * 10u + static_cast<unsigned>(*p++ - '0');
        if (band < 1 || band > m_bands.size() || *p++ != '_') return false;
        if (std::strcmp(p, "freq") == 0) {
            m_bands[band - 1].freq.store(value, std::memory_order_relaxed); return true;
        }
        if (std::strcmp(p, "gain") == 0) {
            m_bands[band - 1].gain.store(value, std::memory_order_relaxed); return true;
        }
        if (std::strcmp(p, "q") == 0) {
            m_bands[band - 1].q.store(value, std::memory_order_relaxed); return true;
        }
        return false;
    }
    const char* parameter_control_type() const noexcept override { return "eq24"; }
    bool get_parameter_control(const std::string& id, float& value) const noexcept override {
        const char* p = id.c_str();
        if (*p++ != 'b' || *p < '1' || *p > '9') return false;
        unsigned band = static_cast<unsigned>(*p++ - '0');
        if (*p >= '0' && *p <= '9') band = band * 10u + static_cast<unsigned>(*p++ - '0');
        if (band < 1 || band > m_bands.size() || *p++ != '_') return false;
        if (std::strcmp(p, "freq") == 0) value = m_bands[band - 1].freq.load(std::memory_order_relaxed);
        else if (std::strcmp(p, "gain") == 0) value = m_bands[band - 1].gain.load(std::memory_order_relaxed);
        else if (std::strcmp(p, "q") == 0) value = m_bands[band - 1].q.load(std::memory_order_relaxed);
        else return false;
        return true;
    }
    std::deque<Band> m_bands;
private:
    float m_sampleRate;
};

#endif
