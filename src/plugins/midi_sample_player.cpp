#include "audio_processor.h"
#include "processor_registry.h"
#include "midi_device_selector.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include <spa/buffer/buffer.h>
#include <spa/control/control.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr unsigned MIDI_CHANNELS = 16;
constexpr unsigned MIDI_NOTES = 128;
constexpr unsigned MAX_SAMPLE_VOICES = 64;
constexpr unsigned MAX_SAMPLE_CHANNELS = 8;
constexpr unsigned SAMPLE_ASSET_RATE = 48000;
constexpr uint64_t MAX_SAMPLE_BYTES = 512ull * 1024ull * 1024ull;

using Json = nlohmann::json;

static size_t umpMessageWords(uint8_t messageType) noexcept
{
    switch (messageType & 0x0fu) {
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

// This service exposes one PipeWire MIDI input and fans its raw MIDI 1.0/2.0
// events into the queues attached to MIDI input ports. Graph-connected MIDI
// events enter those same queues through DspEngine's normal MIDI routing.
class MidiEventInputService final {
public:
    static std::shared_ptr<MidiEventInputService> acquire(
        bool autoConnect, const std::string& selectedOutput)
    {
        static std::mutex cacheMutex;
        static std::map<std::string, std::weak_ptr<MidiEventInputService>> cache;
        const std::string key = autoConnect
            ? std::string("auto:") + selectedOutput
            : std::string("manual");

        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = cache.find(key);
        if (found != cache.end()) {
            if (auto existing = found->second.lock())
                return existing;
        }

        auto created = std::shared_ptr<MidiEventInputService>(
            new MidiEventInputService(autoConnect, selectedOutput));
        cache[key] = created;
        return created;
    }

    ~MidiEventInputService() { stop(); }

    MidiEventInputService(const MidiEventInputService&) = delete;
    MidiEventInputService& operator=(const MidiEventInputService&) = delete;

    void addEventQueue(const std::shared_ptr<MidiEventQueue>& queue)
    {
        if (!queue) return;
        std::lock_guard<std::mutex> lock(eventQueuesLock_);
        for (size_t index = 0; index < eventQueues_.size(); ++index) {
            auto current = std::atomic_load(&eventQueues_[index]);
            if (current == queue) {
                ++eventQueueRefs_[index];
                return;
            }
            if (!current) {
                std::atomic_store(&eventQueues_[index], queue);
                eventQueueRefs_[index] = 1;
                return;
            }
        }
        throw std::runtime_error("MIDI sample player input queue capacity exhausted");
    }

    void removeEventQueue(const std::shared_ptr<MidiEventQueue>& queue) noexcept
    {
        if (!queue) return;
        std::lock_guard<std::mutex> lock(eventQueuesLock_);
        for (size_t index = 0; index < eventQueues_.size(); ++index) {
            auto current = std::atomic_load(&eventQueues_[index]);
            if (current != queue) continue;
            if (eventQueueRefs_[index] > 1) {
                --eventQueueRefs_[index];
            } else {
                eventQueueRefs_[index] = 0;
                std::atomic_store(&eventQueues_[index], std::shared_ptr<MidiEventQueue>{});
            }
            return;
        }
    }

private:
    struct InputPortData {
        MidiEventInputService* owner = nullptr;
    };

    std::array<std::shared_ptr<MidiEventQueue>, 32> eventQueues_{};
    std::array<unsigned, 32> eventQueueRefs_{}; // Protected by eventQueuesLock_.
    std::mutex eventQueuesLock_;

    bool autoConnect_ = false;
    std::string selectedOutput_;
    pw_main_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    void* inputPort_ = nullptr;
    pw_proxy* linkProxy_ = nullptr;
    cm5audio::midi::InputLinkRequest* linkRequest_ = nullptr;
    std::thread thread_;

    MidiEventInputService(bool autoConnect, const std::string& selectedOutput)
        : autoConnect_(autoConnect), selectedOutput_(selectedOutput)
    {
        try {
            start();
        } catch (...) {
            stop();
            throw;
        }
    }

    void publishEvent(const MidiEvent& event) noexcept
    {
        for (auto& slot : eventQueues_) {
            auto queue = std::atomic_load(&slot);
            if (queue) queue->push(event);
        }
    }

    void publishMidi1(const uint8_t* bytes, size_t size) noexcept
    {
        if (!bytes || size == 0) return;
        MidiEvent event;
        event.protocol = 1;
        event.size = static_cast<uint8_t>(std::min(size, CM5_MIDI_MAX_EVENT_BYTES));
        std::memcpy(event.data, bytes, event.size);
        publishEvent(event);
    }

    void publishUmp(const uint32_t* words, size_t wordCount) noexcept
    {
        if (!words) return;
        size_t index = 0;
        while (index < wordCount) {
            const size_t packetWords = umpMessageWords(
                static_cast<uint8_t>(words[index] >> 28u));
            if (packetWords == 0 || packetWords > wordCount - index ||
                packetWords * sizeof(uint32_t) > CM5_MIDI_MAX_EVENT_BYTES)
                break;

            MidiEvent event;
            event.protocol = 2;
            event.size = static_cast<uint8_t>(packetWords * sizeof(uint32_t));
            std::memcpy(event.data, words + index, event.size);
            publishEvent(event);
            index += packetWords;
        }
    }

    static void onProcess(void* userdata, spa_io_position* position)
    {
        (void)position;
        auto* self = static_cast<MidiEventInputService*>(userdata);
        if (!self || !self->inputPort_) return;

        pw_buffer* pwBuffer = pw_filter_dequeue_buffer(self->inputPort_);
        if (!pwBuffer) return;
        self->processInputBuffer(pwBuffer);
        pw_filter_queue_buffer(self->inputPort_, pwBuffer);
    }

    static void onInputLinkCreated(void* userdata, pw_proxy* proxy)
    {
        auto* self = static_cast<MidiEventInputService*>(userdata);
        if (!self) return;
        self->linkRequest_ = nullptr;
        self->linkProxy_ = proxy;
        if (!proxy) {
            std::fprintf(stderr,
                "[CM5Audio MIDI Sample Player] Automatic MIDI link creation failed for '%s'\n",
                self->selectedOutput_.c_str());
        }
    }

    static void onStateChanged(void* userdata,
                               enum pw_filter_state oldState,
                               enum pw_filter_state state,
                               const char* error)
    {
        (void)oldState;
        auto* self = static_cast<MidiEventInputService*>(userdata);
        if (!self) return;

        std::fprintf(stderr,
            "[CM5Audio MIDI Sample Player] PipeWire filter state %d -> %d; "
            "auto_connect=%s; output_selection=[%s]\n",
            static_cast<int>(oldState), static_cast<int>(state),
            self->autoConnect_ ? "enabled" : "disabled",
            self->selectedOutput_.c_str());

        if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) {
            if (self->autoConnect_ && !self->linkProxy_ && !self->linkRequest_) {
                self->linkRequest_ = cm5audio::midi::createInputLinkAsync(
                    self->filter_, self->selectedOutput_, "midi_in",
                    &MidiEventInputService::onInputLinkCreated, self);
                if (!self->linkRequest_) {
                    std::fprintf(stderr,
                        "[CM5Audio MIDI Sample Player] Could not start automatic MIDI link request for '%s'\n",
                        self->selectedOutput_.c_str());
                }
            }
        }

        if (state == PW_FILTER_STATE_ERROR) {
            std::fprintf(stderr,
                "[CM5Audio MIDI Sample Player] PipeWire filter error: %s\n",
                error ? error : "unknown error");
        }
    }

    static const pw_filter_events& filterEvents() noexcept
    {
        static const pw_filter_events events = [] {
            pw_filter_events value{};
            value.version = PW_VERSION_FILTER_EVENTS;
            value.process = &MidiEventInputService::onProcess;
            value.state_changed = &MidiEventInputService::onStateChanged;
            return value;
        }();
        return events;
    }

    void start()
    {
        loop_ = pw_main_loop_new(nullptr);
        if (!loop_)
            throw std::runtime_error("MIDI Sample Player: pw_main_loop_new failed");

        pw_properties* nodeProperties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Midi",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_APP_NAME, "CM5Audio",
            PW_KEY_NODE_NAME, "cm5audio-midi-sample-player",
            PW_KEY_NODE_DESCRIPTION, "CM5Audio MIDI Sample Player",
            PW_KEY_NODE_AUTOCONNECT, "false",
            nullptr);
        if (!nodeProperties)
            throw std::runtime_error("MIDI Sample Player: PipeWire node properties allocation failed");

        filter_ = pw_filter_new_simple(
            pw_main_loop_get_loop(loop_),
            "CM5Audio MIDI Sample Player",
            nodeProperties,
            &filterEvents(),
            this);
        if (!filter_)
            throw std::runtime_error("MIDI Sample Player: pw_filter_new_simple failed");

        pw_properties* portProperties = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit raw UMP",
            PW_KEY_PORT_NAME, "midi_in",
            nullptr);
        if (!portProperties)
            throw std::runtime_error("MIDI Sample Player: MIDI port properties allocation failed");

        inputPort_ = pw_filter_add_port(
            filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(InputPortData), portProperties, nullptr, 0);
        if (!inputPort_)
            throw std::runtime_error("MIDI Sample Player: pw_filter_add_port failed");

        static_cast<InputPortData*>(inputPort_)->owner = this;
        const int result = pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
        if (result < 0)
            throw std::runtime_error("MIDI Sample Player: pw_filter_connect failed");

        thread_ = std::thread([this] { pw_main_loop_run(loop_); });
    }

    void stop() noexcept
    {
        if (loop_) pw_main_loop_quit(loop_);
        if (thread_.joinable()) thread_.join();

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

    void processInputBuffer(pw_buffer* pwBuffer) noexcept
    {
        if (!pwBuffer || !pwBuffer->buffer || pwBuffer->buffer->n_datas < 1)
            return;
        spa_data* data = &pwBuffer->buffer->datas[0];
        if (!data->data || !data->chunk) return;

        const size_t offset = data->chunk->offset;
        const size_t size = data->chunk->size;
        if (offset > data->maxsize || size > data->maxsize - offset ||
            size < sizeof(spa_pod_sequence))
            return;

        const auto* base = static_cast<const uint8_t*>(data->data) + offset;
        const auto* sequence = reinterpret_cast<const spa_pod_sequence*>(base);
        if (sequence->pod.type != SPA_TYPE_Sequence ||
            SPA_POD_SIZE(&sequence->pod) > size)
            return;

        spa_pod_control* control = nullptr;
        SPA_POD_SEQUENCE_FOREACH(sequence, control) {
            if (control->value.type != SPA_TYPE_Bytes) continue;
            const size_t payloadSize = SPA_POD_BODY_SIZE(&control->value);
            const auto* payload = static_cast<const uint8_t*>(SPA_POD_BODY(&control->value));
            if (!payload || payloadSize == 0) continue;

            if (control->type == SPA_CONTROL_UMP &&
                payloadSize % sizeof(uint32_t) == 0 && payloadSize <= 1024) {
                std::array<uint32_t, 256> alignedWords{};
                std::memcpy(alignedWords.data(), payload, payloadSize);
                publishUmp(alignedWords.data(), payloadSize / sizeof(uint32_t));
            } else if (control->type == SPA_CONTROL_Midi) {
                publishMidi1(payload, payloadSize);
            }
        }
    }
};

struct SampleData {
    std::vector<float> interleaved;
    unsigned channels = 0;
    unsigned sampleRate = SAMPLE_ASSET_RATE;
    uint64_t startFrame = 0;
    uint64_t frames = 0;
};

static std::filesystem::path assetRootPath()
{
    const char* configured = std::getenv("CM5AUDIO_ASSET_DIR");
    return configured && *configured
        ? std::filesystem::path(configured)
        : std::filesystem::path("./assets");
}

static bool validAssetHash(const std::string& hash)
{
    if (hash.size() != 64) return false;
    return std::all_of(hash.begin(), hash.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

static std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Accept the exact content hash or a unique display/original filename from the
// existing Audio Assets library. Matching stays on the control thread.
static std::string resolveAssetHash(const std::string& requested)
{
    const std::string selector = trim(requested);
    if (selector.empty()) return {};

    const auto root = assetRootPath();
    if (validAssetHash(selector)) {
        if (!std::filesystem::is_regular_file(root / selector / "metadata.json"))
            throw std::invalid_argument("MIDI Sample Player: audio asset SHA-256 was not found in the Audio Assets library");
        return selector;
    }

    if (!std::filesystem::exists(root))
        throw std::invalid_argument("MIDI Sample Player: no Audio Assets directory exists; upload a sample in the dashboard first");

    std::string match;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        const std::string hash = entry.path().filename().string();
        if (!validAssetHash(hash)) continue;

        const auto metadataPath = entry.path() / "metadata.json";
        if (!std::filesystem::is_regular_file(metadataPath)) continue;

        try {
            std::ifstream file(metadataPath, std::ios::binary);
            if (!file) continue;
            Json metadata;
            file >> metadata;
            const std::string displayName = metadata.value("display_name", std::string());
            const std::string originalName = metadata.value("original_filename", std::string());
            if (selector != displayName && selector != originalName) continue;

            if (!match.empty()) {
                throw std::invalid_argument(
                    "MIDI Sample Player: more than one audio asset matches '" + selector +
                    "'; use the asset's full SHA-256 identifier instead");
            }
            match = hash;
        } catch (const nlohmann::json::exception&) {
            // Ignore incomplete or corrupt asset metadata while scanning.
        }
    }

    if (match.empty())
        throw std::invalid_argument(
            "MIDI Sample Player: audio asset '" + selector +
            "' was not found; enter its exact filename, display name, or SHA-256 identifier");
    return match;
}

static std::shared_ptr<const SampleData> loadSampleAsset(const std::string& requested)
{
    const std::string hash = resolveAssetHash(requested);
    if (hash.empty()) return {};

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::weak_ptr<const SampleData>> cache;
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto found = cache.find(hash); found != cache.end()) {
        if (auto existing = found->second.lock()) return existing;
    }

    const auto directory = assetRootPath() / hash;
    Json metadata;
    {
        std::ifstream file(directory / "metadata.json", std::ios::binary);
        if (!file) throw std::runtime_error("MIDI Sample Player: cannot read audio asset metadata");
        file >> metadata;
    }

    const unsigned channels = metadata.value("channels", 0u);
    const unsigned sampleRate = metadata.value("sample_rate", 0u);
    const uint64_t frames = metadata.value("frames", uint64_t{0});
    if (channels == 0 || channels > MAX_SAMPLE_CHANNELS ||
        sampleRate != SAMPLE_ASSET_RATE || frames == 0)
        throw std::runtime_error("MIDI Sample Player: asset metadata must describe 48 kHz audio with 1–8 channels and at least one frame");

    if (frames > std::numeric_limits<uint64_t>::max() / channels / sizeof(float))
        throw std::runtime_error("MIDI Sample Player: audio asset dimensions overflow");
    const uint64_t byteCount = frames * static_cast<uint64_t>(channels) * sizeof(float);
    if (byteCount > MAX_SAMPLE_BYTES || byteCount > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("MIDI Sample Player: audio asset exceeds the 512 MiB in-memory sample limit");

    std::error_code error;
    const uint64_t actualBytes = std::filesystem::file_size(directory / "audio.f32", error);
    if (error || actualBytes != byteCount)
        throw std::runtime_error("MIDI Sample Player: converted F32 sample data is missing or has an invalid size");

    auto sample = std::make_shared<SampleData>();
    sample->channels = channels;
    sample->sampleRate = sampleRate;
    sample->frames = frames;
    sample->interleaved.resize(static_cast<size_t>(frames) * channels);

    std::ifstream file(directory / "audio.f32", std::ios::binary);
    if (!file || !file.read(reinterpret_cast<char*>(sample->interleaved.data()),
                            static_cast<std::streamsize>(byteCount)))
        throw std::runtime_error("MIDI Sample Player: unable to read converted F32 sample data");

    // Treat anything below -60 dBFS as leading silence. This catches digital
    // silence and very low-level pre-roll/noise without trimming normal fades.
    constexpr float LEADING_SILENCE_THRESHOLD = 0.001f;
    uint64_t firstActiveFrame = frames;
    for (size_t index = 0; index < sample->interleaved.size(); ++index) {
        const float value = sample->interleaved[index];
        if (!std::isfinite(value) || std::abs(value) > 32.0f)
            throw std::runtime_error("MIDI Sample Player: audio asset contains invalid F32 samples");

        if (firstActiveFrame == frames && std::abs(value) > LEADING_SILENCE_THRESHOLD)
            firstActiveFrame = static_cast<uint64_t>(index / channels);
    }

    if (firstActiveFrame > 0 && firstActiveFrame < frames) {
        sample->startFrame = firstActiveFrame;
        sample->frames = frames - firstActiveFrame;
        const double skippedMs = static_cast<double>(firstActiveFrame) *
            1000.0 / static_cast<double>(sampleRate);
        std::fprintf(stderr,
            "[CM5Audio MIDI Sample Player] Trimmed %.1f ms of leading silence "
            "(threshold -60 dBFS)\n",
            skippedMs);
    }

    std::shared_ptr<const SampleData> immutable = sample;
    cache[hash] = immutable;
    return immutable;
}

class MidiSamplePlayerProcessor final : public AudioProcessor {
public:
    MidiSamplePlayerProcessor(
        std::shared_ptr<const SampleData> sample,
        std::shared_ptr<MidiEventInputService> midi,
        unsigned midiChannel,
        unsigned rootNote,
        unsigned maxVoices,
        float gain,
        bool loop,
        float attackMs,
        float releaseMs)
        : sample_(std::move(sample)),
          midi_(std::move(midi)),
          midiChannel_(midiChannel),
          rootNote_(rootNote),
          maxVoices_(maxVoices),
          gain_(gain),
          loop_(loop),
          attackMs_(attackMs),
          releaseMs_(releaseMs)
    {
        for (unsigned note = 0; note < MIDI_NOTES; ++note) {
            pitchRatio_[note] = std::pow(
                2.0, (static_cast<double>(note) - static_cast<double>(rootNote_)) / 12.0);
        }
    }

    ~MidiSamplePlayerProcessor() override
    {
        if (graphMidiQueue_ && midi_)
            midi_->removeEventQueue(graphMidiQueue_);
    }

    void attach_midi_input(const std::string& portId,
                           const std::shared_ptr<MidiEventQueue>& queue) override
    {
        if (portId != "midi_in" || !queue || queue == graphMidiQueue_) return;
        if (graphMidiQueue_) midi_->removeEventQueue(graphMidiQueue_);
        graphMidiQueue_ = queue;
        midi_->addEventQueue(graphMidiQueue_);
    }

    void process(AudioProcessContext& context) override
    {
        AudioBusBuffer* output = nullptr;
        for (size_t index = 0; index < context.audio_output_count; ++index) {
            if (context.audio_outputs[index].port_id &&
                std::strcmp(context.audio_outputs[index].port_id, "audio_out") == 0) {
                output = &context.audio_outputs[index];
                break;
            }
        }
        if (!output && context.audio_output_count > 0)
            output = &context.audio_outputs[0];
        if (!output || !output->output || output->channels == 0 || context.frames == 0)
            return;

        const size_t outputSamples = context.frames * output->channels;
        std::fill_n(output->output, outputSamples, 0.0f);

        const double outputRate = context.sample_rate > 0.0
            ? context.sample_rate : static_cast<double>(SAMPLE_ASSET_RATE);
        for (size_t inputIndex = 0; inputIndex < context.midi_input_count; ++inputIndex) {
            const MidiInputBuffer& input = context.midi_inputs[inputIndex];
            if (!input.events) continue;
            for (size_t eventIndex = 0; eventIndex < input.event_count; ++eventIndex)
                applyMidiEvent(input.events[eventIndex], outputRate);
        }

        if (!sample_ || sample_->frames == 0 || sample_->channels == 0)
            return;

        const double rateRatio = static_cast<double>(sample_->sampleRate) / outputRate;
        const unsigned activeVoiceLimit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
        const unsigned outputChannels = std::min(output->channels, MAX_SAMPLE_CHANNELS);

        for (size_t frame = 0; frame < context.frames; ++frame) {
            for (unsigned voiceIndex = 0; voiceIndex < activeVoiceLimit; ++voiceIndex) {
                Voice& voice = voices_[voiceIndex];
                if (!voice.active) continue;

                if (voice.position >= static_cast<double>(sample_->frames)) {
                    if (loop_) {
                        voice.position = std::fmod(
                            voice.position, static_cast<double>(sample_->frames));
                    } else {
                        voice.active = false;
                        continue;
                    }
                }

                const uint64_t firstFrame = static_cast<uint64_t>(voice.position);
                uint64_t secondFrame = firstFrame + 1;
                if (secondFrame >= sample_->frames)
                    secondFrame = loop_ ? 0 : firstFrame;
                const float fraction = static_cast<float>(
                    voice.position - static_cast<double>(firstFrame));

                float envelope = voice.attackRemaining > 0
                    ? 1.0f - static_cast<float>(voice.attackRemaining) /
                                  static_cast<float>(voice.attackTotal)
                    : 1.0f;
                if (voice.attackRemaining > 0) --voice.attackRemaining;

                if (voice.releasing) {
                    if (voice.releaseRemaining == 0) {
                        voice.active = false;
                        continue;
                    }
                    envelope *= static_cast<float>(voice.releaseRemaining) /
                                static_cast<float>(voice.releaseTotal);
                }

                const float voiceGain = gain_ * voice.velocity * envelope;
                if (voiceGain != 0.0f) {
                    for (unsigned channel = 0; channel < outputChannels; ++channel) {
                        const float sampleValue = readSample(
                            firstFrame, secondFrame, fraction,
                            channel, outputChannels);
                        output->output[frame * output->channels + channel] +=
                            sampleValue * voiceGain;
                    }
                }

                voice.position += voice.pitchRatio * rateRatio;
                if (voice.releasing) {
                    --voice.releaseRemaining;
                    if (voice.releaseRemaining == 0)
                        voice.active = false;
                } else if (!loop_ && voice.position >= static_cast<double>(sample_->frames)) {
                    voice.active = false;
                }
            }
        }
    }

private:
    struct Voice {
        bool active = false;
        bool keyDown = false;
        bool releasing = false;
        unsigned channel = 0;
        unsigned note = 0;
        double position = 0.0;
        double pitchRatio = 1.0;
        float velocity = 1.0f;
        uint64_t age = 0;
        uint64_t attackRemaining = 0;
        uint64_t attackTotal = 0;
        uint64_t releaseRemaining = 0;
        uint64_t releaseTotal = 0;
    };

    std::shared_ptr<const SampleData> sample_;
    std::shared_ptr<MidiEventInputService> midi_;
    std::shared_ptr<MidiEventQueue> graphMidiQueue_;
    unsigned midiChannel_ = 0; // 0 means all channels; otherwise MIDI 1–16.
    unsigned rootNote_ = 60;
    unsigned maxVoices_ = 16;
    float gain_ = 1.0f;
    bool loop_ = false;
    float attackMs_ = 2.0f;
    float releaseMs_ = 80.0f;
    std::array<double, MIDI_NOTES> pitchRatio_{};
    std::array<Voice, MAX_SAMPLE_VOICES> voices_{};
    std::array<bool, MIDI_CHANNELS> sustain_{};
    uint64_t nextVoiceAge_ = 1;

    bool acceptsChannel(unsigned channel) const noexcept
    {
        return channel < MIDI_CHANNELS &&
               (midiChannel_ == 0 || midiChannel_ == channel + 1);
    }

    void beginRelease(Voice& voice, double sampleRate) noexcept
    {
        if (!voice.active || voice.releasing) return;
        voice.releasing = true;
        const double requested = std::max(0.0, static_cast<double>(releaseMs_)) *
                                 0.001 * sampleRate;
        voice.releaseTotal = static_cast<uint64_t>(std::llround(requested));
        voice.releaseRemaining = voice.releaseTotal;
        if (voice.releaseRemaining == 0)
            voice.active = false;
    }

    void noteOn(unsigned channel, unsigned note, float velocity,
                double sampleRate) noexcept
    {
        if (!sample_ || !acceptsChannel(channel) || note >= MIDI_NOTES || velocity <= 0.0f)
            return;

        const unsigned limit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
        Voice* selected = nullptr;
        for (unsigned index = 0; index < limit; ++index) {
            if (!voices_[index].active) {
                selected = &voices_[index];
                break;
            }
        }

        if (!selected) {
            // Prefer stealing an already-releasing voice; otherwise steal the
            // oldest active voice. All storage was allocated before processing.
            for (unsigned index = 0; index < limit; ++index) {
                Voice& candidate = voices_[index];
                if (!selected ||
                    (candidate.releasing && !selected->releasing) ||
                    (candidate.releasing == selected->releasing && candidate.age < selected->age))
                    selected = &candidate;
            }
        }
        if (!selected) return;

        Voice& voice = *selected;
        voice = Voice{};
        voice.active = true;
        voice.keyDown = true;
        voice.channel = channel;
        voice.note = note;
        voice.position = 0.0;
        voice.pitchRatio = pitchRatio_[note];
        voice.velocity = std::max(0.0f, std::min(1.0f, velocity));
        voice.age = nextVoiceAge_++;
        const double attackSamples = std::max(0.0, static_cast<double>(attackMs_)) *
                                     0.001 * sampleRate;
        voice.attackTotal = static_cast<uint64_t>(std::llround(attackSamples));
        voice.attackRemaining = voice.attackTotal;
    }

    void noteOff(unsigned channel, unsigned note, double sampleRate) noexcept
    {
        if (!acceptsChannel(channel) || note >= MIDI_NOTES) return;

        // A repeated same-pitch note is released one voice at a time, oldest first.
        Voice* selected = nullptr;
        const unsigned limit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
        for (unsigned index = 0; index < limit; ++index) {
            Voice& voice = voices_[index];
            if (!voice.active || !voice.keyDown ||
                voice.channel != channel || voice.note != note)
                continue;
            if (!selected || voice.age < selected->age)
                selected = &voice;
        }
        if (!selected) return;

        selected->keyDown = false;
        if (sustain_[channel]) return;
        beginRelease(*selected, sampleRate);
    }

    void controlChange(unsigned channel, unsigned controller, unsigned value,
                       double sampleRate) noexcept
    {
        if (!acceptsChannel(channel)) return;
        if (controller == 64) {
            const bool down = value >= 64;
            if (sustain_[channel] && !down) {
                const unsigned limit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
                for (unsigned index = 0; index < limit; ++index) {
                    Voice& voice = voices_[index];
                    if (voice.active && voice.channel == channel && !voice.keyDown)
                        beginRelease(voice, sampleRate);
                }
            }
            sustain_[channel] = down;
        } else if (controller == 120) {
            // All Sound Off: immediately silence every voice on this channel.
            const unsigned limit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
            for (unsigned index = 0; index < limit; ++index) {
                if (voices_[index].active && voices_[index].channel == channel)
                    voices_[index].active = false;
            }
        } else if (controller == 123) {
            // All Notes Off: respect the sustain pedal, just like individual notes.
            const unsigned limit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
            for (unsigned index = 0; index < limit; ++index) {
                Voice& voice = voices_[index];
                if (!voice.active || voice.channel != channel || !voice.keyDown) continue;
                voice.keyDown = false;
                if (!sustain_[channel]) beginRelease(voice, sampleRate);
            }
        } else if (controller == 121) {
            // Reset All Controllers also releases notes held only by sustain.
            sustain_[channel] = false;
            const unsigned limit = std::min(maxVoices_, MAX_SAMPLE_VOICES);
            for (unsigned index = 0; index < limit; ++index) {
                Voice& voice = voices_[index];
                if (voice.active && voice.channel == channel && !voice.keyDown)
                    beginRelease(voice, sampleRate);
            }
        }
    }

    void handleMidi1Message(uint8_t status, uint8_t data1, uint8_t data2,
                            double sampleRate) noexcept
    {
        if (status < 0x80u || status >= 0xf0u) return;
        const unsigned channel = status & 0x0fu;
        const uint8_t command = status & 0xf0u;
        const unsigned first = data1 & 0x7fu;
        const unsigned second = data2 & 0x7fu;

        if (command == 0x80u) {
            noteOff(channel, first, sampleRate);
        } else if (command == 0x90u) {
            if (second == 0) noteOff(channel, first, sampleRate);
            else noteOn(channel, first, static_cast<float>(second) / 127.0f, sampleRate);
        } else if (command == 0xb0u) {
            controlChange(channel, first, second, sampleRate);
        }
    }

    void handleUmpMessage(const uint32_t* words, size_t wordCount,
                          double sampleRate) noexcept
    {
        size_t index = 0;
        while (index < wordCount) {
            const uint32_t first = words[index];
            const uint8_t messageType = static_cast<uint8_t>(first >> 28u);
            const size_t packetWords = umpMessageWords(messageType);
            if (packetWords == 0 || packetWords > wordCount - index) break;

            if (messageType == 0x2 && packetWords == 1) {
                const uint8_t status = static_cast<uint8_t>((first >> 16u) & 0xffu);
                const uint8_t data1 = static_cast<uint8_t>((first >> 8u) & 0x7fu);
                const uint8_t data2 = static_cast<uint8_t>(first & 0x7fu);
                handleMidi1Message(status, data1, data2, sampleRate);
            } else if (messageType == 0x4 && packetWords == 2) {
                const unsigned status = (first >> 20u) & 0x0fu;
                const unsigned channel = (first >> 16u) & 0x0fu;
                const unsigned data1 = (first >> 8u) & 0x7fu;
                const uint32_t data2 = words[index + 1];
                if (status == 0x8u) {
                    noteOff(channel, data1, sampleRate);
                } else if (status == 0x9u) {
                    const uint16_t velocity = static_cast<uint16_t>(data2 >> 16u);
                    if (velocity == 0) noteOff(channel, data1, sampleRate);
                    else noteOn(channel, data1,
                                static_cast<float>(velocity) / 65535.0f,
                                sampleRate);
                } else if (status == 0xbu) {
                    const uint32_t value = data2;
                    const unsigned normalized = value >= 0x80000000u ? 127u :
                        static_cast<unsigned>((static_cast<uint64_t>(value) * 127u) / 0xffffffffu);
                    controlChange(channel, data1, normalized, sampleRate);
                }
            }
            index += packetWords;
        }
    }

    void applyMidiEvent(const MidiEvent& event, double sampleRate) noexcept
    {
        if (event.size == 0 || event.size > CM5_MIDI_MAX_EVENT_BYTES) return;
        if (event.protocol == 1) {
            if (event.size >= 3)
                handleMidi1Message(event.data[0], event.data[1], event.data[2], sampleRate);
        } else if (event.protocol == 2 && event.size % sizeof(uint32_t) == 0) {
            std::array<uint32_t, 4> words{};
            const size_t count = event.size / sizeof(uint32_t);
            if (count <= words.size()) {
                std::memcpy(words.data(), event.data, event.size);
                handleUmpMessage(words.data(), count, sampleRate);
            }
        }
    }

    float readSample(uint64_t firstFrame, uint64_t secondFrame,
                     float fraction, unsigned outputChannel,
                     unsigned outputChannels) const noexcept
    {
        const unsigned sourceChannels = sample_->channels;
        const auto interpolate = [&](unsigned sourceChannel) noexcept {
            const size_t first = static_cast<size_t>(sample_->startFrame + firstFrame) *
                sourceChannels + sourceChannel;
            const size_t second = static_cast<size_t>(sample_->startFrame + secondFrame) *
                sourceChannels + sourceChannel;
            const float a = sample_->interleaved[first];
            const float b = sample_->interleaved[second];
            return a + (b - a) * fraction;
        };

        if (sourceChannels == 1)
            return interpolate(0);
        if (outputChannels >= sourceChannels)
            return interpolate(outputChannel % sourceChannels);

        // Downmix evenly when the selected graph output has fewer channels
        // than the uploaded sample. Stereo-to-mono is a simple L/R average.
        const unsigned begin = outputChannel * sourceChannels / outputChannels;
        const unsigned end = (outputChannel + 1) * sourceChannels / outputChannels;
        float sum = 0.0f;
        unsigned count = 0;
        for (unsigned channel = begin; channel < end; ++channel) {
            sum += interpolate(channel);
            ++count;
        }
        return count ? sum / static_cast<float>(count) : 0.0f;
    }
};

} // namespace

void register_midi_sample_player_processor(ProcessorRegistry& registry)
{
    ProcessorDescriptor descriptor{
        "midi_sample_player",
        "MIDI Sample Player",
        "Polyphonic MIDI sample instrument. Plays an uploaded Audio Asset at "
        "different semitones according to MIDI note numbers, with velocity, "
        "looping, sustain pedal, and note release support.",
        {
            {"sample_asset", "Sample Asset (filename, display name, or SHA-256)",
             "string", "", {}, {}, {},
             "Upload audio through the dashboard's Audio Assets manager first.", {}},
            {"midi_channel", "MIDI Channel", "integer", 0, 0.0, 16.0, 1.0,
             "0 = Omni; 1–16 = specific channel", {}},
            {"root_note", "Root Note", "integer", 60, 0.0, 127.0, 1.0,
             "MIDI note number played at the original sample pitch", {}},
            {"max_voices", "Maximum Voices", "integer", 16, 1.0,
             static_cast<double>(MAX_SAMPLE_VOICES), 1.0, "voices", {}},
            {"gain", "Output Gain", "number", 1.0, 0.0, 8.0, 0.01, "", {}},
            {"loop", "Loop Sample", "boolean", false, {}, {}, {}, "", {}},
            {"attack_ms", "Attack", "number", 2.0, 0.0, 500.0, 1.0, "ms", {}},
            {"release_ms", "Release", "number", 80.0, 0.0, 5000.0, 1.0, "ms", {}},
            {"auto_connect", "Auto Connect MIDI", "enum", "disabled", {}, {}, {}, "",
             {{"Disabled (manual PipeWire/graph routing)", "disabled"},
              {"Enabled", "enabled"}}},
            {"midi_output", "MIDI Input Source", "enum", "manual", {}, {}, {}, "",
             makeMidiOutputChoices()}
        },
        [](const Json& params) -> std::unique_ptr<AudioProcessor> {
            const std::string sampleAsset = params.at("sample_asset").get<std::string>();
            const auto sample = loadSampleAsset(sampleAsset);
            const unsigned midiChannel = params.at("midi_channel").get<unsigned>();
            const unsigned rootNote = params.at("root_note").get<unsigned>();
            const unsigned maxVoices = params.at("max_voices").get<unsigned>();
            const float gain = params.at("gain").get<float>();
            const bool loop = params.at("loop").get<bool>();
            const float attackMs = params.at("attack_ms").get<float>();
            const float releaseMs = params.at("release_ms").get<float>();
            const bool autoConnect = params.at("auto_connect").get<std::string>() == "enabled";
            const std::string midiOutput = params.at("midi_output").get<std::string>();

            auto midi = MidiEventInputService::acquire(autoConnect, midiOutput);
            return std::make_unique<MidiSamplePlayerProcessor>(
                sample, std::move(midi), midiChannel, rootNote, maxVoices,
                gain, loop, attackMs, releaseMs);
        }
    };

    descriptor.ports = {
        {"midi_in", "MIDI Input", ProcessorPortType::MIDI,
         ProcessorPortDirection::INPUT, ProcessorChannelLayout{}, "midi_output"},
        {"audio_out", "Audio Output", ProcessorPortType::AUDIO,
         ProcessorPortDirection::OUTPUT, ProcessorChannelLayout{1, 8, 2}, {}}
    };
    descriptor.chain_channel_mode = ProcessorChainChannelMode::MONO_ONLY;
    registry.register_processor(std::move(descriptor));
}
