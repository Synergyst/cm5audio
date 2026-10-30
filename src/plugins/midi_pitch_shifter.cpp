#include "audio_processor.h"
#include "processor_registry.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <spa/control/control.h>
#include <spa/pod/iter.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define MPS_LOG(...) do { \
    std::fprintf(stderr, "[MIDI Pitch Shifter] "); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fputc('\n', stderr); \
    std::fflush(stderr); \
} while (0)

namespace {

// ====================================================================
// Shared PipeWire MIDI receiver -- uses pw_filter, not pw_stream.
// ====================================================================

class MidiInputService final {
public:
    static std::shared_ptr<MidiInputService> acquire() {
        static std::mutex mutex;
        static std::weak_ptr<MidiInputService> weak;

        std::lock_guard<std::mutex> lock(mutex);

        auto instance = weak.lock();

        if (!instance) {
            instance = std::shared_ptr<MidiInputService>(
                new MidiInputService());

            weak = instance;
        }

        return instance;
    }

    bool available() const noexcept {
        return available_;
    }

    float get_pitch_bend(unsigned channel) const noexcept {
        if (channel < 1 || channel > 16)
            return 0.0f;

        return pitch_bends_[channel - 1].load(
            std::memory_order_relaxed);
    }

    ~MidiInputService() {
        MPS_LOG("Shared MIDI service destructor");
        stop_midi_input();
    }

private:
    static constexpr unsigned kChannels = 16;
    static constexpr size_t kMaxMidiEventSize = 1024;

    std::array<std::atomic<float>, kChannels> pitch_bends_{};

    std::array<float, kChannels> last_logged_bend_{};
    std::array<bool, kChannels> have_logged_bend_{};
    std::array<std::chrono::steady_clock::time_point, kChannels>
        last_bend_log_time_{};

    std::array<std::array<uint8_t, 128>, kChannels> last_cc_{};
    std::array<std::array<bool, 128>, kChannels> have_cc_{};

    std::array<uint64_t, 16> ump_type_counts_{};
    std::array<bool, 16> utility_logged_{};

    uint64_t callback_count_ = 0;
    uint64_t ump_event_count_ = 0;
    uint64_t midi_event_count_ = 0;
    uint64_t pitch_bend_count_ = 0;
    uint64_t raw_dump_count_ = 0;

    std::chrono::steady_clock::time_point stats_time_{};

    pw_main_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    void* input_port_ = nullptr;

    std::thread loop_thread_;
    bool available_ = false;

    MidiInputService() {
        for (auto& bend : pitch_bends_)
            bend.store(0.0f, std::memory_order_relaxed);

        stats_time_ = std::chrono::steady_clock::now();

        MPS_LOG("Creating shared MIDI input service");

        try {
            available_ = start_midi_input();
        } catch (const std::exception& error) {
            MPS_LOG("MIDI initialization exception: %s", error.what());
            stop_midi_input();
        } catch (...) {
            MPS_LOG("Unknown MIDI initialization exception");
            stop_midi_input();
        }

        MPS_LOG(
            "Shared MIDI input: %s",
            available_ ? "AVAILABLE" : "UNAVAILABLE");
    }

    // ---------------------------------------------------------------
    // Same UMP packet sizing used by the working midi-testing app.
    // ---------------------------------------------------------------

    static size_t ump_message_words(uint8_t message_type) noexcept {
        switch (message_type & 0x0F) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x6:
        case 0x7:
            return 1;

        case 0x3:
        case 0x4:
        case 0x8:
        case 0x9:
        case 0xA:
            return 2;

        case 0xB:
        case 0xC:
            return 3;

        case 0x5:
        case 0xD:
        case 0xE:
        case 0xF:
            return 4;

        default:
            return 0;
        }
    }

    // ---------------------------------------------------------------
    // MIDI value storage and diagnostics.
    // ---------------------------------------------------------------

    void update_pitch_bend(
        unsigned channel,
        float normalized,
        const char* protocol,
        unsigned group,
        uint32_t raw_value) noexcept {

        if (channel < 1 || channel > 16)
            return;

        normalized = std::clamp(normalized, -1.0f, 1.0f);

        const unsigned index = channel - 1;

        pitch_bends_[index].store(
            normalized,
            std::memory_order_relaxed);

        ++pitch_bend_count_;

        const auto now = std::chrono::steady_clock::now();

        constexpr auto kLogInterval =
            std::chrono::milliseconds(25);

        const bool first = !have_logged_bend_[index];

        const bool changed =
            first ||
            std::fabs(
                normalized - last_logged_bend_[index]) > 0.0001f;

        const bool interval_elapsed =
            first ||
            now - last_bend_log_time_[index] >= kLogInterval;

        if (changed && interval_elapsed) {
            MPS_LOG(
                "PITCH BEND: %s group=%u channel=%u "
                "raw=0x%08X normalized=%+.6f",
                protocol,
                group,
                channel,
                raw_value,
                normalized);

            last_logged_bend_[index] = normalized;
            last_bend_log_time_[index] = now;
            have_logged_bend_[index] = true;
        }
    }

    void handle_control_change(
        unsigned channel,
        unsigned controller,
        unsigned value,
        const char* protocol) noexcept {

        if (channel < 1 || channel > 16 ||
            controller > 127 || value > 127)
            return;

        const unsigned index = channel - 1;

        if (!have_cc_[index][controller] ||
            last_cc_[index][controller] != value) {

            MPS_LOG(
                "CONTROL CHANGE: %s channel=%u CC=%u value=%u",
                protocol,
                channel,
                controller,
                value);

            last_cc_[index][controller] =
                static_cast<uint8_t>(value);

            have_cc_[index][controller] = true;
        }
    }

    // ---------------------------------------------------------------
    // Legacy MIDI 1.0 bytes.
    // ---------------------------------------------------------------

    void handle_midi1(
        const uint8_t* midi,
        size_t size,
        const char* protocol) noexcept {

        if (!midi || size == 0)
            return;

        ++midi_event_count_;

        size_t i = 0;

        while (i < size) {
            const uint8_t status = midi[i];

            if (status >= 0xF8) {
                ++i;
                continue;
            }

            if (status < 0x80 || status >= 0xF0) {
                ++i;
                continue;
            }

            const unsigned message_type = status & 0xF0;
            const unsigned channel = (status & 0x0F) + 1u;

            const size_t message_length =
                (message_type == 0xC0 ||
                 message_type == 0xD0) ? 2u : 3u;

            if (i + message_length > size)
                break;

            const unsigned data1 = midi[i + 1] & 0x7F;

            const unsigned data2 =
                message_length == 3
                    ? midi[i + 2] & 0x7F
                    : 0u;

            if (message_type == 0xE0) {
                const unsigned value =
                    (data2 << 7) | data1;

                const int offset =
                    static_cast<int>(value) - 8192;

                const float normalized =
                    offset >= 0
                        ? static_cast<float>(offset) / 8191.0f
                        : static_cast<float>(offset) / 8192.0f;

                update_pitch_bend(
                    channel,
                    normalized,
                    protocol,
                    0,
                    value);
            }
            else if (message_type == 0xB0) {
                handle_control_change(
                    channel,
                    data1,
                    data2,
                    protocol);
            }

            i += message_length;
        }
    }

    // ---------------------------------------------------------------
    // Raw UMP parsing.
    //
    // The incoming SPA payload is copied into aligned uint32_t
    // storage, just as in the working midi-testing implementation.
    // ---------------------------------------------------------------

    void dump_ump(
        const uint32_t* words,
        size_t word_count,
        unsigned message_type,
        unsigned group) noexcept {

        if (message_type == 0x0 || raw_dump_count_ >= 32)
            return;

        MPS_LOG(
            "UMP MT=%u group=%u words=%zu: "
            "W0=0x%08X W1=0x%08X W2=0x%08X W3=0x%08X",
            message_type,
            group,
            word_count,
            words[0],
            word_count > 1 ? words[1] : 0,
            word_count > 2 ? words[2] : 0,
            word_count > 3 ? words[3] : 0);

        ++raw_dump_count_;
    }

    void handle_ump(
        const uint8_t* payload,
        size_t size) noexcept {

        if (!payload || size == 0 ||
            size > kMaxMidiEventSize ||
            size % sizeof(uint32_t) != 0)
            return;

        ++ump_event_count_;

        // Aligned copy, exactly like midi-testing.
        std::array<uint32_t,
            kMaxMidiEventSize / sizeof(uint32_t)> ump_copy{};

        std::memcpy(ump_copy.data(), payload, size);

        const size_t total_words =
            size / sizeof(uint32_t);

        size_t word_index = 0;

        while (word_index < total_words) {
            const uint32_t word0 = ump_copy[word_index];

            const unsigned message_type =
                (word0 >> 28) & 0x0F;

            const size_t packet_words =
                ump_message_words(
                    static_cast<uint8_t>(message_type));

            if (packet_words == 0 ||
                packet_words > total_words - word_index) {

                MPS_LOG(
                    "Malformed UMP: MT=%u total_words=%zu "
                    "word_index=%zu",
                    message_type,
                    total_words,
                    word_index);

                break;
            }

            const unsigned group =
                (word0 >> 24) & 0x0F;

            ++ump_type_counts_[message_type];

            if (message_type == 0x0) {
                if (!utility_logged_[group]) {
                    MPS_LOG(
                        "UMP utility No-op/utility traffic: group=%u",
                        group);

                    utility_logged_[group] = true;
                }

                word_index += packet_words;
                continue;
            }

            dump_ump(
                ump_copy.data() + word_index,
                packet_words,
                message_type,
                group);

            // MIDI 1.0 Channel Voice UMP.
            if (message_type == 0x2) {
                const unsigned status =
                    (word0 >> 20) & 0x0F;

                const unsigned channel =
                    ((word0 >> 16) & 0x0F) + 1u;

                const unsigned data1 =
                    (word0 >> 8) & 0x7F;

                const unsigned data2 =
                    word0 & 0x7F;

                if (status == 0x0E) {
                    const unsigned value =
                        (data2 << 7) | data1;

                    const int offset =
                        static_cast<int>(value) - 8192;

                    const float normalized =
                        offset >= 0
                            ? static_cast<float>(offset) / 8191.0f
                            : static_cast<float>(offset) / 8192.0f;

                    update_pitch_bend(
                        channel,
                        normalized,
                        "UMP-MIDI1",
                        group,
                        value);
                }
                else if (status == 0x0B) {
                    handle_control_change(
                        channel,
                        data1,
                        data2,
                        "UMP-MIDI1");
                }
            }

            // MIDI 2.0 Channel Voice UMP.
            else if (message_type == 0x4) {
                const uint32_t word1 =
                    ump_copy[word_index + 1];

                const unsigned status =
                    (word0 >> 20) & 0x0F;

                const unsigned channel =
                    ((word0 >> 16) & 0x0F) + 1u;

                if (status == 0x0E) {
                    // Full-resolution 32-bit MIDI 2.0 Pitch Bend.
                    double normalized = 0.0;

                    if (word1 >= 0x80000000u) {
                        normalized =
                            static_cast<double>(
                                word1 - 0x80000000u) /
                            2147483647.0;
                    } else {
                        normalized =
                            -static_cast<double>(
                                0x80000000u - word1) /
                            2147483648.0;
                    }

                    update_pitch_bend(
                        channel,
                        static_cast<float>(normalized),
                        "UMP-MIDI2",
                        group,
                        word1);
                }
                else if (status == 0x0B) {
                    const unsigned controller =
                        (word0 >> 8) & 0xFF;

                    MPS_LOG(
                        "MIDI2 CC: group=%u channel=%u "
                        "controller=%u value32=0x%08X",
                        group,
                        channel,
                        controller,
                        word1);
                }
                else {
                    static unsigned other_midi2_logged = 0;

                    if (other_midi2_logged < 32) {
                        MPS_LOG(
                            "MIDI2 message: group=%u channel=%u "
                            "status=0x%X W0=0x%08X W1=0x%08X",
                            group,
                            channel,
                            status,
                            word0,
                            word1);

                        ++other_midi2_logged;
                    }
                }
            }

            word_index += packet_words;
        }
    }

    // ---------------------------------------------------------------
    // Diagnostic statistics.
    // ---------------------------------------------------------------

    void log_stats() noexcept {
        const auto now = std::chrono::steady_clock::now();

        if (now - stats_time_ < std::chrono::seconds(2))
            return;

        MPS_LOG(
            "RX STATS: callbacks=%llu UMP-events=%llu "
            "MIDI-events=%llu pitch-bends=%llu",
            static_cast<unsigned long long>(callback_count_),
            static_cast<unsigned long long>(ump_event_count_),
            static_cast<unsigned long long>(midi_event_count_),
            static_cast<unsigned long long>(pitch_bend_count_));

        MPS_LOG(
            "UMP TYPES: MT0=%llu MT1=%llu MT2=%llu MT3=%llu "
            "MT4=%llu MT5=%llu MT6=%llu MT7=%llu "
            "MT8=%llu MT9=%llu MTA=%llu MTB=%llu "
            "MTC=%llu MTD=%llu MTE=%llu MTF=%llu",
            static_cast<unsigned long long>(ump_type_counts_[0]),
            static_cast<unsigned long long>(ump_type_counts_[1]),
            static_cast<unsigned long long>(ump_type_counts_[2]),
            static_cast<unsigned long long>(ump_type_counts_[3]),
            static_cast<unsigned long long>(ump_type_counts_[4]),
            static_cast<unsigned long long>(ump_type_counts_[5]),
            static_cast<unsigned long long>(ump_type_counts_[6]),
            static_cast<unsigned long long>(ump_type_counts_[7]),
            static_cast<unsigned long long>(ump_type_counts_[8]),
            static_cast<unsigned long long>(ump_type_counts_[9]),
            static_cast<unsigned long long>(ump_type_counts_[10]),
            static_cast<unsigned long long>(ump_type_counts_[11]),
            static_cast<unsigned long long>(ump_type_counts_[12]),
            static_cast<unsigned long long>(ump_type_counts_[13]),
            static_cast<unsigned long long>(ump_type_counts_[14]),
            static_cast<unsigned long long>(ump_type_counts_[15]));

        callback_count_ = 0;
        ump_event_count_ = 0;
        midi_event_count_ = 0;
        pitch_bend_count_ = 0;
        ump_type_counts_.fill(0);

        stats_time_ = now;
    }

    // ---------------------------------------------------------------
    // pw_filter callbacks.
    // ---------------------------------------------------------------

    static void on_state_changed(
        void* userdata,
        enum pw_filter_state old_state,
        enum pw_filter_state new_state,
        const char* error) {

        (void)userdata;

        MPS_LOG(
            "PipeWire MIDI filter state: %d -> %d; error=%s",
            static_cast<int>(old_state),
            static_cast<int>(new_state),
            error ? error : "(none)");
    }

    static void on_process(
        void* userdata,
        spa_io_position* position) {

        (void)position;

        auto* self =
            static_cast<MidiInputService*>(userdata);

        if (!self || !self->input_port_)
            return;

        ++self->callback_count_;

        self->log_stats();

        pw_buffer* input_buffer =
            pw_filter_dequeue_buffer(self->input_port_);

        if (!input_buffer)
            return;

        if (input_buffer->buffer &&
            input_buffer->buffer->n_datas >= 1) {

            spa_data* data =
                &input_buffer->buffer->datas[0];

            if (data->data &&
                data->chunk &&
                data->chunk->offset <= data->maxsize &&
                data->chunk->size <=
                    data->maxsize - data->chunk->offset &&
                data->chunk->size >= sizeof(spa_pod_sequence)) {

                const auto* base =
                    static_cast<const uint8_t*>(data->data) +
                    data->chunk->offset;

                const auto* sequence =
                    reinterpret_cast<const spa_pod_sequence*>(base);

                const size_t available = data->chunk->size;

                if (sequence->pod.type == SPA_TYPE_Sequence &&
                    SPA_POD_SIZE(&sequence->pod) <= available) {

                    const spa_pod_control* control = nullptr;

                    SPA_POD_SEQUENCE_FOREACH(sequence, control) {
                        if (control->value.type != SPA_TYPE_Bytes)
                            continue;

                        const size_t event_size =
                            SPA_POD_BODY_SIZE(&control->value);

                        const auto* payload =
                            static_cast<const uint8_t*>(
                                SPA_POD_BODY(&control->value));

                        if (!payload || event_size == 0)
                            continue;

                        if (control->type == SPA_CONTROL_UMP) {
                            self->handle_ump(payload, event_size);
                        }
                        else if (control->type == SPA_CONTROL_Midi) {
                            self->handle_midi1(
                                payload,
                                event_size,
                                "PipeWire-MIDI1");
                        }
                    }
                }
            }
        }

        // Recycle the input buffer, matching the working
        // midi-testing pw_filter implementation.
        pw_filter_queue_buffer(self->input_port_, input_buffer);
    }

    static const pw_filter_events filter_events_;

    // ---------------------------------------------------------------
    // Filter setup and teardown.
    // ---------------------------------------------------------------

    bool start_midi_input() {
        MPS_LOG("Creating PipeWire main loop");

        loop_ = pw_main_loop_new(nullptr);

        if (!loop_)
            return fail("pw_main_loop_new");

        MPS_LOG("Creating pw_filter MIDI sink");

        pw_properties* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Midi",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_CLASS, "Midi/Sink",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_APP_NAME, "CM5Audio",
            PW_KEY_NODE_NAME, "cm5audio-midi-pitch-shifter",
            PW_KEY_NODE_DESCRIPTION,
                "Shared MIDI Pitch Shifter Input",
            PW_KEY_NODE_AUTOCONNECT, "false",
            nullptr);

        if (!properties)
            return fail("pw_properties_new");

        filter_ = pw_filter_new_simple(
            pw_main_loop_get_loop(loop_),
            "CM5Audio MIDI Pitch Shifter",
            properties,
            &filter_events_,
            this);

        if (!filter_)
            return fail("pw_filter_new_simple");

        MPS_LOG("PipeWire MIDI filter created");

        // This is the same port format used by the working
        // midi-testing application's raw UMP input.
        pw_properties* port_properties = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit raw UMP",
            PW_KEY_PORT_NAME, "input_1",
            nullptr);

        if (!port_properties)
            return fail("MIDI port properties");

        MPS_LOG("Adding 32 bit raw UMP input port");

        input_port_ = pw_filter_add_port(
            filter_,
            PW_DIRECTION_INPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            0,
            port_properties,
            nullptr,
            0);

        if (!input_port_)
            return fail("pw_filter_add_port");

        MPS_LOG("Raw UMP input port created");

        MPS_LOG("Connecting PipeWire filter");

        const int result =
            pw_filter_connect(
                filter_,
                PW_FILTER_FLAG_RT_PROCESS,
                nullptr,
                0);

        if (result < 0)
            return fail("pw_filter_connect", result);

        MPS_LOG("PipeWire MIDI filter connected");

        try {
            loop_thread_ = std::thread([this]() {
                MPS_LOG("PipeWire MIDI main loop running");

                const int run_result =
                    pw_main_loop_run(loop_);

                MPS_LOG(
                    "PipeWire MIDI main loop returned: %d",
                    run_result);
            });
        } catch (const std::exception& error) {
            MPS_LOG(
                "Unable to start PipeWire main-loop thread: %s",
                error.what());

            return fail("std::thread");
        }

        MPS_LOG("Shared raw UMP input initialized");
        MPS_LOG("Expected port format: 32 bit raw UMP");

        return true;
    }

    bool fail(
        const char* operation,
        int error = 0) noexcept {

        MPS_LOG(
            "MIDI initialization failed at %s (error=%d)",
            operation,
            error);

        stop_midi_input();

        return false;
    }

    void stop_midi_input() noexcept {
        MPS_LOG("Stopping shared MIDI input");

        if (loop_) {
            pw_main_loop_quit(loop_);
        }

        if (loop_thread_.joinable()) {
            loop_thread_.join();
        }

        if (filter_) {
            MPS_LOG("Destroying PipeWire MIDI filter");

            pw_filter_destroy(filter_);
            filter_ = nullptr;
            input_port_ = nullptr;
        }

        if (loop_) {
            pw_main_loop_destroy(loop_);
            loop_ = nullptr;
        }

        MPS_LOG("Shared MIDI input stopped");
    }
};

const pw_filter_events MidiInputService::filter_events_ = [] {
    pw_filter_events events{};

    events.version = PW_VERSION_FILTER_EVENTS;
    events.process = MidiInputService::on_process;
    events.state_changed = MidiInputService::on_state_changed;

    return events;
}();


// ====================================================================
// Audio pitch shifter
// ====================================================================

class MidiPitchShifterProcessor final : public AudioProcessor {
public:
    explicit MidiPitchShifterProcessor(
        unsigned channel,
        float pitch_range)
        : listen_channel_(std::clamp(channel, 1u, 16u)),
          pitch_range_semitones_(
              std::clamp(pitch_range, 0.5f, 24.0f)),
          midi_input_(MidiInputService::acquire()),
          delay_line_(kMaxChannels * kRingFrames, 0.0f) {

        MPS_LOG(
            "Processor created: %p channel=%u range=+/-%.1f "
            "service=%p MIDI=%s",
            static_cast<void*>(this),
            listen_channel_,
            pitch_range_semitones_,
            static_cast<void*>(midi_input_.get()),
            midi_input_->available() ? "available" : "unavailable");
    }

    ~MidiPitchShifterProcessor() override {
        MPS_LOG(
            "Processor destroyed: %p channel=%u",
            static_cast<void*>(this),
            listen_channel_);
    }

    void process(
        float* buffer,
        size_t frames,
        unsigned channels) override {

        if (!buffer || frames == 0 || channels == 0)
            return;

        const float normalized =
            midi_input_->get_pitch_bend(listen_channel_);

        const float semitones =
            normalized * pitch_range_semitones_;

        const float target_ratio =
            std::exp2(semitones / 12.0f);

        const float target_wet =
            std::fabs(target_ratio - 1.0f) > 0.000001f
                ? 1.0f
                : 0.0f;

        for (size_t frame = 0; frame < frames; ++frame) {
            smoothed_ratio_ +=
                (target_ratio - smoothed_ratio_) * kSmoothing;

            wet_mix_ +=
                (target_wet - wet_mix_) * kSmoothing;

            const float phase_a = phase_;

            float phase_b = phase_a + 0.5f;

            if (phase_b >= 1.0f)
                phase_b -= 1.0f;

            const float weight_a =
                0.5f - 0.5f * std::cos(kTwoPi * phase_a);

            const float weight_b = 1.0f - weight_a;

            const float delay_a =
                static_cast<float>(kMinDelayFrames) +
                phase_a * static_cast<float>(kDelaySpanFrames);

            const float delay_b =
                static_cast<float>(kMinDelayFrames) +
                phase_b * static_cast<float>(kDelaySpanFrames);

            for (unsigned channel = 0;
                 channel < channels && channel < kMaxChannels;
                 ++channel) {

                const size_t index =
                    frame * static_cast<size_t>(channels) + channel;

                const float input = buffer[index];

                float* channel_delay =
                    delay_line_.data() +
                    static_cast<size_t>(channel) * kRingFrames;

                channel_delay[write_position_] = input;

                if (wet_mix_ > 0.00001f) {
                    const float a =
                        read_delayed(
                            channel_delay,
                            delay_a,
                            write_position_);

                    const float b =
                        read_delayed(
                            channel_delay,
                            delay_b,
                            write_position_);

                    const float shifted =
                        a * weight_a + b * weight_b;

                    buffer[index] =
                        input + (shifted - input) * wet_mix_;
                }
            }

            // Moving read heads change pitch without changing
            // the overall audio stream's playback duration.
            const float phase_step =
                (1.0f - smoothed_ratio_) /
                static_cast<float>(kDelaySpanFrames);

            phase_ += phase_step;

            if (phase_ >= 1.0f)
                phase_ -= 1.0f;
            else if (phase_ < 0.0f)
                phase_ += 1.0f;

            ++write_position_;

            if (write_position_ >= kRingFrames)
                write_position_ = 0;
        }
    }

private:
    static constexpr size_t kSampleRate = 48000;

    static constexpr size_t kMinDelayFrames =
        (kSampleRate * 20) / 1000;

    static constexpr size_t kDelaySpanFrames =
        (kSampleRate * 100) / 1000;

    static constexpr size_t kRingFrames =
        kMinDelayFrames + kDelaySpanFrames + 4;

    static constexpr unsigned kMaxChannels = 32;

    static constexpr float kSmoothing = 0.001f;

    static constexpr float kTwoPi =
        6.2831853071795864769f;

    const unsigned listen_channel_;
    const float pitch_range_semitones_;

    std::shared_ptr<MidiInputService> midi_input_;

    std::vector<float> delay_line_;

    size_t write_position_ = 0;
    float phase_ = 0.0f;
    float smoothed_ratio_ = 1.0f;
    float wet_mix_ = 0.0f;

    float read_delayed(
        const float* delay,
        float delay_frames,
        size_t write_position) const noexcept {

        float read_position =
            static_cast<float>(write_position) - delay_frames;

        if (read_position < 0.0f)
            read_position += static_cast<float>(kRingFrames);

        const size_t index0 =
            static_cast<size_t>(read_position);

        const size_t index1 =
            (index0 + 1u >= kRingFrames)
                ? 0u
                : index0 + 1u;

        const float fraction =
            read_position - static_cast<float>(index0);

        return delay[index0] +
            (delay[index1] - delay[index0]) * fraction;
    }
};

} // namespace


// ====================================================================
// Registration
// ====================================================================

void register_midi_pitch_shifter_processor(
    ProcessorRegistry& registry) {

    MPS_LOG("Registering MIDI Pitch Shifter processor");

    registry.register_processor({
        "midi_pitch_shifter",
        "MIDI Pitch Shifter",
        "MIDI Pitch Bend-controlled audio pitch shifter. "
        "All instances share one PipeWire raw UMP input.",
        {
            {
                "listen_channel",
                "MIDI Listen Channel",
                "number",
                10.0,
                1.0,
                16.0,
                1.0,
                "",
                {}
            },
            {
                "pitch_range",
                "Pitch Bend Range",
                "number",
                2.0,
                0.5,
                24.0,
                0.5,
                "semitones",
                {}
            }
        },
        [](const nlohmann::json& params) {
            const unsigned channel =
                params.value("listen_channel", 10u);

            const float pitch_range =
                params.value("pitch_range", 2.0f);

            MPS_LOG(
                "Creating processor instance: channel=%u "
                "pitch_range=%.1f",
                channel,
                pitch_range);

            return std::make_unique<MidiPitchShifterProcessor>(
                channel,
                pitch_range);
        }
    });

    MPS_LOG("MIDI Pitch Shifter registration completed");
}
