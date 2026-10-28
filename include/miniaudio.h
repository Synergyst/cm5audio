#ifndef MINIAUDIO_H
#define MINIAUDIO_H

/*
 * Minimal C++17 Miniaudio-compatible shim using PipeWire's pw_filter API.
 *
 * Supported callback PCM formats: u8, s16, packed s24, s32, f32.
 * PipeWire DSP ports are always 32-bit float, one mono port per channel.
 * The callback buffers are interleaved in the configured callback format.
 *
 * This is intentionally a small subset of Miniaudio, not the full library.
 * Configure sampleRate to match the PipeWire graph rate. PipeWire handles
 * conversion between hardware-native formats/rates and the graph; this shim
 * converts callback sample formats but does not resample the application's
 * callback between different sample rates.
 */

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// 1. MINIMAL TYPES AND CONSTANTS
// -----------------------------------------------------------------------------
using ma_uint8  = uint8_t;
using ma_uint16 = uint16_t;
using ma_uint32 = uint32_t;
using ma_uint64 = uint64_t;
using ma_int8   = int8_t;
using ma_int16  = int16_t;
using ma_int32  = int32_t;
using ma_int64  = int64_t;
using ma_result = int32_t;
using ma_bool32 = int32_t;

#define MA_SUCCESS 0
#define MA_ERROR   (-1)
#define MA_MAX_CHANNELS 256
#define MA_SIMD_ALIGNMENT 64
#define MA_PIPEWIRE_MAX_PROCESS_FRAMES 16384u

// Values kept compatible with the shim supplied by the application.
typedef enum {
    ma_device_type_playback = 1,
    ma_device_type_capture  = 2,
    ma_device_type_duplex   = 3
} ma_device_type;

typedef enum {
    ma_format_unknown = 0,
    ma_format_u8      = 1,
    ma_format_s16     = 2,
    ma_format_s24     = 3, // tightly packed 24-bit signed PCM
    ma_format_s32     = 4,
    ma_format_f32     = 5
} ma_format;

typedef enum {
    ma_share_mode_shared = 0,
    ma_share_mode_exclusive = 1
} ma_share_mode;

typedef enum {
    ma_backend_alsa       = 0,
    ma_backend_pulseaudio = 1,
    ma_backend_pipewire   = 2,
    ma_backend_jack       = 3,
    ma_backend_oss        = 4,
    ma_backend_wasapi     = 5,
    ma_backend_null       = 6,
    ma_backend_coreaudio  = 7,
    ma_backend_sndio      = 8,
    ma_backend_audio4     = 9,
    ma_backend_webaudio   = 10,
    ma_backend_winmm      = 11,
    ma_backend_aaudio     = 12,
    ma_backend_opensl     = 13,
    ma_backend_dsound     = 14,
    ma_backend_custom     = 15
} ma_backend;

union ma_device_id {
    char alsa[256];
    char pulse[256];
    ma_uint32 jack; // Used here to hold a PipeWire registry node ID.
    int nullbackend;
    char coreaudio[256];
    char sndio[256];
    char audio4[256];
    char oss[256];
    char webaudio[256];
    int winmm;
    int aaudio;
    int opensl;
    char wasapi[256];
    uint8_t dsound[16];
    uint8_t custom[256];
};

struct ma_device_info {
    ma_device_id id;
    char name[256];
    ma_uint32 nativeDataFormatCount;
    struct {
        ma_format format;
        ma_uint32 channels;
        ma_uint32 sampleRate;
        ma_uint32 flags;
    } nativeDataFormats[16];
};

struct ma_context_config {
    ma_uint32 placeholder;
};

struct ma_context {
    ma_backend backend;
    std::vector<ma_device_info> playbackDevices;
    std::vector<ma_device_info> captureDevices;
};

struct ma_device;

typedef void (*ma_device_data_callback)(
    ma_device* pDevice,
    void* pOutput,
    const void* pInput,
    ma_uint32 frameCount
);

struct ma_device_channel_config {
    ma_format format;
    ma_uint32 channels;
    const ma_device_id* pDeviceID;
    ma_share_mode shareMode;

    // Compatibility fields used by this shim.
    ma_device_id id;
    ma_uint32 internalChannels;
    ma_uint32 internalSampleRate;
    ma_format internalFormat;
    int channelMap[32];
    int internalChannelMap[32];
    const char* const* customPortNames;

    // Optional bound for customPortNames. If zero, at most `channels` names
    // are read for compatibility with the original header.
    ma_uint32 customPortNameCount;
};

struct ma_device_config {
    ma_device_type type;
    ma_uint32 sampleRate;
    ma_uint32 periodSizeInFrames;
    ma_device_channel_config playback;
    ma_device_channel_config capture;
    ma_device_data_callback dataCallback;
    void* pUserData;
    const char* pNodeName;
    const char* pNodeDescription;
};

struct ma_device {
    ma_device_type type;
    ma_uint32 sampleRate;
    ma_device_channel_config playback;
    ma_device_channel_config capture;
    ma_device_data_callback dataCallback;
    void* pUserData;

    struct pw_main_loop* loop;
    std::thread loop_thread;
    struct pw_filter* filter;
    std::vector<void*> in_ports;
    std::vector<void*> out_ports;

    // All buffers below are allocated during init, never grown in RT process.
    std::vector<float> input_interleaved;
    std::vector<float> output_interleaved;
    std::vector<uint8_t> callback_input;
    std::vector<uint8_t> callback_output;
    ma_uint32 maxProcessFrames;

    // Diagnostics may be read by application code while the device is running.
    std::atomic<ma_uint32> graphSampleRate;
    std::atomic<ma_bool32> sampleRateMismatch;
    std::atomic<ma_uint64> processCount;
    std::atomic<ma_uint64> oversizedBlockCount;
    std::atomic<ma_uint64> nullPortBufferCount;
};

// -----------------------------------------------------------------------------
// 2. SAMPLE FORMAT AND CONVERSION HELPERS
// -----------------------------------------------------------------------------
inline ma_uint32 ma_get_bytes_per_sample(ma_format format)
{
    switch (format) {
        case ma_format_u8:  return 1;
        case ma_format_s16: return 2;
        case ma_format_s24: return 3;
        case ma_format_s32:
        case ma_format_f32: return 4;
        default:            return 0;
    }
}

inline ma_uint32 ma_get_bytes_per_frame(ma_format format, ma_uint32 channels)
{
    const ma_uint32 sampleBytes = ma_get_bytes_per_sample(format);

    if (sampleBytes == 0 ||
        channels == 0 ||
        channels > std::numeric_limits<ma_uint32>::max() / sampleBytes) {
        return 0;
    }

    return sampleBytes * channels;
}

inline const char* ma_get_format_name(ma_format format)
{
    switch (format) {
        case ma_format_f32: return "f32";
        case ma_format_s32: return "s32";
        case ma_format_s24: return "s24";
        case ma_format_s16: return "s16";
        case ma_format_u8:  return "u8";
        default:            return "unknown";
    }
}

inline const char* ma_result_description(ma_result result)
{
    return result == MA_SUCCESS ? "MA_SUCCESS" : "MA_ERROR";
}

inline const char* ma_get_backend_name(ma_backend)
{
    return "PipeWire-Native";
}

inline bool ma_is_supported_format(ma_format format)
{
    return ma_get_bytes_per_sample(format) != 0;
}

inline float ma_sanitize_float(float x)
{
    return std::isfinite(x) ? x : 0.0f;
}

inline float ma_read_sample_f32(
    const void* src,
    ma_format format,
    size_t sampleIndex)
{
    if (!src) return 0.0f;

    const uint8_t* bytes = static_cast<const uint8_t*>(src);

    switch (format) {
        case ma_format_f32: {
            float value;
            std::memcpy(&value, bytes + sampleIndex * 4, sizeof(value));
            return ma_sanitize_float(value);
        }

        case ma_format_u8: {
            const int value = bytes[sampleIndex];
            return static_cast<float>(value - 128) / 128.0f;
        }

        case ma_format_s16: {
            int16_t value;
            std::memcpy(&value, bytes + sampleIndex * 2, sizeof(value));
            return static_cast<float>(value) / 32768.0f;
        }

        case ma_format_s24: {
            const uint8_t* p = bytes + sampleIndex * 3;
            int32_t value;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
            value = (static_cast<int32_t>(p[0]) << 16) |
                    (static_cast<int32_t>(p[1]) << 8)  |
                    static_cast<int32_t>(p[2]);
#else
            value = static_cast<int32_t>(p[0]) |
                    (static_cast<int32_t>(p[1]) << 8) |
                    (static_cast<int32_t>(p[2]) << 16);
#endif

            if (value & 0x00800000) {
                value |= static_cast<int32_t>(0xFF000000u);
            }

            return static_cast<float>(value) / 8388608.0f;
        }

        case ma_format_s32: {
            int32_t value;
            std::memcpy(&value, bytes + sampleIndex * 4, sizeof(value));

            return static_cast<float>(
                static_cast<double>(value) / 2147483648.0
            );
        }

        default:
            return 0.0f;
    }
}

inline void ma_write_sample_from_f32(
    void* dst,
    ma_format format,
    size_t sampleIndex,
    float sample)
{
    if (!dst) return;

    uint8_t* bytes = static_cast<uint8_t*>(dst);
    const double x = static_cast<double>(ma_sanitize_float(sample));

    switch (format) {
        case ma_format_f32: {
            const float value = static_cast<float>(x);
            std::memcpy(bytes + sampleIndex * 4, &value, sizeof(value));
            break;
        }

        case ma_format_u8: {
            const double clamped = std::max(-1.0, std::min(1.0, x));
            const int value = static_cast<int>(
                std::llround((clamped + 1.0) * 127.5)
            );

            bytes[sampleIndex] = static_cast<uint8_t>(
                std::max(0, std::min(255, value))
            );
            break;
        }

        case ma_format_s16: {
            const double clamped = std::max(-1.0, std::min(1.0, x));
            const double scaled = clamped < 0.0
                ? clamped * 32768.0
                : clamped * 32767.0;

            const long value = std::lround(scaled);
            const int16_t out = static_cast<int16_t>(
                std::max(-32768L, std::min(32767L, value))
            );

            std::memcpy(bytes + sampleIndex * 2, &out, sizeof(out));
            break;
        }

        case ma_format_s24: {
            const double clamped = std::max(-1.0, std::min(1.0, x));
            const double scaled = clamped < 0.0
                ? clamped * 8388608.0
                : clamped * 8388607.0;

            int32_t value = static_cast<int32_t>(std::llround(scaled));
            value = std::max(-8388608, std::min(8388607, value));

            uint8_t* p = bytes + sampleIndex * 3;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
            p[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
            p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>(value & 0xFF);
#else
            p[0] = static_cast<uint8_t>(value & 0xFF);
            p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
#endif
            break;
        }

        case ma_format_s32: {
            const double clamped = std::max(-1.0, std::min(1.0, x));

            const long double scaled = clamped < 0.0
                ? static_cast<long double>(clamped) * 2147483648.0L
                : static_cast<long double>(clamped) * 2147483647.0L;

            int64_t wide = static_cast<int64_t>(std::llround(scaled));
            wide = std::max<int64_t>(
                INT32_MIN,
                std::min<int64_t>(INT32_MAX, wide)
            );

            const int32_t out = static_cast<int32_t>(wide);
            std::memcpy(bytes + sampleIndex * 4, &out, sizeof(out));
            break;
        }

        default:
            break;
    }
}

inline void ma_fill_silence(
    void* dst,
    size_t frameCount,
    ma_format format,
    ma_uint32 channels)
{
    if (!dst) return;

    const ma_uint32 bpf = ma_get_bytes_per_frame(format, channels);
    if (!bpf) return;

    std::memset(dst, 0, frameCount * static_cast<size_t>(bpf));

    // Unsigned 8-bit PCM uses 128 as digital silence, not 0.
    if (format == ma_format_u8) {
        std::memset(dst, 128, frameCount * static_cast<size_t>(bpf));
    }
}

inline int ma_channel_position(const int map[32], ma_uint32 channel)
{
    return channel < 32 ? map[channel] : static_cast<int>(channel);
}

inline void ma_fix_all_zero_channel_map(int map[32], ma_uint32 channels)
{
    if (channels <= 1) return;

    const ma_uint32 n = std::min<ma_uint32>(channels, 32);
    bool allZero = true;

    for (ma_uint32 i = 0; i < n; ++i) {
        if (map[i] != 0) {
            allZero = false;
            break;
        }
    }

    // Treat an all-zero map from a zero-initialized config as the default map.
    if (allZero) {
        for (int i = 0; i < 32; ++i) {
            map[i] = i;
        }
    }
}

inline int ma_find_channel_position(
    const int map[32],
    ma_uint32 channels,
    int targetPosition)
{
    for (ma_uint32 i = 0; i < channels; ++i) {
        if (ma_channel_position(map, i) == targetPosition) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

// -----------------------------------------------------------------------------
// 3. SINGLE-PRODUCER / SINGLE-CONSUMER PCM RING BUFFER
// -----------------------------------------------------------------------------
struct ma_pcm_rb {
    ma_format format;
    ma_uint32 channels;
    ma_uint32 capacityFrames;
    std::atomic<ma_uint64> rIndex;
    std::atomic<ma_uint64> wIndex;
    ma_uint32 bytesPerFrame;
    std::vector<uint8_t> buffer;

    ma_pcm_rb()
        : format(ma_format_unknown),
          channels(0),
          capacityFrames(0),
          rIndex(0),
          wIndex(0),
          bytesPerFrame(0),
          buffer()
    {}

    ma_pcm_rb(const ma_pcm_rb&) = delete;
    ma_pcm_rb& operator=(const ma_pcm_rb&) = delete;

    // Preserve compatibility with existing project code such as `rb = {};`.
    // The ring buffer must not be copied while a producer/consumer is active.
    ma_pcm_rb(ma_pcm_rb&& other) noexcept
        : format(other.format),
          channels(other.channels),
          capacityFrames(other.capacityFrames),
          rIndex(other.rIndex.load(std::memory_order_relaxed)),
          wIndex(other.wIndex.load(std::memory_order_relaxed)),
          bytesPerFrame(other.bytesPerFrame),
          buffer(std::move(other.buffer))
    {
        other.format = ma_format_unknown;
        other.channels = 0;
        other.capacityFrames = 0;
        other.bytesPerFrame = 0;
        other.rIndex.store(0, std::memory_order_relaxed);
        other.wIndex.store(0, std::memory_order_relaxed);
    }

    ma_pcm_rb& operator=(ma_pcm_rb&& other) noexcept
    {
        if (this != &other) {
            format = other.format;
            channels = other.channels;
            capacityFrames = other.capacityFrames;
            bytesPerFrame = other.bytesPerFrame;
            rIndex.store(
                other.rIndex.load(std::memory_order_relaxed),
                std::memory_order_relaxed
            );
            wIndex.store(
                other.wIndex.load(std::memory_order_relaxed),
                std::memory_order_relaxed
            );
            buffer = std::move(other.buffer);

            other.format = ma_format_unknown;
            other.channels = 0;
            other.capacityFrames = 0;
            other.bytesPerFrame = 0;
            other.rIndex.store(0, std::memory_order_relaxed);
            other.wIndex.store(0, std::memory_order_relaxed);
        }

        return *this;
    }
};

inline ma_result ma_pcm_rb_init(
    ma_format format,
    ma_uint32 channels,
    ma_uint32 capacityFrames,
    void*,
    void*,
    ma_pcm_rb* pRB)
{
    if (!pRB ||
        !ma_is_supported_format(format) ||
        channels == 0 ||
        channels > MA_MAX_CHANNELS ||
        capacityFrames == 0) {
        return MA_ERROR;
    }

    const ma_uint32 bpf = ma_get_bytes_per_frame(format, channels);

    if (bpf == 0 ||
        static_cast<uint64_t>(capacityFrames) * bpf >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return MA_ERROR;
    }

    try {
        pRB->buffer.assign(
            static_cast<size_t>(capacityFrames) * bpf,
            0
        );
    } catch (...) {
        pRB->buffer.clear();
        pRB->format = ma_format_unknown;
        pRB->channels = pRB->capacityFrames = pRB->bytesPerFrame = 0;
        pRB->rIndex.store(0, std::memory_order_relaxed);
        pRB->wIndex.store(0, std::memory_order_relaxed);
        return MA_ERROR;
    }

    pRB->format = format;
    pRB->channels = channels;
    pRB->capacityFrames = capacityFrames;
    pRB->bytesPerFrame = bpf;
    pRB->rIndex.store(0, std::memory_order_relaxed);
    pRB->wIndex.store(0, std::memory_order_relaxed);

    return MA_SUCCESS;
}

inline void ma_pcm_rb_uninit(ma_pcm_rb* pRB)
{
    if (!pRB) return;

    pRB->buffer.clear();
    pRB->format = ma_format_unknown;
    pRB->channels = pRB->capacityFrames = pRB->bytesPerFrame = 0;
    pRB->rIndex.store(0, std::memory_order_relaxed);
    pRB->wIndex.store(0, std::memory_order_relaxed);
}

inline ma_uint32 ma_pcm_rb_available_read(ma_pcm_rb* pRB)
{
    if (!pRB || pRB->capacityFrames == 0) return 0;

    const ma_uint64 r = pRB->rIndex.load(std::memory_order_relaxed);
    const ma_uint64 w = pRB->wIndex.load(std::memory_order_acquire);
    const ma_uint64 used = w - r;

    return static_cast<ma_uint32>(
        std::min<ma_uint64>(used, pRB->capacityFrames)
    );
}

inline ma_uint32 ma_pcm_rb_available_write(ma_pcm_rb* pRB)
{
    if (!pRB || pRB->capacityFrames == 0) return 0;

    const ma_uint32 used = ma_pcm_rb_available_read(pRB);

    return used >= pRB->capacityFrames
        ? 0
        : pRB->capacityFrames - used;
}

inline ma_result ma_pcm_rb_acquire_read(
    ma_pcm_rb* pRB,
    ma_uint32* pframeCount,
    void** ppData)
{
    if (!pRB || !pframeCount || !ppData ||
        pRB->capacityFrames == 0 || pRB->bytesPerFrame == 0) {
        return MA_ERROR;
    }

    const ma_uint64 r = pRB->rIndex.load(std::memory_order_relaxed);
    const ma_uint64 w = pRB->wIndex.load(std::memory_order_acquire);

    const ma_uint64 used64 = std::min<ma_uint64>(
        w - r,
        pRB->capacityFrames
    );

    const ma_uint32 contiguous = pRB->capacityFrames -
        static_cast<ma_uint32>(r % pRB->capacityFrames);

    const ma_uint32 count = static_cast<ma_uint32>(
        std::min<ma_uint64>(
            *pframeCount,
            std::min<ma_uint64>(used64, contiguous)
        )
    );

    *pframeCount = count;

    *ppData = count
        ? static_cast<void*>(
            pRB->buffer.data() +
            static_cast<size_t>(r % pRB->capacityFrames) * pRB->bytesPerFrame
          )
        : nullptr;

    return MA_SUCCESS;
}

inline ma_result ma_pcm_rb_commit_read(
    ma_pcm_rb* pRB,
    ma_uint32 frameCount)
{
    if (!pRB || pRB->capacityFrames == 0) return MA_ERROR;

    const ma_uint64 r = pRB->rIndex.load(std::memory_order_relaxed);
    const ma_uint64 w = pRB->wIndex.load(std::memory_order_acquire);

    const ma_uint64 used = std::min<ma_uint64>(
        w - r,
        pRB->capacityFrames
    );

    const ma_uint32 contiguous = pRB->capacityFrames -
        static_cast<ma_uint32>(r % pRB->capacityFrames);

    if (frameCount > used || frameCount > contiguous) {
        return MA_ERROR;
    }

    pRB->rIndex.store(r + frameCount, std::memory_order_release);
    return MA_SUCCESS;
}

inline ma_result ma_pcm_rb_acquire_write(
    ma_pcm_rb* pRB,
    ma_uint32* pframeCount,
    void** ppData)
{
    if (!pRB || !pframeCount || !ppData ||
        pRB->capacityFrames == 0 || pRB->bytesPerFrame == 0) {
        return MA_ERROR;
    }

    const ma_uint64 w = pRB->wIndex.load(std::memory_order_relaxed);
    const ma_uint64 r = pRB->rIndex.load(std::memory_order_acquire);

    const ma_uint64 used = std::min<ma_uint64>(
        w - r,
        pRB->capacityFrames
    );

    const ma_uint32 available = used >= pRB->capacityFrames
        ? 0
        : pRB->capacityFrames - static_cast<ma_uint32>(used);

    const ma_uint32 contiguous = pRB->capacityFrames -
        static_cast<ma_uint32>(w % pRB->capacityFrames);

    const ma_uint32 count = std::min(
        *pframeCount,
        std::min(available, contiguous)
    );

    *pframeCount = count;

    *ppData = count
        ? static_cast<void*>(
            pRB->buffer.data() +
            static_cast<size_t>(w % pRB->capacityFrames) * pRB->bytesPerFrame
          )
        : nullptr;

    return MA_SUCCESS;
}

inline ma_result ma_pcm_rb_commit_write(
    ma_pcm_rb* pRB,
    ma_uint32 frameCount)
{
    if (!pRB || pRB->capacityFrames == 0) return MA_ERROR;

    const ma_uint64 w = pRB->wIndex.load(std::memory_order_relaxed);
    const ma_uint64 r = pRB->rIndex.load(std::memory_order_acquire);

    const ma_uint64 used = std::min<ma_uint64>(
        w - r,
        pRB->capacityFrames
    );

    const ma_uint32 available = used >= pRB->capacityFrames
        ? 0
        : pRB->capacityFrames - static_cast<ma_uint32>(used);

    const ma_uint32 contiguous = pRB->capacityFrames -
        static_cast<ma_uint32>(w % pRB->capacityFrames);

    if (frameCount > available || frameCount > contiguous) {
        return MA_ERROR;
    }

    pRB->wIndex.store(w + frameCount, std::memory_order_release);
    return MA_SUCCESS;
}

// -----------------------------------------------------------------------------
// 4. PIPEWIRE INITIALIZATION AND DEVICE ENUMERATION
// -----------------------------------------------------------------------------
inline void ma_pipewire_init_once()
{
    static std::once_flag initFlag;
    std::call_once(initFlag, []() { pw_init(nullptr, nullptr); });
}

inline ma_context_config ma_context_config_init()
{
    return ma_context_config{};
}

struct ma_pw_enum_data {
    std::vector<ma_device_info>* playback;
    std::vector<ma_device_info>* capture;
};

inline void ma_pw_enum_registry_global(
    void* data,
    uint32_t id,
    uint32_t,
    const char* type,
    uint32_t,
    const struct spa_dict* props)
{
    auto* enumData = static_cast<ma_pw_enum_data*>(data);

    if (!enumData || !type || !props ||
        std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }

    const char* mediaClass = spa_dict_lookup(props, "media.class");
    const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char* channelsStr = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNELS);

    if (!mediaClass) return;

    ma_uint32 channels = 2;

    if (channelsStr && channelsStr[0]) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(channelsStr, &end, 10);

        if (end != channelsStr && *end == '\0' &&
            parsed > 0 && parsed <= MA_MAX_CHANNELS) {
            channels = static_cast<ma_uint32>(parsed);
        }
    }

    const std::string klass(mediaClass);

    const bool isSink =
        klass.find("Audio/Sink") != std::string::npos ||
        klass.find("Stream/Output/Audio") != std::string::npos;

    const bool isSource =
        klass.find("Audio/Source") != std::string::npos ||
        klass.find("Stream/Input/Audio") != std::string::npos;

    const bool isFilter =
        klass.find("Audio/Filter") != std::string::npos;

    if (!isSink && !isSource && !isFilter) return;

    ma_device_info info{};
    info.id.jack = id; // Store only the PipeWire node ID.
    const char* displayName =
        (description && description[0]) ? description :
        ((nodeName && nodeName[0]) ? nodeName : "PipeWire Audio Node");

    std::snprintf(info.name, sizeof(info.name), "%s", displayName);

    info.nativeDataFormatCount = 1;
    info.nativeDataFormats[0] = { ma_format_f32, channels, 48000, 0 };

    if ((isSink || isFilter) && enumData->playback) {
        enumData->playback->push_back(info);
    }

    if ((isSource || isFilter) && enumData->capture) {
        enumData->capture->push_back(info);
    }
}

struct ma_pw_enum_sync {
    int sequence;
    bool done;
};

inline void ma_pw_enum_core_done(void* data, uint32_t, int sequence)
{
    auto* sync = static_cast<ma_pw_enum_sync*>(data);
    if (sync && sequence == sync->sequence) {
        sync->done = true;
    }
}

inline const struct pw_registry_events* ma_pw_get_registry_events()
{
    static const struct pw_registry_events events = [] {
        struct pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = ma_pw_enum_registry_global;
        return e;
    }();

    return &events;
}

inline const struct pw_core_events* ma_pw_get_core_events()
{
    static const struct pw_core_events events = [] {
        struct pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.done = ma_pw_enum_core_done;
        return e;
    }();

    return &events;
}

inline ma_result ma_context_init(
    const ma_backend*,
    ma_uint32,
    const void*,
    ma_context* pContext)
{
    if (!pContext) return MA_ERROR;

    pContext->backend = ma_backend_pipewire;
    pContext->playbackDevices.clear();
    pContext->captureDevices.clear();

    ma_pipewire_init_once();

    struct pw_main_loop* loop = pw_main_loop_new(nullptr);
    if (!loop) return MA_ERROR;

    struct pw_context* context = pw_context_new(
        pw_main_loop_get_loop(loop),
        nullptr,
        0
    );

    if (!context) {
        pw_main_loop_destroy(loop);
        return MA_ERROR;
    }

    struct pw_core* core = pw_context_connect(context, nullptr, 0);

    if (!core) {
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return MA_ERROR;
    }

    struct pw_registry* registry = pw_core_get_registry(
        core,
        PW_VERSION_REGISTRY,
        0
    );

    if (!registry) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return MA_ERROR;
    }

    ma_pw_enum_data enumData{
        &pContext->playbackDevices,
        &pContext->captureDevices
    };

    ma_pw_enum_sync syncData{ -1, false };

    struct spa_hook registryHook{};
    struct spa_hook coreHook{};

    pw_registry_add_listener(
        registry,
        &registryHook,
        ma_pw_get_registry_events(),
        &enumData
    );

    pw_core_add_listener(
        core,
        &coreHook,
        ma_pw_get_core_events(),
        &syncData
    );

    syncData.sequence = pw_core_sync(core, PW_ID_CORE, 0);

    if (syncData.sequence < 0) {
        spa_hook_remove(&registryHook);
        spa_hook_remove(&coreHook);

        pw_proxy_destroy(
            reinterpret_cast<struct pw_proxy*>(registry)
        );

        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);

        return MA_ERROR;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (!syncData.done &&
           std::chrono::steady_clock::now() < deadline) {
        const int r = pw_loop_iterate(
            pw_main_loop_get_loop(loop),
            20
        );

        if (r < 0) break;
    }

    const bool completed = syncData.done;

    spa_hook_remove(&registryHook);
    spa_hook_remove(&coreHook);

    pw_proxy_destroy(
        reinterpret_cast<struct pw_proxy*>(registry)
    );

    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return completed ? MA_SUCCESS : MA_ERROR;
}

inline ma_result ma_context_get_devices(
    ma_context* pContext,
    ma_device_info** ppPlaybackDeviceInfos,
    ma_uint32* pPlaybackDeviceCount,
    ma_device_info** ppCaptureDeviceInfos,
    ma_uint32* pCaptureDeviceCount)
{
    if (!pContext) return MA_ERROR;

    if (ppPlaybackDeviceInfos && pPlaybackDeviceCount) {
        *ppPlaybackDeviceInfos = pContext->playbackDevices.empty()
            ? nullptr
            : pContext->playbackDevices.data();

        *pPlaybackDeviceCount =
            static_cast<ma_uint32>(pContext->playbackDevices.size());
    }

    if (ppCaptureDeviceInfos && pCaptureDeviceCount) {
        *ppCaptureDeviceInfos = pContext->captureDevices.empty()
            ? nullptr
            : pContext->captureDevices.data();

        *pCaptureDeviceCount =
            static_cast<ma_uint32>(pContext->captureDevices.size());
    }

    return MA_SUCCESS;
}

inline ma_result ma_context_get_device_info(
    ma_context* pContext,
    ma_device_type type,
    const ma_device_id* pDeviceID,
    ma_device_info* pDeviceInfo)
{
    if (!pContext || !pDeviceInfo) return MA_ERROR;

    const auto& list = (type & ma_device_type_capture)
        ? pContext->captureDevices
        : pContext->playbackDevices;

    for (const auto& device : list) {
        if (!pDeviceID || device.id.jack == pDeviceID->jack) {
            *pDeviceInfo = device;
            return MA_SUCCESS;
        }
    }

    *pDeviceInfo = ma_device_info{};
    return MA_ERROR;
}

inline ma_result ma_context_uninit(ma_context* pContext)
{
    if (!pContext) return MA_SUCCESS;

    pContext->playbackDevices.clear();
    pContext->captureDevices.clear();

    return MA_SUCCESS;
}

inline ma_device_config ma_device_config_init(ma_device_type type)
{
    ma_device_config config{};

    config.type = type;
    config.sampleRate = 48000;
    config.periodSizeInFrames = 0;

    config.playback.format = ma_format_f32;
    config.playback.channels = 16;

    config.capture.format = ma_format_f32;
    config.capture.channels = 16;

    config.playback.shareMode = ma_share_mode_shared;
    config.capture.shareMode = ma_share_mode_shared;

    for (int i = 0; i < 32; ++i) {
        config.playback.channelMap[i] = i;
        config.playback.internalChannelMap[i] = i;
        config.capture.channelMap[i] = i;
        config.capture.internalChannelMap[i] = i;
    }

    config.pNodeName = nullptr;
    config.pNodeDescription = nullptr;

    return config;
}

// -----------------------------------------------------------------------------
// 5. PIPEWIRE FILTER PROCESSING
// -----------------------------------------------------------------------------
inline void ma_pw_shim_on_process(
    void* userdata,
    struct spa_io_position* position)
{
    auto* device = static_cast<ma_device*>(userdata);

    if (!device || !position) return;

    const ma_uint32 frames = position->clock.duration;
    if (frames == 0) return;

    device->processCount.fetch_add(1, std::memory_order_relaxed);

    // SPA clock.rate is a period per sample (typically 1/48000).
    if (position->clock.rate.num != 0 &&
        position->clock.rate.denom != 0) {
        const double rate =
            static_cast<double>(position->clock.rate.denom) /
            static_cast<double>(position->clock.rate.num);

        const ma_uint32 graphRate =
            (rate > 0.0 && rate < 1000000.0)
                ? static_cast<ma_uint32>(std::llround(rate))
                : 0;

        if (graphRate != 0) {
            device->graphSampleRate.store(
                graphRate,
                std::memory_order_relaxed
            );

            device->sampleRateMismatch.store(
                graphRate != device->sampleRate ? 1 : 0,
                std::memory_order_relaxed
            );
        }
    }

    const bool wantsCapture =
        (device->type & ma_device_type_capture) != 0;

    const bool wantsPlayback =
        (device->type & ma_device_type_playback) != 0;

    const ma_uint32 capturePorts = wantsCapture
        ? static_cast<ma_uint32>(device->in_ports.size())
        : 0;

    const ma_uint32 playbackPorts = wantsPlayback
        ? static_cast<ma_uint32>(device->out_ports.size())
        : 0;

    // If a PipeWire quantum exceeds the preallocated capacity, emit silence and
    // skip the callback. Never resize/allocate/throw on the real-time thread.
    if (frames > device->maxProcessFrames) {
        device->oversizedBlockCount.fetch_add(
            1,
            std::memory_order_relaxed
        );

        for (ma_uint32 c = 0; c < playbackPorts; ++c) {
            float* out = static_cast<float*>(
                pw_filter_get_dsp_buffer(device->out_ports[c], frames)
            );

            if (out) {
                std::fill(out, out + frames, 0.0f);
            }
        }

        return;
    }

    if (wantsCapture && capturePorts != 0) {
        for (ma_uint32 c = 0; c < capturePorts; ++c) {
            float* in = static_cast<float*>(
                pw_filter_get_dsp_buffer(device->in_ports[c], frames)
            );

            if (in) {
                for (ma_uint32 f = 0; f < frames; ++f) {
                    device->input_interleaved[
                        static_cast<size_t>(f) * capturePorts + c
                    ] = ma_sanitize_float(in[f]);
                }
            } else {
                device->nullPortBufferCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

                for (ma_uint32 f = 0; f < frames; ++f) {
                    device->input_interleaved[
                        static_cast<size_t>(f) * capturePorts + c
                    ] = 0.0f;
                }
            }
        }
    }

    const void* callbackInput = nullptr;
    void* callbackOutput = nullptr;

    if (wantsCapture) {
        ma_fill_silence(
            device->callback_input.data(),
            frames,
            device->capture.format,
            device->capture.channels
        );

        const size_t appChannels = device->capture.channels;

        for (ma_uint32 f = 0; f < frames; ++f) {
            for (ma_uint32 appC = 0;
                 appC < device->capture.channels;
                 ++appC) {
                const int wantedPosition = ma_channel_position(
                    device->capture.channelMap,
                    appC
                );

                const int sourceC = ma_find_channel_position(
                    device->capture.internalChannelMap,
                    capturePorts,
                    wantedPosition
                );

                const float value =
                    (sourceC >= 0 &&
                     static_cast<ma_uint32>(sourceC) < capturePorts)
                    ? device->input_interleaved[
                        static_cast<size_t>(f) * capturePorts +
                        static_cast<ma_uint32>(sourceC)
                      ]
                    : 0.0f;

                ma_write_sample_from_f32(
                    device->callback_input.data(),
                    device->capture.format,
                    static_cast<size_t>(f) * appChannels + appC,
                    value
                );
            }
        }

        callbackInput = device->callback_input.data();
    }

    if (wantsPlayback) {
        ma_fill_silence(
            device->callback_output.data(),
            frames,
            device->playback.format,
            device->playback.channels
        );

        callbackOutput = device->callback_output.data();
    }

    if (device->dataCallback) {
        device->dataCallback(
            device,
            callbackOutput,
            callbackInput,
            frames
        );
    }

    if (wantsPlayback && playbackPorts != 0) {
        const size_t appChannels = device->playback.channels;

        for (ma_uint32 f = 0; f < frames; ++f) {
            for (ma_uint32 portC = 0; portC < playbackPorts; ++portC) {
                const int wantedPosition = ma_channel_position(
                    device->playback.internalChannelMap,
                    portC
                );

                const int sourceC = ma_find_channel_position(
                    device->playback.channelMap,
                    device->playback.channels,
                    wantedPosition
                );

                const float value =
                    (sourceC >= 0 &&
                     static_cast<ma_uint32>(sourceC) <
                        device->playback.channels)
                    ? ma_read_sample_f32(
                        device->callback_output.data(),
                        device->playback.format,
                        static_cast<size_t>(f) * appChannels +
                            static_cast<ma_uint32>(sourceC)
                      )
                    : 0.0f;

                device->output_interleaved[
                    static_cast<size_t>(f) * playbackPorts + portC
                ] = value;
            }
        }

        for (ma_uint32 c = 0; c < playbackPorts; ++c) {
            float* out = static_cast<float*>(
                pw_filter_get_dsp_buffer(device->out_ports[c], frames)
            );

            if (out) {
                for (ma_uint32 f = 0; f < frames; ++f) {
                    out[f] = device->output_interleaved[
                        static_cast<size_t>(f) * playbackPorts + c
                    ];
                }
            } else {
                device->nullPortBufferCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
        }
    }
}

inline const struct pw_filter_events* ma_pw_get_filter_events()
{
    static const struct pw_filter_events events = [] {
        struct pw_filter_events e{};
        e.version = PW_VERSION_FILTER_EVENTS;
        e.process = ma_pw_shim_on_process;
        return e;
    }();

    return &events;
}

inline void ma_pw_device_clear_resources(ma_device* pDevice)
{
    if (!pDevice) return;

    if (pDevice->filter) {
        pw_filter_destroy(pDevice->filter);
        pDevice->filter = nullptr;
    }

    if (pDevice->loop) {
        pw_main_loop_destroy(pDevice->loop);
        pDevice->loop = nullptr;
    }

    pDevice->in_ports.clear();
    pDevice->out_ports.clear();

    pDevice->input_interleaved.clear();
    pDevice->output_interleaved.clear();

    pDevice->callback_input.clear();
    pDevice->callback_output.clear();

    pDevice->maxProcessFrames = 0;
}

inline ma_result ma_device_init(
    ma_context* pContext,
    const ma_device_config* pConfig,
    ma_device* pDevice)
{
    if (!pConfig || !pDevice) return MA_ERROR;

    // A running/joinable loop thread cannot safely be overwritten by init.
    if (pDevice->loop_thread.joinable()) return MA_ERROR;

    pDevice->loop = nullptr;
    pDevice->filter = nullptr;

    pDevice->in_ports.clear();
    pDevice->out_ports.clear();

    pDevice->input_interleaved.clear();
    pDevice->output_interleaved.clear();

    pDevice->callback_input.clear();
    pDevice->callback_output.clear();

    pDevice->maxProcessFrames = 0;

    pDevice->graphSampleRate.store(0, std::memory_order_relaxed);
    pDevice->sampleRateMismatch.store(0, std::memory_order_relaxed);
    pDevice->processCount.store(0, std::memory_order_relaxed);
    pDevice->oversizedBlockCount.store(0, std::memory_order_relaxed);
    pDevice->nullPortBufferCount.store(0, std::memory_order_relaxed);

    pDevice->type = pConfig->type;
    pDevice->sampleRate = pConfig->sampleRate;
    pDevice->playback = pConfig->playback;
    pDevice->capture = pConfig->capture;
    pDevice->dataCallback = pConfig->dataCallback;
    pDevice->pUserData = pConfig->pUserData;

    const bool wantsPlayback =
        (pDevice->type & ma_device_type_playback) != 0;

    const bool wantsCapture =
        (pDevice->type & ma_device_type_capture) != 0;

    if ((!wantsPlayback && !wantsCapture) ||
        (pDevice->type & ~ma_device_type_duplex) != 0 ||
        pDevice->sampleRate == 0) {
        return MA_ERROR;
    }

    if (wantsPlayback) {
        if (pDevice->playback.channels == 0 ||
            pDevice->playback.channels > MA_MAX_CHANNELS ||
            !ma_is_supported_format(pDevice->playback.format)) {
            return MA_ERROR;
        }

        if (pDevice->playback.internalChannels == 0) {
            pDevice->playback.internalChannels =
                pDevice->playback.channels;
        }

        if (pDevice->playback.internalChannels > MA_MAX_CHANNELS) {
            return MA_ERROR;
        }

        if (pDevice->playback.internalSampleRate == 0) {
            pDevice->playback.internalSampleRate = pDevice->sampleRate;
        }

        // PipeWire DSP ports are always float, irrespective of callback format.
        pDevice->playback.internalFormat = ma_format_f32;

        ma_fix_all_zero_channel_map(
            pDevice->playback.channelMap,
            pDevice->playback.channels
        );

        ma_fix_all_zero_channel_map(
            pDevice->playback.internalChannelMap,
            pDevice->playback.internalChannels
        );
    }

    if (wantsCapture) {
        if (pDevice->capture.channels == 0 ||
            pDevice->capture.channels > MA_MAX_CHANNELS ||
            !ma_is_supported_format(pDevice->capture.format)) {
            return MA_ERROR;
        }

        if (pDevice->capture.internalChannels == 0) {
            pDevice->capture.internalChannels =
                pDevice->capture.channels;
        }

        if (pDevice->capture.internalChannels > MA_MAX_CHANNELS) {
            return MA_ERROR;
        }

        if (pDevice->capture.internalSampleRate == 0) {
            pDevice->capture.internalSampleRate = pDevice->sampleRate;
        }

        pDevice->capture.internalFormat = ma_format_f32;

        ma_fix_all_zero_channel_map(
            pDevice->capture.channelMap,
            pDevice->capture.channels
        );

        ma_fix_all_zero_channel_map(
            pDevice->capture.internalChannelMap,
            pDevice->capture.internalChannels
        );
    }

    ma_uint32 maxFrames = std::max<ma_uint32>(
        4096,
        pConfig->periodSizeInFrames
    );

    if (maxFrames > MA_PIPEWIRE_MAX_PROCESS_FRAMES) {
        return MA_ERROR;
    }

    pDevice->maxProcessFrames = maxFrames;

    const ma_uint32 capturePortCount = wantsCapture
        ? pDevice->capture.internalChannels
        : 0;

    const ma_uint32 playbackPortCount = wantsPlayback
        ? pDevice->playback.internalChannels
        : 0;

    const ma_uint32 callbackInputBPF = wantsCapture
        ? ma_get_bytes_per_frame(
              pDevice->capture.format,
              pDevice->capture.channels
          )
        : 0;

    const ma_uint32 callbackOutputBPF = wantsPlayback
        ? ma_get_bytes_per_frame(
              pDevice->playback.format,
              pDevice->playback.channels
          )
        : 0;

    if ((wantsCapture && callbackInputBPF == 0) ||
        (wantsPlayback && callbackOutputBPF == 0)) {
        return MA_ERROR;
    }

    try {
        if (capturePortCount) {
            pDevice->input_interleaved.resize(
                static_cast<size_t>(maxFrames) * capturePortCount,
                0.0f
            );
        }

        if (playbackPortCount) {
            pDevice->output_interleaved.resize(
                static_cast<size_t>(maxFrames) * playbackPortCount,
                0.0f
            );
        }

        if (wantsCapture) {
            pDevice->callback_input.resize(
                static_cast<size_t>(maxFrames) * callbackInputBPF,
                0
            );
        }

        if (wantsPlayback) {
            pDevice->callback_output.resize(
                static_cast<size_t>(maxFrames) * callbackOutputBPF,
                0
            );
        }
    } catch (...) {
        pDevice->input_interleaved.clear();
        pDevice->output_interleaved.clear();
        pDevice->callback_input.clear();
        pDevice->callback_output.clear();
        pDevice->maxProcessFrames = 0;
        return MA_ERROR;
    }

    ma_pipewire_init_once();

    pDevice->loop = pw_main_loop_new(nullptr);

    if (!pDevice->loop) {
        ma_pw_device_clear_resources(pDevice);
        return MA_ERROR;
    }

    static std::atomic<ma_uint64> nextDefaultName{1};

    std::string resolvedName =
        "cm5audio_device_" +
        std::to_string(
            nextDefaultName.fetch_add(1, std::memory_order_relaxed)
        );

    std::string resolvedDescription =
        "CM5Audio Native PipeWire Device Shim";

    if (pConfig->pNodeName && pConfig->pNodeName[0]) {
        resolvedName = pConfig->pNodeName;
    }

    if (pConfig->pNodeDescription && pConfig->pNodeDescription[0]) {
        resolvedDescription = pConfig->pNodeDescription;
    } else if (pContext) {
        const auto& list = wantsCapture
            ? pContext->captureDevices
            : pContext->playbackDevices;

        const ma_device_id* targetId = wantsCapture
            ? pDevice->capture.pDeviceID
            : pDevice->playback.pDeviceID;

        for (const auto& dev : list) {
            if (!targetId || dev.id.jack == targetId->jack) {
                if (dev.name[0]) {
                    resolvedDescription = dev.name;
                    break;
                }
            }
        }
    }

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, resolvedName.c_str(),
        PW_KEY_NODE_DESCRIPTION, resolvedDescription.c_str(),
        nullptr
    );

    if (!props) {
        ma_pw_device_clear_resources(pDevice);
        return MA_ERROR;
    }

    pDevice->filter = pw_filter_new_simple(
        pw_main_loop_get_loop(pDevice->loop),
        resolvedName.c_str(),
        props,
        ma_pw_get_filter_events(),
        pDevice
    );

    if (!pDevice->filter) {
        ma_pw_device_clear_resources(pDevice);
        return MA_ERROR;
    }

    auto addPorts = [&](
        bool isCapture,
        ma_uint32 count,
        const ma_device_channel_config& channelConfig) -> bool {

        for (ma_uint32 i = 0; i < count; ++i) {
            char fallback[32];
            const char* portName = nullptr;

            const ma_uint32 providedNames =
                channelConfig.customPortNameCount
                    ? channelConfig.customPortNameCount
                    : channelConfig.channels;

            if (channelConfig.customPortNames &&
                i < providedNames &&
                channelConfig.customPortNames[i] &&
                channelConfig.customPortNames[i][0]) {
                portName = channelConfig.customPortNames[i];
            } else {
                std::snprintf(
                    fallback,
                    sizeof(fallback),
                    "%s_%02u",
                    isCapture ? "input" : "output",
                    i
                );

                portName = fallback;
            }

            struct pw_properties* portProps = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, portName,
                nullptr
            );

            if (!portProps) return false;

            void* port = pw_filter_add_port(
                pDevice->filter,
                isCapture ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                0,
                portProps,
                nullptr,
                0
            );

            if (!port) return false;

            if (isCapture) {
                pDevice->in_ports.push_back(port);
            } else {
                pDevice->out_ports.push_back(port);
            }
        }

        return true;
    };

    if (wantsCapture &&
        !addPorts(true, capturePortCount, pDevice->capture)) {
        ma_pw_device_clear_resources(pDevice);
        return MA_ERROR;
    }

    if (wantsPlayback &&
        !addPorts(false, playbackPortCount, pDevice->playback)) {
        ma_pw_device_clear_resources(pDevice);
        return MA_ERROR;
    }

    if (pw_filter_connect(
            pDevice->filter,
            static_cast<pw_filter_flags>(
                PW_FILTER_FLAG_RT_PROCESS |
                PW_FILTER_FLAG_INACTIVE
            ),
            nullptr,
            0) < 0) {
        ma_pw_device_clear_resources(pDevice);
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

inline ma_result ma_device_init_ex(
    const ma_backend*,
    ma_uint32,
    ma_context* pContext,
    const ma_device_config* pConfig,
    ma_device* pDevice)
{
    return ma_device_init(pContext, pConfig, pDevice);
}

inline ma_bool32 ma_device_is_started(const ma_device* pDevice)
{
    return pDevice &&
           pDevice->filter &&
           pw_filter_get_state(pDevice->filter, nullptr) ==
               PW_FILTER_STATE_STREAMING;
}

struct ma_pw_active_request {
    struct pw_filter* filter;
    bool active;
    int result;
};

inline int ma_pw_set_active_on_loop(
    struct spa_loop*,
    bool,
    uint32_t,
    const void*,
    size_t,
    void* userData)
{
    auto* request = static_cast<ma_pw_active_request*>(userData);

    if (!request || !request->filter) return -1;

    request->result = pw_filter_set_active(
        request->filter,
        request->active
    );

    return request->result;
}

inline ma_result ma_pw_set_filter_active(
    ma_device* pDevice,
    bool active)
{
    if (!pDevice || !pDevice->filter || !pDevice->loop) {
        return MA_ERROR;
    }

    // Before the loop thread starts, no other thread can be using the filter.
    if (!pDevice->loop_thread.joinable()) {
        return pw_filter_set_active(pDevice->filter, active) < 0
            ? MA_ERROR
            : MA_SUCCESS;
    }

    // Once the main loop is running, serialize the operation on that loop.
    // pw_loop_lock() is not available in all PipeWire versions targeted by
    // this shim; pw_loop_invoke() is the loop-thread mechanism.
    ma_pw_active_request request{pDevice->filter, active, -1};

    const int invokeResult = pw_loop_invoke(
        pw_main_loop_get_loop(pDevice->loop),
        ma_pw_set_active_on_loop,
        0,
        nullptr,
        0,
        true,
        &request
    );

    if (invokeResult < 0 || request.result < 0) {
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

inline ma_result ma_device_start(ma_device* pDevice)
{
    if (!pDevice || !pDevice->filter || !pDevice->loop) {
        return MA_ERROR;
    }

    if (pDevice->loop_thread.joinable()) {
        // The main loop remains running while inactive, so reactivate in place.
        return ma_pw_set_filter_active(pDevice, true);
    }

    if (pw_filter_set_active(pDevice->filter, true) < 0) {
        return MA_ERROR;
    }

    try {
        pDevice->loop_thread = std::thread([pDevice]() {
            pw_main_loop_run(pDevice->loop);
        });
    } catch (...) {
        pw_filter_set_active(pDevice->filter, false);
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

inline ma_result ma_device_stop(ma_device* pDevice)
{
    if (!pDevice || !pDevice->filter || !pDevice->loop) {
        return MA_SUCCESS;
    }

    return ma_pw_set_filter_active(pDevice, false);
}

inline void ma_device_uninit(ma_device* pDevice)
{
    if (!pDevice) return;

    ma_device_stop(pDevice);

    if (pDevice->loop) {
        pw_main_loop_quit(pDevice->loop);
    }

    if (pDevice->loop_thread.joinable()) {
        pDevice->loop_thread.join();
    }

    ma_pw_device_clear_resources(pDevice);

    pDevice->dataCallback = nullptr;
    pDevice->pUserData = nullptr;
    pDevice->type = static_cast<ma_device_type>(0);
    pDevice->sampleRate = 0;

    pDevice->graphSampleRate.store(0, std::memory_order_relaxed);
    pDevice->sampleRateMismatch.store(0, std::memory_order_relaxed);
}

#endif // MINIAUDIO_H
