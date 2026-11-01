#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

constexpr size_t CM5_MIDI_MAX_EVENT_BYTES = 16;
constexpr size_t CM5_MIDI_MAX_EVENTS_PER_BLOCK = 256;
constexpr size_t CM5_MIDI_EVENT_QUEUE_CAPACITY = 1024;

// One normalized MIDI event. MIDI 1.0 messages use protocol=1; UMP packets
// use protocol=2. The byte array preserves the original wire representation.
struct MidiEvent {
    uint64_t time = 0;
    uint8_t protocol = 1;
    uint8_t size = 0;
    uint8_t data[CM5_MIDI_MAX_EVENT_BYTES]{};
};

// Bounded multi-producer/single-consumer queue. PipeWire MIDI capture and
// graph-generated MIDI may both enqueue events; the DSP worker is the consumer.
// The sequence-number algorithm avoids locks and allocations on either thread.
class MidiEventQueue {
public:
    MidiEventQueue() noexcept {
        for (size_t i = 0; i < CM5_MIDI_EVENT_QUEUE_CAPACITY; ++i)
            m_cells[i].sequence.store(i, std::memory_order_relaxed);
    }

    MidiEventQueue(const MidiEventQueue&) = delete;
    MidiEventQueue& operator=(const MidiEventQueue&) = delete;

    bool push(const MidiEvent& event) noexcept {
        if (event.size == 0 || event.size > CM5_MIDI_MAX_EVENT_BYTES)
            return false;

        Cell* cell = nullptr;
        size_t position = m_enqueue.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_cells[position & (CM5_MIDI_EVENT_QUEUE_CAPACITY - 1)];
            const size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) -
                                        static_cast<intptr_t>(position);
            if (difference == 0) {
                if (m_enqueue.compare_exchange_weak(position, position + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            } else if (difference < 0) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                position = m_enqueue.load(std::memory_order_relaxed);
            }
        }

        cell->event = event;
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    bool pop(MidiEvent& event) noexcept {
        const size_t position = m_dequeue.load(std::memory_order_relaxed);
        Cell& cell = m_cells[position & (CM5_MIDI_EVENT_QUEUE_CAPACITY - 1)];
        const size_t sequence = cell.sequence.load(std::memory_order_acquire);
        const intptr_t difference = static_cast<intptr_t>(sequence) -
                                    static_cast<intptr_t>(position + 1);
        if (difference != 0)
            return false;

        event = cell.event;
        cell.sequence.store(position + CM5_MIDI_EVENT_QUEUE_CAPACITY,
                            std::memory_order_release);
        m_dequeue.store(position + 1, std::memory_order_relaxed);
        return true;
    }

    uint64_t dropped() const noexcept {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    struct Cell {
        std::atomic<size_t> sequence{0};
        MidiEvent event{};
    };

    static_assert((CM5_MIDI_EVENT_QUEUE_CAPACITY &
                   (CM5_MIDI_EVENT_QUEUE_CAPACITY - 1)) == 0,
                  "MIDI event queue capacity must be a power of two");

    std::array<Cell, CM5_MIDI_EVENT_QUEUE_CAPACITY> m_cells{};
    alignas(64) std::atomic<size_t> m_enqueue{0};
    alignas(64) std::atomic<size_t> m_dequeue{0};
    std::atomic<uint64_t> m_dropped{0};
};

// Audio buffers are interleaved float32. A bus has either an input pointer,
// an output pointer, or both when a processor is doing in-place bus processing.
struct AudioBusBuffer {
    const char* port_id = nullptr;
    const float* input = nullptr;
    float* output = nullptr;
    unsigned channels = 0;
};

struct MidiInputBuffer {
    const char* port_id = nullptr;
    const MidiEvent* events = nullptr;
    size_t event_count = 0;
};

struct MidiOutputBuffer {
    const char* port_id = nullptr;
    MidiEvent* events = nullptr;
    size_t event_count = 0;
    size_t event_capacity = 0;

    bool push(const MidiEvent& event) noexcept {
        if (!events || event_count >= event_capacity || event.size == 0 ||
            event.size > CM5_MIDI_MAX_EVENT_BYTES)
            return false;
        events[event_count++] = event;
        return true;
    }
};

struct AudioProcessContext {
    // Legacy primary-bus aliases. Existing processors can continue using these
    // fields through the default adapter below.
    const float* input = nullptr;
    float* output = nullptr;
    size_t frames = 0;
    unsigned input_channels = 0;
    unsigned output_channels = 0;
    double sample_rate = 48000.0;

    // Typed, independently-sized buses. Arrays are prepared by DspEngine before
    // calling a processor; processors must not retain these pointers.
    const AudioBusBuffer* audio_inputs = nullptr;
    size_t audio_input_count = 0;
    AudioBusBuffer* audio_outputs = nullptr;
    size_t audio_output_count = 0;
    const MidiInputBuffer* midi_inputs = nullptr;
    size_t midi_input_count = 0;
    MidiOutputBuffer* midi_outputs = nullptr;
    size_t midi_output_count = 0;
};

// Native processors execute on the audio thread. Implementations must not
// allocate, lock, perform I/O, or throw from process(). New processors can
// override process(AudioProcessContext&) and use any declared typed ports.
// Existing effects retain their in-place process(buffer, frames, channels) API.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual void process(float* buffer, size_t frames, unsigned channels) {
        (void)buffer;
        (void)frames;
        (void)channels;
    }

    virtual void process(AudioProcessContext& context) {
        if (!context.audio_outputs || context.audio_output_count == 0) {
            if (context.output && context.output_channels)
                process(context.output, context.frames, context.output_channels);
            return;
        }

        AudioBusBuffer& primaryOutput = context.audio_outputs[0];
        if (!primaryOutput.output || primaryOutput.channels == 0 || context.frames == 0)
            return;

        const AudioBusBuffer* primaryInput =
            context.audio_inputs && context.audio_input_count
                ? &context.audio_inputs[0] : nullptr;

        // Legacy processors are in-place effects. Their adapter must first copy
        // the primary input bus into the primary output bus; the graph keeps the
        // input and output endpoint buffers separate so they can be routed
        // independently. A null input is treated as silence.
        if (!primaryInput || !primaryInput->input || primaryInput->channels == 0) {
            for (size_t i = 0; i < context.frames * primaryOutput.channels; ++i)
                primaryOutput.output[i] = 0.0f;
        } else if (primaryInput->input == primaryOutput.output &&
                   primaryInput->channels == primaryOutput.channels) {
            // Already in-place.
        } else {
            const unsigned copyChannels = std::min(primaryInput->channels, primaryOutput.channels);
            for (size_t frame = 0; frame < context.frames; ++frame) {
                const float* source = primaryInput->input + frame * primaryInput->channels;
                float* destination = primaryOutput.output + frame * primaryOutput.channels;
                for (unsigned channel = 0; channel < copyChannels; ++channel)
                    destination[channel] = source[channel];
                for (unsigned channel = copyChannels; channel < primaryOutput.channels; ++channel)
                    destination[channel] = 0.0f;
            }
        }

        process(primaryOutput.output, context.frames, primaryOutput.channels);
    }

    // Realtime parameter control is delivered by the MIDI control worker, not
    // the audio callback. Implementations must update only state that the audio
    // processor already reads safely (normally atomic parameters). Static or
    // structurally significant settings should remain unsupported.
    virtual const char* parameter_control_type() const noexcept { return nullptr; }
    virtual bool supports_parameter_control(const std::string& parameterId) const noexcept {
        (void)parameterId;
        return false;
    }
    virtual bool get_parameter_control(const std::string& parameterId, float& value) const noexcept {
        (void)parameterId;
        (void)value;
        return false;
    }
    virtual bool set_parameter_control(const std::string& parameterId, float value) noexcept {
        (void)parameterId;
        (void)value;
        return false;
    }

    // Called on the control thread when a graph-native MIDI input port is
    // attached. Most processors do not need to override it.
    virtual void attach_midi_input(const std::string& portId,
                                   const std::shared_ptr<MidiEventQueue>& queue) {
        (void)portId;
        (void)queue;
    }
};

#endif
