#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <cstddef>

// Audio buffers are interleaved float32: frame * channels + channel.
// The input and output buffers are distinct for graph-native plugin nodes.
struct AudioProcessContext {
    const float* input = nullptr;
    float* output = nullptr;
    size_t frames = 0;
    unsigned input_channels = 0;
    unsigned output_channels = 0;
    double sample_rate = 48000.0;
};

// Native processors execute on the audio thread. Implementations must not
// allocate, lock, perform I/O, or throw from either process() overload.
//
// The legacy in-place overload remains available for existing route-chain
// processors. New graph-native processors may override the context overload
// to implement independent input/output widths. Its default adapter maps the
// common channels, clears additional outputs, then invokes the legacy method.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual void process(float* buffer, size_t frames, unsigned channels) {
        (void)buffer;
        (void)frames;
        (void)channels;
    }

    virtual void process(AudioProcessContext& context) {
        if (!context.output || context.frames == 0 || context.output_channels == 0)
            return;

        if (!context.input || context.input_channels == 0) {
            for (size_t i = 0; i < context.frames * context.output_channels; ++i)
                context.output[i] = 0.0f;
            return;
        }

        const unsigned copyChannels = context.input_channels < context.output_channels
            ? context.input_channels : context.output_channels;

        for (size_t frame = 0; frame < context.frames; ++frame) {
            float* out = context.output + frame * context.output_channels;
            const float* in = context.input + frame * context.input_channels;
            for (unsigned channel = 0; channel < copyChannels; ++channel)
                out[channel] = in[channel];
            for (unsigned channel = copyChannels; channel < context.output_channels; ++channel)
                out[channel] = 0.0f;
        }

        process(context.output, context.frames, context.output_channels);
    }
};

#endif
