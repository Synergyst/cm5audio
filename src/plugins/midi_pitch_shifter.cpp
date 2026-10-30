#include "audio_processor.h"
#include "processor_registry.h"
#include "midi_device_selector.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include <spa/buffer/buffer.h>
#include <spa/control/control.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr unsigned MIDI_CHANNELS = 16;
constexpr unsigned MAX_AUDIO_CHANNELS = 8;

constexpr uint32_t DELAY_RING_SIZE = 8192;
constexpr uint32_t DELAY_RING_MASK = DELAY_RING_SIZE - 1;

constexpr float SAMPLE_RATE = 48000.0f;
constexpr float MIN_DELAY_MS = 20.0f;
constexpr float DELAY_SPAN_MS = 100.0f;

constexpr float MIN_DELAY_SAMPLES =
    SAMPLE_RATE * MIN_DELAY_MS / 1000.0f;

constexpr float DELAY_SPAN_SAMPLES =
    SAMPLE_RATE * DELAY_SPAN_MS / 1000.0f;

constexpr float RATIO_SMOOTHING = 0.002081f;
constexpr unsigned FADE_TABLE_SIZE = 1024;

static_assert((DELAY_RING_SIZE & DELAY_RING_MASK) == 0,
              "Delay ring size must be a power of two");

static size_t umpMessageWords(uint8_t messageType) noexcept
{
    switch (messageType & 0x0f) {
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
    case 0xa:
        return 2;

    case 0xb:
    case 0xc:
        return 3;

    case 0x5:
    case 0xd:
    case 0xe:
    case 0xf:
        return 4;

    default:
        return 0;
    }
}

static float normalizeMidi1PitchBend(uint16_t value) noexcept
{
    if (value >= 8192)
        return static_cast<float>(value - 8192) / 8191.0f;

    return static_cast<float>(
        static_cast<int>(value) - 8192) / 8192.0f;
}

static float normalizeMidi2PitchBend(uint32_t value) noexcept
{
    constexpr uint32_t CENTER = 0x80000000u;

    if (value >= CENTER)
        return static_cast<float>(
            static_cast<double>(value - CENTER) /
            static_cast<double>(0x7fffffffu));

    return static_cast<float>(
        (static_cast<double>(value) -
         static_cast<double>(CENTER)) /
        static_cast<double>(0x80000000u));
}

static std::vector<ProcessorChoice> makeMidiOutputChoices()
{
    std::vector<ProcessorChoice> result;

    const auto choices = cm5audio::midi::makeOutputChoices();
    result.reserve(choices.size());

    for (const auto& item : choices) {
        ProcessorChoice choice;
        choice.label = item.first;
        choice.value = item.second;
        result.emplace_back(std::move(choice));
    }

    return result;
}

class MidiInputService final {
public:
    static std::shared_ptr<MidiInputService> acquire(
        bool autoConnect,
        const std::string& selectedOutput)
    {
        static std::mutex cacheMutex;
        static std::map<std::string, std::weak_ptr<MidiInputService>> cache;

        const std::string key = autoConnect
            ? std::string("auto:") + selectedOutput
            : std::string("manual");

        std::lock_guard<std::mutex> lock(cacheMutex);

        const auto it = cache.find(key);
        if (it != cache.end()) {
            std::shared_ptr<MidiInputService> existing = it->second.lock();
            if (existing)
                return existing;
        }

        std::shared_ptr<MidiInputService> created(
            new MidiInputService(autoConnect, selectedOutput));

        cache[key] = created;
        return created;
    }

    ~MidiInputService()
    {
        stop();
    }

    MidiInputService(const MidiInputService&) = delete;
    MidiInputService& operator=(const MidiInputService&) = delete;

    float pitchBend(unsigned channel) const noexcept
    {
        if (channel >= MIDI_CHANNELS)
            return 0.0f;

        return pitchBends_[channel].load(std::memory_order_acquire);
    }

private:
    struct InputPortData {
        MidiInputService* owner = nullptr;
    };

    std::array<std::atomic<float>, MIDI_CHANNELS> pitchBends_{};

    bool autoConnect_ = false;
    std::string selectedOutput_;

    pw_main_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    void* inputPort_ = nullptr;
    pw_proxy* linkProxy_ = nullptr;
    cm5audio::midi::InputLinkRequest* linkRequest_ = nullptr;

    std::thread thread_;

    MidiInputService(bool autoConnect,
                     const std::string& selectedOutput)
        : autoConnect_(autoConnect),
          selectedOutput_(selectedOutput)
    {
        for (auto& bend : pitchBends_)
            bend.store(0.0f, std::memory_order_relaxed);

        try {
            start();
        } catch (...) {
            stop();
            throw;
        }
    }

    static void onProcess(void* userdata, spa_io_position* position)
    {
        (void)position;

        auto* self = static_cast<MidiInputService*>(userdata);
        if (!self || !self->inputPort_)
            return;

        pw_buffer* pwBuffer =
            pw_filter_dequeue_buffer(self->inputPort_);

        if (!pwBuffer)
            return;

        self->processInputBuffer(pwBuffer);
        pw_filter_queue_buffer(self->inputPort_, pwBuffer);
    }

    static void onInputLinkCreated(void* userdata, pw_proxy* proxy)
    {
        auto* self = static_cast<MidiInputService*>(userdata);
        if (!self)
            return;

        self->linkRequest_ = nullptr;
        self->linkProxy_ = proxy;

        if (!proxy) {
            std::fprintf(stderr,
                "[MIDI Pitch Shifter] Automatic MIDI link creation failed "
                "for '%s'\n",
                self->selectedOutput_.c_str());
        }
    }

    static void onStateChanged(void* userdata,
                               enum pw_filter_state oldState,
                               enum pw_filter_state state,
                               const char* error)
    {
        (void)oldState;

        auto* self = static_cast<MidiInputService*>(userdata);
        if (!self)
            return;

        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] filter state %d -> %d; "
            "auto_connect=%s; output_selection=[%s]\n",
            static_cast<int>(oldState),
            static_cast<int>(state),
            self->autoConnect_ ? "enabled" : "disabled",
            self->selectedOutput_.c_str());

        if (state == PW_FILTER_STATE_PAUSED ||
            state == PW_FILTER_STATE_STREAMING) {
            if (self->autoConnect_ &&
                !self->linkProxy_ &&
                !self->linkRequest_) {
                self->linkRequest_ =
                    cm5audio::midi::createInputLinkAsync(
                        self->filter_,
                        self->selectedOutput_,
                        "midi_in",
                        &MidiInputService::onInputLinkCreated,
                        self);

                if (!self->linkRequest_) {
                    std::fprintf(stderr,
                        "[MIDI Pitch Shifter] Could not start automatic "
                        "MIDI link request for '%s'\n",
                        self->selectedOutput_.c_str());
                }
            }
        }

        if (state == PW_FILTER_STATE_UNCONNECTED ||
            state == PW_FILTER_STATE_ERROR) {
            self->clearPitchBends();
        }

        if (state == PW_FILTER_STATE_ERROR) {
            std::fprintf(stderr,
                "[MIDI Pitch Shifter] PipeWire filter error: %s\\n",
                error ? error : "unknown error");
        }
    }

    static const pw_filter_events& filterEvents() noexcept
    {
        static const pw_filter_events events = [] {
            pw_filter_events value{};
            value.version = PW_VERSION_FILTER_EVENTS;
            value.process = &MidiInputService::onProcess;
            value.state_changed = &MidiInputService::onStateChanged;
            return value;
        }();

        return events;
    }

    void start()
    {
        loop_ = pw_main_loop_new(nullptr);
        if (!loop_)
            throw std::runtime_error(
                "MIDI Pitch Shifter: pw_main_loop_new failed");

        pw_properties* nodeProperties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Midi",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_APP_NAME, "CM5Audio",
            PW_KEY_NODE_NAME, "cm5audio-midi-pitch-shifter",
            PW_KEY_NODE_DESCRIPTION, "CM5Audio MIDI Pitch Shifter",
            PW_KEY_NODE_AUTOCONNECT, "false",
            nullptr);

        if (!nodeProperties)
            throw std::runtime_error(
                "MIDI Pitch Shifter: pw_properties_new failed");

        filter_ = pw_filter_new_simple(
            pw_main_loop_get_loop(loop_),
            "CM5Audio MIDI Pitch Shifter",
            nodeProperties,
            &filterEvents(),
            this);

        if (!filter_)
            throw std::runtime_error(
                "MIDI Pitch Shifter: pw_filter_new_simple failed");

        pw_properties* portProperties = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit raw UMP",
            PW_KEY_PORT_NAME, "midi_in",
            nullptr);

        if (!portProperties)
            throw std::runtime_error(
                "MIDI Pitch Shifter: MIDI port properties allocation failed");

        inputPort_ = pw_filter_add_port(
            filter_,
            PW_DIRECTION_INPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(InputPortData),
            portProperties,
            nullptr,
            0);

        if (!inputPort_)
            throw std::runtime_error(
                "MIDI Pitch Shifter: pw_filter_add_port failed");

        static_cast<InputPortData*>(inputPort_)->owner = this;

        const int result = pw_filter_connect(
            filter_,
            PW_FILTER_FLAG_RT_PROCESS,
            nullptr,
            0);

        if (result < 0)
            throw std::runtime_error(
                "MIDI Pitch Shifter: pw_filter_connect failed");

        thread_ = std::thread([this] {
            pw_main_loop_run(loop_);
        });
    }

    void stop() noexcept
    {
        if (loop_)
            pw_main_loop_quit(loop_);

        if (thread_.joinable())
            thread_.join();

        cm5audio::midi::cancelInputLinkRequest(linkRequest_);

        if (filter_ && linkProxy_)
            cm5audio::midi::destroyInputLink(filter_, linkProxy_);

        if (filter_) {
            pw_filter_destroy(filter_);
            filter_ = nullptr;
        }

        inputPort_ = nullptr;
        linkProxy_ = nullptr;

        if (loop_) {
            pw_main_loop_destroy(loop_);
            loop_ = nullptr;
        }
    }

    void clearPitchBends() noexcept
    {
        for (auto& bend : pitchBends_)
            bend.store(0.0f, std::memory_order_release);
    }

    void setPitchBend(unsigned channel, float value) noexcept
    {
        if (channel >= MIDI_CHANNELS)
            return;

        value = std::max(-1.0f, std::min(1.0f, value));
        pitchBends_[channel].store(value, std::memory_order_release);
    }

    void handleMidi1(const uint8_t* bytes, size_t size) noexcept
    {
        if (!bytes || size < 3)
            return;

        const uint8_t status = bytes[0];

        if (status < 0x80 || status >= 0xf0)
            return;

        if ((status & 0xf0u) != 0xe0u)
            return;

        const unsigned channel = status & 0x0fu;

        const uint16_t value = static_cast<uint16_t>(
            ((static_cast<uint16_t>(bytes[2] & 0x7fu)) << 7u) |
            static_cast<uint16_t>(bytes[1] & 0x7fu));

        setPitchBend(channel, normalizeMidi1PitchBend(value));
    }

    void handleUmp(const uint32_t* words, size_t wordCount) noexcept
    {
        size_t index = 0;

        while (index < wordCount) {
            const uint32_t first = words[index];

            const uint8_t messageType =
                static_cast<uint8_t>(first >> 28u);

            const size_t packetWords = umpMessageWords(messageType);

            if (packetWords == 0 || packetWords > wordCount - index)
                break;

            if (messageType == 0x2 && packetWords == 1) {
                // MIDI 1.0 Channel Voice UMP.
                const uint8_t status =
                    static_cast<uint8_t>((first >> 16u) & 0xffu);

                if ((status & 0xf0u) == 0xe0u) {
                    const unsigned channel = status & 0x0fu;

                    const uint16_t value = static_cast<uint16_t>(
                        (((first >> 8u) & 0x7fu) << 7u) |
                        (first & 0x7fu));

                    setPitchBend(
                        channel,
                        normalizeMidi1PitchBend(value));
                }
            } else if (messageType == 0x4 && packetWords == 2) {
                // MIDI 2.0 Channel Voice UMP.
                const unsigned status = (first >> 20u) & 0x0fu;

                if (status == 0x0eu) {
                    const unsigned channel = (first >> 16u) & 0x0fu;
                    const uint32_t value = words[index + 1];

                    setPitchBend(
                        channel,
                        normalizeMidi2PitchBend(value));
                }
            }

            index += packetWords;
        }
    }

    void processInputBuffer(pw_buffer* pwBuffer) noexcept
    {
        if (!pwBuffer || !pwBuffer->buffer ||
            pwBuffer->buffer->n_datas < 1)
            return;

        spa_data* data = &pwBuffer->buffer->datas[0];
        if (!data->data || !data->chunk)
            return;

        const size_t offset = data->chunk->offset;
        const size_t size = data->chunk->size;

        if (offset > data->maxsize ||
            size > data->maxsize - offset ||
            size < sizeof(spa_pod_sequence))
            return;

        const auto* base =
            static_cast<const uint8_t*>(data->data) + offset;

        const auto* sequence =
            reinterpret_cast<const spa_pod_sequence*>(base);

        if (sequence->pod.type != SPA_TYPE_Sequence ||
            SPA_POD_SIZE(&sequence->pod) > size)
            return;

        spa_pod_control* control = nullptr;

        SPA_POD_SEQUENCE_FOREACH(sequence, control) {
            if (control->value.type != SPA_TYPE_Bytes)
                continue;

            const size_t payloadSize =
                SPA_POD_BODY_SIZE(&control->value);

            const auto* payload = static_cast<const uint8_t*>(
                SPA_POD_BODY(&control->value));

            if (!payload || payloadSize == 0)
                continue;

            if (control->type == SPA_CONTROL_UMP &&
                payloadSize % sizeof(uint32_t) == 0 &&
                payloadSize <= 1024) {
                std::array<uint32_t, 256> alignedWords{};
                std::memcpy(
                    alignedWords.data(),
                    payload,
                    payloadSize);

                handleUmp(
                    alignedWords.data(),
                    payloadSize / sizeof(uint32_t));
            } else if (control->type == SPA_CONTROL_Midi) {
                handleMidi1(payload, payloadSize);
            }
        }
    }
};

class MidiPitchShifterProcessor final : public AudioProcessor {
public:
    MidiPitchShifterProcessor(
        std::shared_ptr<MidiInputService> midi,
        unsigned listenChannel,
        float pitchRange)
        : midi_(std::move(midi)),
          listenChannel_(listenChannel - 1u),
          pitchRange_(pitchRange),
          delayLine_(
              static_cast<size_t>(MAX_AUDIO_CHANNELS) *
              DELAY_RING_SIZE,
              0.0f)
    {
        for (unsigned i = 0; i <= FADE_TABLE_SIZE; ++i) {
            const float phase =
                static_cast<float>(i) /
                static_cast<float>(FADE_TABLE_SIZE);

            fadeTable_[i] = 0.5f - 0.5f *
                std::cos(phase * 6.2831853071795864769f);
        }
    }

    void process(float* buffer,
                 size_t frames,
                 unsigned channels) override
    {
        if (!buffer || frames == 0 || channels == 0)
            return;

        const unsigned processChannels =
            std::min(channels, MAX_AUDIO_CHANNELS);

        if (processChannels == 0)
            return;

        const float bend =
            midi_->pitchBend(listenChannel_);

        const float targetSemitones = bend * pitchRange_;
        const float targetRatio = std::exp2(targetSemitones / 12.0f);
        const float targetWet =
            std::abs(targetSemitones) < 0.0001f ? 0.0f : 1.0f;

        for (size_t frame = 0; frame < frames; ++frame) {
            std::array<float, MAX_AUDIO_CHANNELS> input{};

            for (unsigned channel = 0;
                 channel < processChannels;
                 ++channel) {
                const size_t sampleIndex =
                    frame * channels + channel;

                input[channel] = buffer[sampleIndex];

                delayLine_[
                    static_cast<size_t>(channel) * DELAY_RING_SIZE +
                    writePosition_] = input[channel];
            }

            currentRatio_ +=
                (targetRatio - currentRatio_) * RATIO_SMOOTHING;

            pitchWet_ +=
                (targetWet - pitchWet_) * RATIO_SMOOTHING;

            phase_ +=
                (1.0f - currentRatio_) / DELAY_SPAN_SAMPLES;

            if (phase_ >= 1.0f)
                phase_ -= 1.0f;
            else if (phase_ < 0.0f)
                phase_ += 1.0f;

            float phaseB = phase_ + 0.5f;
            if (phaseB >= 1.0f)
                phaseB -= 1.0f;

            const float delayA =
                MIN_DELAY_SAMPLES + phase_ * DELAY_SPAN_SAMPLES;

            const float delayB =
                MIN_DELAY_SAMPLES + phaseB * DELAY_SPAN_SAMPLES;

            const float tablePosition =
                phase_ * static_cast<float>(FADE_TABLE_SIZE);

            const unsigned tableIndex =
                static_cast<unsigned>(tablePosition);

            const float tableFraction =
                tablePosition - static_cast<float>(tableIndex);

            const float fadeA =
                fadeTable_[tableIndex] +
                (fadeTable_[tableIndex + 1] -
                 fadeTable_[tableIndex]) * tableFraction;

            const float fadeB = 1.0f - fadeA;

            for (unsigned channel = 0;
                 channel < processChannels;
                 ++channel) {
                const float sampleA =
                    readDelay(channel, delayA);

                const float sampleB =
                    readDelay(channel, delayB);

                const float shifted =
                    sampleA * fadeA + sampleB * fadeB;

                buffer[frame * channels + channel] =
                    input[channel] * (1.0f - pitchWet_) +
                    shifted * pitchWet_;
            }

            writePosition_ =
                (writePosition_ + 1u) & DELAY_RING_MASK;
        }
    }

private:
    std::shared_ptr<MidiInputService> midi_;

    unsigned listenChannel_ = 9;
    float pitchRange_ = 2.0f;

    std::vector<float> delayLine_;
    std::array<float, FADE_TABLE_SIZE + 1> fadeTable_{};

    uint32_t writePosition_ = 0;
    float phase_ = 0.0f;
    float currentRatio_ = 1.0f;
    float pitchWet_ = 0.0f;

    float readDelay(unsigned channel, float delay) const noexcept
    {
        float position =
            static_cast<float>(writePosition_) - delay;

        if (position < 0.0f)
            position += static_cast<float>(DELAY_RING_SIZE);

        const uint32_t indexA =
            static_cast<uint32_t>(position);

        const uint32_t indexB =
            (indexA + 1u) & DELAY_RING_MASK;

        const float fraction =
            position - static_cast<float>(indexA);

        const size_t base =
            static_cast<size_t>(channel) * DELAY_RING_SIZE;

        const float a = delayLine_[base + indexA];
        const float b = delayLine_[base + indexB];

        return a + (b - a) * fraction;
    }
};

} // namespace

void register_midi_pitch_shifter_processor(ProcessorRegistry& registry)
{
    registry.register_processor({
        "midi_pitch_shifter",
        "MIDI Pitch Shifter",
        "MIDI-controlled audio pitch shifter. Supports MIDI 1.0 and "
        "MIDI 2.0 pitch bend using a dual-delay pitch shifter.",
        {
            {
                "listen_channel",
                "MIDI Listen Channel",
                "integer",
                10,
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
            },
            {
                "auto_connect",
                "Auto Connect",
                "enum",
                "disabled",
                {},
                {},
                {},
                "",
                {
                    {"Disabled", "disabled"},
                    {"Enabled", "enabled"}
                }
            },
            {
                "midi_output",
                "MIDI Output",
                "enum",
                "manual",
                {},
                {},
                {},
                "",
                makeMidiOutputChoices()
            }
        },
        [](const nlohmann::json& params)
            -> std::unique_ptr<AudioProcessor> {
            const unsigned channel =
                params.at("listen_channel").get<unsigned>();

            const float pitchRange =
                params.at("pitch_range").get<float>();

            const bool autoConnect =
                params.at("auto_connect").get<std::string>() ==
                "enabled";

            const std::string midiOutput =
                params.at("midi_output").get<std::string>();

            std::fprintf(stderr,
                "[CM5Audio MIDI AutoConnect] processor factory: "
                "auto_connect=%s; output_selection=[%s]\n",
                autoConnect ? "enabled" : "disabled",
                midiOutput.c_str());

            auto midi = MidiInputService::acquire(
                autoConnect,
                midiOutput);

            return std::make_unique<MidiPitchShifterProcessor>(
                std::move(midi),
                channel,
                pitchRange);
        }
    });
}
