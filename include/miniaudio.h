#ifndef MINIAUDIO_H
#define MINIAUDIO_H

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <thread>

// ==========================================
// 1. TYPES & CONSTANTS STUBS
// ==========================================
typedef uint8_t  ma_uint8;
typedef uint16_t ma_uint16;
typedef uint32_t ma_uint32;
typedef uint64_t ma_uint64;
typedef int8_t   ma_int8;
typedef int16_t  ma_int16;
typedef int32_t  ma_int32;
typedef int64_t  ma_int64;
typedef int32_t  ma_result;
typedef int32_t  ma_bool32;

#define MA_SUCCESS 0
#define MA_ERROR -1
#define MA_MAX_CHANNELS 256
#define MA_SIMD_ALIGNMENT 64

typedef enum {
    ma_device_type_playback = 1,
    ma_device_type_capture  = 2,
    ma_device_type_duplex   = 3
} ma_device_type;

typedef enum {
    ma_format_unknown = 0,
    ma_format_u8      = 1,
    ma_format_s16     = 2,
    ma_format_s24     = 3,
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
    ma_uint32 jack;
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

typedef void (*ma_device_data_callback)(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

struct ma_device_channel_config {
    ma_format format;
    ma_uint32 channels;
    const ma_device_id* pDeviceID;
    ma_share_mode shareMode;
    
    ma_device_id id;
    ma_uint32 internalChannels;
    ma_uint32 internalSampleRate;
    ma_format internalFormat;
    int channelMap[32];
    int internalChannelMap[32];
    const char* const* customPortNames;
};

struct ma_device_config {
    ma_device_type type;
    ma_uint32 sampleRate;
    ma_uint32 periodSizeInFrames;
    ma_device_channel_config playback;
    ma_device_channel_config capture;
    ma_device_data_callback dataCallback;
    void* pUserData;
};

struct ma_device {
    ma_device_type type;
    ma_uint32 sampleRate;
    ma_device_channel_config playback;
    ma_device_channel_config capture;
    ma_device_data_callback dataCallback;
    void* pUserData;

    // Native PipeWire Backing State
    struct pw_main_loop* loop;
    std::thread loop_thread;
    struct pw_filter* filter;
    std::vector<void*> in_ports;
    std::vector<void*> out_ports;
    std::vector<float> input_interleaved;
    std::vector<float> output_interleaved;
};

// ==========================================
// 2. PCM RING BUFFER IMPLEMENTATION
// ==========================================
struct ma_pcm_rb {
    ma_format format;
    ma_uint32 channels;
    ma_uint32 capacityFrames;
    ma_uint32 rIndex;
    ma_uint32 wIndex;
    ma_uint32 bytesPerFrame;
    std::vector<uint8_t> buffer;
};

inline ma_result ma_pcm_rb_init(ma_format format, ma_uint32 channels, ma_uint32 capacityFrames, void*, void*, ma_pcm_rb* pRB) {
    *pRB = ma_pcm_rb{};
    pRB->format = format;
    pRB->channels = channels;
    pRB->capacityFrames = capacityFrames;
    
    ma_uint32 sampleSize = 4;
    if (format == ma_format_s16) sampleSize = 2;
    else if (format == ma_format_u8) sampleSize = 1;
    
    pRB->bytesPerFrame = sampleSize * channels;
    pRB->buffer.resize(capacityFrames * pRB->bytesPerFrame, 0);
    return MA_SUCCESS;
}

inline void ma_pcm_rb_uninit(ma_pcm_rb* pRB) {
    if (pRB) pRB->buffer.clear();
}

inline ma_uint32 ma_pcm_rb_available_read(ma_pcm_rb* pRB) {
    if (pRB->wIndex >= pRB->rIndex) {
        return pRB->wIndex - pRB->rIndex;
    }
    return pRB->capacityFrames - (pRB->rIndex - pRB->wIndex);
}

inline ma_uint32 ma_pcm_rb_available_write(ma_pcm_rb* pRB) {
    return pRB->capacityFrames - ma_pcm_rb_available_read(pRB) - 1;
}

inline ma_result ma_pcm_rb_acquire_read(ma_pcm_rb* pRB, ma_uint32* pframeCount, void** ppData) {
    ma_uint32 available = ma_pcm_rb_available_read(pRB);
    ma_uint32 count = std::min(*pframeCount, available);
    *pframeCount = count;
    if (count == 0) {
        *ppData = nullptr;
        return MA_SUCCESS;
    }
    *ppData = &pRB->buffer[pRB->rIndex * pRB->bytesPerFrame];
    return MA_SUCCESS;
}

inline ma_result ma_pcm_rb_commit_read(ma_pcm_rb* pRB, ma_uint32 frameCount) {
    pRB->rIndex = (pRB->rIndex + frameCount) % pRB->capacityFrames;
    return MA_SUCCESS;
}

inline ma_result ma_pcm_rb_acquire_write(ma_pcm_rb* pRB, ma_uint32* pframeCount, void** ppData) {
    ma_uint32 available = ma_pcm_rb_available_write(pRB);
    ma_uint32 count = std::min(*pframeCount, available);
    *pframeCount = count;
    if (count == 0) {
        *ppData = nullptr;
        return MA_SUCCESS;
    }
    *ppData = &pRB->buffer[pRB->wIndex * pRB->bytesPerFrame];
    return MA_SUCCESS;
}

inline ma_result ma_pcm_rb_commit_write(ma_pcm_rb* pRB, ma_uint32 frameCount) {
    pRB->wIndex = (pRB->wIndex + frameCount) % pRB->capacityFrames;
    return MA_SUCCESS;
}

// ==========================================
// 3. CONTEXT & DYNAMIC PIPEWIRE ENUMERATION
// ==========================================
inline ma_uint32 ma_get_bytes_per_frame(ma_format format, ma_uint32 channels) {
    ma_uint32 sampleSize = 4;
    if (format == ma_format_s16) sampleSize = 2;
    else if (format == ma_format_u8) sampleSize = 1;
    return sampleSize * channels;
}

inline const char* ma_get_format_name(ma_format format) {
    switch (format) {
        case ma_format_f32: return "f32";
        case ma_format_s32: return "s32";
        case ma_format_s24: return "s24";
        case ma_format_s16: return "s16";
        case ma_format_u8:  return "u8";
        default:            return "unknown";
    }
}

inline const char* ma_result_description(ma_result result) {
    if (result == MA_SUCCESS) return "MA_SUCCESS";
    return "MA_ERROR";
}

inline const char* ma_get_backend_name(ma_backend) {
    return "PipeWire-Native";
}

inline ma_context_config ma_context_config_init() {
    return ma_context_config{};
}

struct ContextEnumData {
    std::vector<ma_device_info>* playback;
    std::vector<ma_device_info>* capture;
};

static void enum_registry_global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const struct spa_dict *props) {
    auto* edata = static_cast<ContextEnumData*>(data);
    if (!props || !type) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* media_class = spa_dict_lookup(props, "media.class");
        const char* name = spa_dict_lookup(props, "node.name");
        const char* desc = spa_dict_lookup(props, "node.description");
        const char* channels_str = spa_dict_lookup(props, "audio.channels");
        if (!media_class) return;

        ma_uint32 channels = 2;
        if (channels_str) {
            try { channels = std::stoul(channels_str); } catch (...) {}
        }

        std::string mclass(media_class);
        bool is_sink = (mclass.find("Audio/Sink") != std::string::npos || mclass.find("Stream/Output/Audio") != std::string::npos);
        bool is_source = (mclass.find("Audio/Source") != std::string::npos || mclass.find("Stream/Input/Audio") != std::string::npos);

        if (is_sink || is_source) {
            ma_device_info info = ma_device_info{};
            const char* devName = desc ? desc : (name ? name : "PipeWire Audio Node");
            std::snprintf(info.name, sizeof(info.name), "%s", devName);
            info.id.jack = id;
            std::snprintf(info.id.alsa, sizeof(info.id.alsa), "%u", id);

            info.nativeDataFormatCount = 1;
            info.nativeDataFormats[0] = { ma_format_f32, channels, 48000, 0 };

            if (is_sink && edata->playback) {
                edata->playback->push_back(info);
            } else if (is_source && edata->capture) {
                edata->capture->push_back(info);
            }
        }
    }
}

static void enum_core_done(void *, uint32_t, int) {}

static const struct pw_registry_events enum_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = enum_registry_global,
};

static const struct pw_core_events enum_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = enum_core_done,
};

inline ma_result ma_context_init(const ma_backend*, ma_uint32, const void*, ma_context* pContext) {
    if (!pContext) return MA_ERROR;
    *pContext = ma_context{};
    pContext->backend = ma_backend_pipewire;

    pw_init(NULL, NULL);
    struct pw_main_loop* enum_loop = pw_main_loop_new(NULL);
    if (!enum_loop) return MA_ERROR;

    struct pw_context* enum_ctx = pw_context_new(pw_main_loop_get_loop(enum_loop), NULL, 0);
    if (!enum_ctx) {
        pw_main_loop_destroy(enum_loop);
        return MA_ERROR;
    }

    struct pw_core* enum_core = pw_context_connect(enum_ctx, NULL, 0);
    if (!enum_core) {
        pw_context_destroy(enum_ctx);
        pw_main_loop_destroy(enum_loop);
        return MA_ERROR;
    }

    struct pw_registry* enum_reg = pw_core_get_registry(enum_core, PW_VERSION_REGISTRY, 0);
    if (!enum_reg) {
        pw_core_disconnect(enum_core);
        pw_context_destroy(enum_ctx);
        pw_main_loop_destroy(enum_loop);
        return MA_ERROR;
    }

    ContextEnumData edata{&pContext->playbackDevices, &pContext->captureDevices};
    struct spa_hook reg_listener;
    struct spa_hook core_listener;

    pw_registry_add_listener(enum_reg, &reg_listener, &enum_registry_events, &edata);
    pw_core_add_listener(enum_core, &core_listener, &enum_core_events, enum_loop);

    pw_core_sync(enum_core, PW_ID_CORE, 0);
    
    auto start_time = std::chrono::steady_clock::now();
    while (pw_main_loop_get_loop(enum_loop) && std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(100)) {
        pw_loop_iterate(pw_main_loop_get_loop(enum_loop), 10);
    }

    spa_hook_remove(&reg_listener);
    spa_hook_remove(&core_listener);
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(enum_reg));
    pw_core_disconnect(enum_core);
    pw_context_destroy(enum_ctx);
    pw_main_loop_destroy(enum_loop);

    return MA_SUCCESS;
}

inline ma_result ma_context_get_devices(ma_context* pContext, ma_device_info** ppPlaybackDeviceInfos, ma_uint32* pPlaybackDeviceCount, ma_device_info** ppCaptureDeviceInfos, ma_uint32* pCaptureDeviceCount) {
    if (!pContext) return MA_ERROR;
    if (ppPlaybackDeviceInfos && pPlaybackDeviceCount) {
        *ppPlaybackDeviceInfos = pContext->playbackDevices.data();
        *pPlaybackDeviceCount = static_cast<ma_uint32>(pContext->playbackDevices.size());
    }
    if (ppCaptureDeviceInfos && pCaptureDeviceCount) {
        *ppCaptureDeviceInfos = pContext->captureDevices.data();
        *pCaptureDeviceCount = static_cast<ma_uint32>(pContext->captureDevices.size());
    }
    return MA_SUCCESS;
}

inline ma_result ma_context_get_device_info(ma_context* pContext, ma_device_type type, const ma_device_id* pDeviceID, ma_device_info* pDeviceInfo) {
    if (!pContext || !pDeviceInfo) return MA_ERROR;
    *pDeviceInfo = ma_device_info{};

    const auto& list = (type & ma_device_type_capture) ? pContext->captureDevices : pContext->playbackDevices;
    for (const auto& dev : list) {
        if (!pDeviceID || std::memcmp(&dev.id, pDeviceID, sizeof(ma_device_id)) == 0) {
            *pDeviceInfo = dev;
            return MA_SUCCESS;
        }
    }

    pDeviceInfo->nativeDataFormatCount = 1;
    pDeviceInfo->nativeDataFormats[0] = { ma_format_f32, 16, 48000, 0 };
    std::snprintf(pDeviceInfo->name, sizeof(pDeviceInfo->name), "PipeWire Default Device");
    return MA_SUCCESS;
}

inline ma_result ma_context_uninit(ma_context* pContext) {
    if (pContext) {
        pContext->playbackDevices.clear();
        pContext->captureDevices.clear();
    }
    return MA_SUCCESS;
}

inline ma_device_config ma_device_config_init(ma_device_type type) {
    ma_device_config config = ma_device_config{};
    config.type = type;
    config.sampleRate = 48000;
    config.playback.format = ma_format_f32;
    config.playback.channels = 16;
    config.capture.format = ma_format_f32;
    config.capture.channels = 16;
    config.playback.shareMode = ma_share_mode_shared;
    config.capture.shareMode = ma_share_mode_shared;
    return config;
}

// ==========================================
// 4. NATIVE PIPEWIRE DUPLEX BACKEND SHIM
// ==========================================
inline void pw_shim_on_process(void *userdata, struct spa_io_position *position) {
    struct ma_device *pDevice = (struct ma_device *)userdata;
    if (!position) return;

    ma_uint32 n_samples = position->clock.duration;
    ma_uint32 capture_channels = (pDevice->type & ma_device_type_capture) ? pDevice->capture.channels : 0;
    ma_uint32 playback_channels = (pDevice->type & ma_device_type_playback) ? pDevice->playback.channels : 0;

    const void* pInput = nullptr;
    void* pOutput = nullptr;

    if (capture_channels > 0 && !pDevice->in_ports.empty()) {
        pDevice->input_interleaved.resize(n_samples * capture_channels);
        for (ma_uint32 c = 0; c < capture_channels; ++c) {
            float *in_channel = (float *)pw_filter_get_dsp_buffer(pDevice->in_ports[c], n_samples);
            if (in_channel != nullptr) {
                for (ma_uint32 s = 0; s < n_samples; ++s) {
                    pDevice->input_interleaved[s * capture_channels + c] = in_channel[s];
                }
            } else {
                for (ma_uint32 s = 0; s < n_samples; ++s) {
                    pDevice->input_interleaved[s * capture_channels + c] = 0.0f;
                }
            }
        }
        pInput = pDevice->input_interleaved.data();
    }

    if (playback_channels > 0 && !pDevice->out_ports.empty()) {
        pDevice->output_interleaved.resize(n_samples * playback_channels, 0.0f);
        pOutput = pDevice->output_interleaved.data();
    }

    if (pDevice->dataCallback) {
        pDevice->dataCallback(pDevice, pOutput, pInput, n_samples);
    }

    if (playback_channels > 0 && !pDevice->out_ports.empty() && pOutput != nullptr) {
        for (ma_uint32 c = 0; c < playback_channels; ++c) {
            float *out_channel = (float *)pw_filter_get_dsp_buffer(pDevice->out_ports[c], n_samples);
            if (out_channel != nullptr) {
                for (ma_uint32 s = 0; s < n_samples; ++s) {
                    out_channel[s] = pDevice->output_interleaved[s * playback_channels + c];
                }
            }
        }
    }
}

static const struct pw_filter_events pw_shim_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = pw_shim_on_process,
};

inline ma_result ma_device_init(ma_context*, const ma_device_config* pConfig, ma_device* pDevice) {
    *pDevice = ma_device{};
    pDevice->type = pConfig->type;
    pDevice->sampleRate = pConfig->sampleRate;
    pDevice->playback = pConfig->playback;
    pDevice->capture = pConfig->capture;
    pDevice->dataCallback = pConfig->dataCallback;
    pDevice->pUserData = pConfig->pUserData;

    if (pDevice->playback.internalChannels == 0) pDevice->playback.internalChannels = pDevice->playback.channels;
    if (pDevice->playback.internalSampleRate == 0) pDevice->playback.internalSampleRate = pDevice->sampleRate;
    if (pDevice->playback.internalFormat == ma_format_unknown) pDevice->playback.internalFormat = pDevice->playback.format;

    if (pDevice->capture.internalChannels == 0) pDevice->capture.internalChannels = pDevice->capture.channels;
    if (pDevice->capture.internalSampleRate == 0) pDevice->capture.internalSampleRate = pDevice->sampleRate;
    if (pDevice->capture.internalFormat == ma_format_unknown) pDevice->capture.internalFormat = pDevice->capture.format;

    for (int i = 0; i < 32; ++i) {
        pDevice->playback.channelMap[i] = i;
        pDevice->playback.internalChannelMap[i] = i;
        pDevice->capture.channelMap[i] = i;
        pDevice->capture.internalChannelMap[i] = i;
    }

    static bool pw_initialized = false;
    if (!pw_initialized) {
        pw_init(NULL, NULL);
        pw_initialized = true;
    }

    pDevice->loop = pw_main_loop_new(NULL);
    if (!pDevice->loop) return MA_ERROR;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, "cm5audio_device",
        PW_KEY_NODE_DESCRIPTION, "CM5Audio Native PipeWire Device Shim",
        NULL
    );

    pDevice->filter = pw_filter_new_simple(
        pw_main_loop_get_loop(pDevice->loop),
        "cm5audio_device",
        props,
        &pw_shim_filter_events,
        pDevice
    );

    if (!pDevice->filter) {
        pw_main_loop_destroy(pDevice->loop);
        return MA_ERROR;
    }

    if (pDevice->type & ma_device_type_capture) {
        for (ma_uint32 i = 0; i < pDevice->capture.channels; ++i) {
            const char* portName = nullptr;
            char fallbackName[32];
            if (pDevice->capture.customPortNames && pDevice->capture.customPortNames[i]) {
                portName = pDevice->capture.customPortNames[i];
            } else {
                snprintf(fallbackName, sizeof(fallbackName), "input_%02d", i);
                portName = fallbackName;
            }
            struct pw_properties *in_props = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, portName,
                NULL
            );
            void *in_p = pw_filter_add_port(pDevice->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0, in_props, NULL, 0);
            pDevice->in_ports.push_back(in_p);
        }
    }

    if (pDevice->type & ma_device_type_playback) {
        for (ma_uint32 i = 0; i < pDevice->playback.channels; ++i) {
            const char* portName = nullptr;
            char fallbackName[32];
            if (pDevice->playback.customPortNames && pDevice->playback.customPortNames[i]) {
                portName = pDevice->playback.customPortNames[i];
            } else {
                snprintf(fallbackName, sizeof(fallbackName), "output_%02d", i);
                portName = fallbackName;
            }
            struct pw_properties *out_props = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, portName,
                NULL
            );
            void *out_p = pw_filter_add_port(pDevice->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0, out_props, NULL, 0);
            pDevice->out_ports.push_back(out_p);
        }
    }

    if (pw_filter_connect(pDevice->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
        pw_filter_destroy(pDevice->filter);
        pw_main_loop_destroy(pDevice->loop);
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

inline ma_result ma_device_init_ex(const ma_backend*, ma_uint32, ma_context* pContext, const ma_device_config* pConfig, ma_device* pDevice) {
    return ma_device_init(pContext, pConfig, pDevice);
}

inline ma_bool32 ma_device_is_started(const ma_device* pDevice) {
    return pDevice && pDevice->filter && pw_filter_get_state(pDevice->filter, nullptr) == PW_FILTER_STATE_STREAMING;
}

inline ma_result ma_device_start(ma_device* pDevice) {
    if (!pDevice || !pDevice->filter || !pDevice->loop) return MA_ERROR;

    if (pw_filter_set_active(pDevice->filter, true) < 0) {
        return MA_ERROR;
    }

    pDevice->loop_thread = std::thread([pDevice]() {
        pw_main_loop_run(pDevice->loop);
    });

    return MA_SUCCESS;
}

inline ma_result ma_device_stop(ma_device* pDevice) {
    if (!pDevice || !pDevice->loop) return MA_SUCCESS;

    pw_main_loop_quit(pDevice->loop);

    if (pDevice->loop_thread.joinable()) {
        pDevice->loop_thread.join();
    }
    return MA_SUCCESS;
}

inline void ma_device_uninit(ma_device* pDevice) {
    if (!pDevice) return;
    ma_device_stop(pDevice);

    if (pDevice->filter) {
        pw_filter_destroy(pDevice->filter);
        pDevice->filter = nullptr;
    }
    if (pDevice->loop) {
        pw_main_loop_destroy(pDevice->loop);
        pDevice->loop = nullptr;
    }
}

#endif // MINIAUDIO_H
