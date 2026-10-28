#include "device_manager.h"
#include "dsp_engine.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <sstream>
#include <utility>
#include <iterator>
#include <set>
#include <vector>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/node.h>
#include <pipewire/proxy.h>
#include <pipewire/loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>
#include <spa/utils/defs.h>

namespace {
template <size_t N>
std::string text_id(const char (&value)[N]) {
    return std::string(value, std::find(value, value + N, '\0'));
}

// Never interpret the device-ID union as ALSA on a different backend. Keep raw
// ALSA/Pulse names compatible with saved configurations from earlier releases.
std::string stable_device_id(ma_backend backend, const ma_device_id& id) {
    switch (backend) {
        case ma_backend_pipewire: return "pipewire:" + text_id(id.coreaudio);
        case ma_backend_alsa:       return text_id(id.alsa);
        case ma_backend_pulseaudio: return text_id(id.pulse);
        case ma_backend_jack:       return "jack:" + std::to_string(id.jack);
        case ma_backend_null:       return id.nullbackend == 0 ? "" : "null:" + std::to_string(id.nullbackend);
        case ma_backend_coreaudio:  return "coreaudio:" + text_id(id.coreaudio);
        case ma_backend_sndio:      return "sndio:" + text_id(id.sndio);
        case ma_backend_audio4:     return "audio4:" + text_id(id.audio4);
        case ma_backend_oss:        return "oss:" + text_id(id.oss);
        case ma_backend_webaudio:   return "webaudio:" + text_id(id.webaudio);
        case ma_backend_winmm:      return "winmm:" + std::to_string(id.winmm);
        case ma_backend_aaudio:     return "aaudio:" + std::to_string(id.aaudio);
        case ma_backend_opensl:     return "opensl:" + std::to_string(id.opensl);
        case ma_backend_wasapi: {
            // Encode UTF-16 code units without locale-dependent conversions.
            std::string value = "wasapi:";
            constexpr char hex[] = "0123456789abcdef";
            for (auto unit : id.wasapi) {
                if (unit == 0) break;
                for (int shift = 12; shift >= 0; shift -= 4) value += hex[(unit >> shift) & 15];
            }
            return value;
        }
        case ma_backend_dsound: {
            std::string value = "dsound:";
            constexpr char hex[] = "0123456789abcdef";
            for (auto byte : id.dsound) {
                value += hex[byte >> 4];
                value += hex[byte & 15];
            }
            return value;
        }
        // Custom IDs need a backend-specific contract; this app does not select
        // custom backends. Do not serialize arbitrary union bytes or pointers.
        case ma_backend_custom: return {};
    }
    return {};
}

std::string endpoint_id(const AudioDeviceInfo& info, ma_backend backend) {
    return info.stable_id.empty() ? stable_device_id(backend, info.id) : info.stable_id;
}

std::vector<NativeStreamFormat> native_formats(const ma_device_info& info) {
    std::vector<NativeStreamFormat> formats;
    const auto count = std::min<size_t>(info.nativeDataFormatCount, std::size(info.nativeDataFormats));
    formats.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& format = info.nativeDataFormats[i];
        formats.push_back({format.format, format.channels, format.sampleRate, format.flags});
    }
    return formats;
}

std::string backend_error(const char* operation, ma_result result) {
    return std::string(operation) + ": " + ma_result_description(result) +
           " (" + std::to_string(result) + ")";
}

uint64_t pipewire_property_u64(const struct spa_dict* props, const char* key) {
    const char* text = props ? spa_dict_lookup(props, key) : nullptr;
    if (!text || !*text) return 0;
    try {
        std::size_t used = 0;
        const auto value = std::stoull(text, &used, 10);
        return used == std::strlen(text) ? static_cast<uint64_t>(value) : 0;
    } catch (...) {
        return 0;
    }
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

struct PipeWireAudioLayout {
    unsigned channels = 0; // Zero preserves native/automatic negotiation.
    std::vector<uint32_t> positions;
};

PipeWireAudioLayout parse_pipewire_audio_layout(const struct spa_dict* props) {
    PipeWireAudioLayout layout;
    if (!props) return layout;

    if (const char* text = spa_dict_lookup(props, "audio.channels")) {
        try {
            std::size_t used = 0;
            const unsigned long value = std::stoul(text, &used);
            if (used == std::string(text).size() && value > 0 &&
                value <= SPA_AUDIO_MAX_CHANNELS && value <= CM5_MAX_CHANNELS) {
                layout.channels = static_cast<unsigned>(value);
            }
        } catch (...) {
            // Malformed metadata falls back to PipeWire's native negotiation.
        }
    }

    if (const char* text = spa_dict_lookup(props, "audio.position")) {
        std::string normalized;
        normalized.reserve(std::strlen(text));
        for (unsigned char ch : std::string(text)) {
            if (std::isspace(ch) || ch == '[' || ch == ']' ||
                ch == '"' || ch == 0x27) {
                continue;
            }
            normalized.push_back(static_cast<char>(ch));
        }

        std::stringstream input(normalized);
        std::string token;
        bool valid = !normalized.empty();
        while (valid && std::getline(input, token, ',')) {
            const uint32_t position =
                spa_type_audio_channel_from_short_name(token.c_str());
            if (token.empty() || position == SPA_AUDIO_CHANNEL_UNKNOWN) {
                valid = false;
                break;
            }
            layout.positions.push_back(position);
        }
        if (!valid || normalized.back() == ',')
            layout.positions.clear();
    }

    if (layout.channels == 0 && !layout.positions.empty())
        layout.channels = static_cast<unsigned>(layout.positions.size());

    if (layout.channels == 0 || layout.channels > SPA_AUDIO_MAX_CHANNELS ||
        layout.channels > CM5_MAX_CHANNELS) {
        layout.channels = 0;
        layout.positions.clear();
    } else if (layout.positions.size() != layout.channels) {
        // A channel count without a matching full position list is unpositioned.
        layout.positions.clear();
    }
    return layout;
}
} // namespace

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info, ma_context* context, const std::string& originalId)
    : m_info(info), m_context(context), m_originalId(originalId),
      m_requested_channels(info.requested_channels != 0 ? info.requested_channels : info.channels) {
    m_info.requested_channels = m_requested_channels;
}

HardwareDevice::~HardwareDevice() { stop(); }

void HardwareDevice::stop() {
    if (!m_device_initialized) return;
    ma_device_uninit(&m_device);
    m_device_initialized = false;
}

bool HardwareDevice::init() {
    m_last_error.clear();
    if (m_context == nullptr) return fail(&m_last_error, "Audio context is not initialized");
    if (m_device_initialized) {
        if (ma_device_is_started(&m_device)) return true;
        const auto result = ma_device_start(&m_device);
        if (result != MA_SUCCESS) return fail(&m_last_error, backend_error("Cannot restart device", result));
        return true;
    }
    if (m_requested_channels > MA_MAX_CHANNELS)
        return fail(&m_last_error, "Requested channels exceed the backend limit of " + std::to_string(MA_MAX_CHANNELS));

    bool widthMismatch = false;
    auto attempt_init = [&](const ma_device_id& deviceId) -> bool {
        ma_device_config config = ma_device_config_init(
            m_info.is_capture ? ma_device_type_capture : ma_device_type_playback);
        if (m_info.is_capture) {
            config.capture.format = ma_format_f32;
            config.capture.channels = m_requested_channels;
            config.capture.pDeviceID = &deviceId;
        } else {
            config.playback.format = ma_format_f32;
            config.playback.channels = m_requested_channels;
            config.playback.pDeviceID = &deviceId;
        }
        config.periodSizeInFrames = CM5_BUFFER_FRAMES;
        config.sampleRate = 48000;
        config.dataCallback = HardwareDevice::data_callback;
        config.pUserData = this;
        const auto initResult = ma_device_init(m_context, &config, &m_device);
        if (initResult != MA_SUCCESS)
            return fail(&m_last_error, backend_error("Cannot initialize device", initResult));
        m_device_initialized = true;

        m_info.id = m_info.is_capture ? m_device.capture.id : m_device.playback.id;
        const auto openedId = stable_device_id(m_context->backend, m_info.id);
        if (m_info.stable_id != openedId) m_info.native_formats.clear();
        m_info.stable_id = openedId;
        m_info.backend = ma_get_backend_name(m_context->backend);
        m_info.channels = m_info.is_capture ? m_device.capture.channels : m_device.playback.channels;
        m_info.sample_rate = m_device.sampleRate;
        m_info.internal_channels = m_info.is_capture ? m_device.capture.internalChannels : m_device.playback.internalChannels;
        m_info.internal_sample_rate = m_info.is_capture ? m_device.capture.internalSampleRate : m_device.playback.internalSampleRate;
        m_info.internal_format = m_info.is_capture ? m_device.capture.internalFormat : m_device.playback.internalFormat;
        const auto clientFormat = m_info.is_capture ? m_device.capture.format : m_device.playback.format;
        const auto* clientMap = m_info.is_capture ? m_device.capture.channelMap : m_device.playback.channelMap;
        const auto* internalMap = m_info.is_capture ? m_device.capture.internalChannelMap : m_device.playback.internalChannelMap;

        if (m_info.channels == 0 || m_info.channels > MA_MAX_CHANNELS ||
            m_info.internal_channels == 0 || m_info.internal_channels > MA_MAX_CHANNELS) {
            stop();
            return fail(&m_last_error, "Backend negotiated an invalid channel count");
        }
        try {
            m_info.client_channel_map.assign(clientMap, clientMap + m_info.channels);
            m_info.internal_channel_map.assign(internalMap, internalMap + m_info.internal_channels);
            if (m_info.channels != m_info.internal_channels ||
                (m_requested_channels != 0 && m_info.channels != m_requested_channels)) {
                widthMismatch = true;
                stop();
                return fail(&m_last_error, "Channel request " + std::to_string(m_requested_channels) +
                    " negotiated client=" + std::to_string(m_info.channels) +
                    ", backend=" + std::to_string(m_info.internal_channels) +
                    "; refusing channel remix. Select a supported width (OS audio profiles are managed externally)");
            }
            if (clientFormat != ma_format_f32 || m_info.sample_rate != 48000) {
                stop();
                return fail(&m_last_error, "Backend did not provide the required f32/48000 client stream");
            }
            if (!m_input_rb.init(ma_format_f32, m_info.channels, CM5_BUFFER_FRAMES * 3) ||
                !m_output_rb.init(ma_format_f32, m_info.channels, CM5_BUFFER_FRAMES * 3)) {
                stop();
                return fail(&m_last_error, "Cannot allocate device ring buffers");
            }
            io_buffer.assign(static_cast<size_t>(CM5_BUFFER_FRAMES) * m_info.channels, 0.0f);
        } catch (const std::exception& error) {
            stop();
            return fail(&m_last_error, std::string("Cannot prepare device buffers: ") + error.what());
        }
        const auto startResult = ma_device_start(&m_device);
        if (startResult != MA_SUCCESS) {
            stop();
            return fail(&m_last_error, backend_error("Cannot start device", startResult));
        }
        m_last_error.clear();
        return true;
    };

    const ma_device_id primaryId = m_info.id;
    if (attempt_init(primaryId)) return true;
    if (m_context->backend != ma_backend_alsa || widthMismatch) return false;
    std::string plughwStr = text_id(primaryId.alsa);
    if (plughwStr.find("plughw:") == 0) return false;
    if (plughwStr.find("hw:") == 0) plughwStr = "plughw" + plughwStr.substr(2);
    else if (!plughwStr.empty() && plughwStr[0] == ':') plughwStr = "plughw" + plughwStr;
    else plughwStr = "plughw:" + plughwStr;
    ma_device_id fallbackId{};
    std::strncpy(fallbackId.alsa, plughwStr.c_str(), sizeof(fallbackId.alsa) - 1);
    const std::string primaryError = m_last_error;
    if (attempt_init(fallbackId)) return true;
    m_last_error = primaryError + "; ALSA fallback " + plughwStr + ": " + m_last_error;
    return false;
}

void HardwareDevice::data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* device = static_cast<HardwareDevice*>(pDevice->pUserData);
    const size_t bytesNeeded = static_cast<size_t>(frameCount) * device->m_info.channels * sizeof(float);
    if (device->m_info.is_capture && pInput) {
        device->m_input_rb.write(pInput, bytesNeeded);
    } else if (!device->m_info.is_capture && pOutput) {
        const size_t got = device->m_output_rb.read(pOutput, bytesNeeded);
        if (got < bytesNeeded) std::memset(static_cast<unsigned char*>(pOutput) + got, 0, bytesNeeded - got);
    }
}

// PipeWire Monitor Stream Callbacks
/*static void on_process_monitor(void* userdata) {
    auto* monitor = static_cast<NodeMonitor*>(userdata);
    struct pw_buffer* b = pw_stream_dequeue_buffer(monitor->stream);
    if (!b) return;

    struct spa_buffer* buf = b->buffer;
    if (buf->datas[0].data == nullptr) {
        pw_stream_queue_buffer(monitor->stream, b);
        return;
    }

    float* samples = static_cast<float*>(buf->datas[0].data);
    uint32_t stride = buf->datas[0].chunk->stride / sizeof(float);
    if (stride == 0) stride = monitor->channels;
    uint32_t n_frames = buf->datas[0].chunk->size / (stride * sizeof(float));

    if (monitor->dsp_engine && n_frames > 0) {
        monitor->dsp_engine->update_meters(samples, monitor->channels, stride, monitor->endpoint_name, true);
    }

    pw_stream_queue_buffer(monitor->stream, b);
}

static const struct pw_stream_events monitor_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process_monitor,
};*/

static void on_stream_state_changed(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    auto* monitor = static_cast<NodeMonitor*>(userdata);
    std::cout << "[PipeWire Monitor] Node " << monitor->node_id 
              << " state changed: " << pw_stream_state_as_string(old) 
              << " -> " << pw_stream_state_as_string(state);
    if (error) {
        std::cout << " [ERROR: " << error << "]";
    }
    std::cout << std::endl;
}

static void on_monitor_param_changed(void* userdata, uint32_t id,
                                     const struct spa_pod* param) {
    if (id != SPA_PARAM_Format || !param) return;
    auto* monitor = static_cast<NodeMonitor*>(userdata);
    struct spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) < 0 ||
        info.format != SPA_AUDIO_FORMAT_F32 ||
        info.channels == 0 || info.channels > CM5_MAX_CHANNELS) {
        std::cerr << "[PipeWire Monitor] Unsupported negotiated format for node "
                  << monitor->node_id << std::endl;
        return;
    }
    monitor->channels.store(info.channels, std::memory_order_release);
    std::cerr << "[FORMAT-DEBUG negotiated] Node " << monitor->node_id
              << " channels=" << info.channels
              << " rate=" << info.rate
              << " format=" << static_cast<unsigned>(info.format)
              << " positions=";
    for (uint32_t i = 0; i < info.channels; ++i) {
        std::cerr << (i ? "," : "") << spa_type_audio_channel_to_short_name(info.position[i]);
    }
    std::cerr << std::endl;
    if (monitor->dsp_engine) {
        monitor->dsp_engine->register_pipewire_meter_bank(
            monitor->endpoint_name, info.channels, monitor->is_capture);
    }
}

static void on_process_monitor(void* userdata) {
    auto* monitor = static_cast<NodeMonitor*>(userdata);
    struct pw_buffer* b = pw_stream_dequeue_buffer(monitor->stream);
    if (!b) return;

    auto queue_buffer = [&]() { pw_stream_queue_buffer(monitor->stream, b); };
    if (!b->buffer || b->buffer->n_datas == 0) {
        queue_buffer();
        return;
    }

    struct spa_data* data = &b->buffer->datas[0];
    if (!data->data || !data->chunk) {
        queue_buffer();
        return;
    }

    const size_t offset = data->chunk->offset;
    const size_t bytes = data->chunk->size;
    if (offset > data->maxsize || bytes > data->maxsize - offset) {
        queue_buffer();
        return;
    }

    const unsigned channels = monitor->channels.load(std::memory_order_acquire);
    if (channels == 0 || channels > CM5_MAX_CHANNELS) {
        queue_buffer();
        return;
    }

    const size_t strideBytes = data->chunk->stride > 0
        ? static_cast<size_t>(data->chunk->stride)
        : static_cast<size_t>(channels) * sizeof(float);
    if (strideBytes < static_cast<size_t>(channels) * sizeof(float) ||
        strideBytes % sizeof(float) != 0) {
        queue_buffer();
        return;
    }

    const size_t frames = bytes / strideBytes;
    auto* samples = reinterpret_cast<float*>(
        static_cast<uint8_t*>(data->data) + offset);

    if (monitor->dsp_engine && frames > 0) {
        monitor->dsp_engine->update_meters(
            samples, channels, static_cast<unsigned>(strideBytes / sizeof(float)),
            monitor->endpoint_name, monitor->is_capture, frames);
    }
    queue_buffer();
}

static const struct pw_stream_events monitor_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_monitor_param_changed,
    .process = on_process_monitor,
};

void DeviceManager::start_node_monitor(uint32_t nodeId, const std::string& name,
                                       bool isCapture, unsigned channels,
                                       std::vector<uint32_t> positions) {
    if (!m_pw_enabled || !m_pw_loop || !m_pw_thread.joinable()) return;

    MonitorLoopRequest request;
    request.manager = this;
    request.start = true;
    request.node_id = nodeId;
    request.name = name;
    request.is_capture = isCapture;
    request.channels = channels;
    request.positions = std::move(positions);

    std::lock_guard<std::mutex> invokeLock(m_monitorInvokeLock);
    const int result = pw_loop_invoke(
        pw_main_loop_get_loop(m_pw_loop), &DeviceManager::invoke_monitor_operation,
        0, nullptr, 0, true, &request);
    if (result < 0) {
        std::cerr << "[PipeWire Monitor] Could not dispatch monitor start for node "
                  << nodeId << ": " << result << std::endl;
    }
}

void DeviceManager::stop_node_monitor(uint32_t nodeId) {
    if (!m_pw_enabled || !m_pw_loop || !m_pw_thread.joinable()) return;

    MonitorLoopRequest request;
    request.manager = this;
    request.start = false;
    request.node_id = nodeId;

    std::lock_guard<std::mutex> invokeLock(m_monitorInvokeLock);
    const int result = pw_loop_invoke(
        pw_main_loop_get_loop(m_pw_loop), &DeviceManager::invoke_monitor_operation,
        0, nullptr, 0, true, &request);
    if (result < 0) {
        std::cerr << "[PipeWire Monitor] Could not dispatch monitor stop for node "
                  << nodeId << ": " << result << std::endl;
    }
}

int DeviceManager::invoke_monitor_operation(
    struct spa_loop*, bool, uint32_t, const void*, std::size_t, void* user_data) {
    auto* request = static_cast<MonitorLoopRequest*>(user_data);
    if (!request || !request->manager) return -1;

    if (request->start) {
        request->manager->start_node_monitor_in_loop(
            request->node_id, request->name, request->is_capture,
            request->channels, request->positions);
    } else {
        request->manager->stop_node_monitor_in_loop(request->node_id);
    }
    return 0;
}

int DeviceManager::invoke_pipewire_cleanup(
    struct spa_loop*, bool, uint32_t, const void*, std::size_t, void* user_data) {
    auto* manager = static_cast<DeviceManager*>(user_data);
    if (!manager) return -1;

    for (auto& entry : manager->m_pw_node_info_watchers) {
        auto& watch = entry.second;
        spa_hook_remove(&watch->listener);
        if (watch->proxy) {
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(watch->proxy));
            watch->proxy = nullptr;
        }
    }
    manager->m_pw_node_info_watchers.clear();

    if (manager->m_pw_registry) {
        spa_hook_remove(&manager->m_registry_listener);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(manager->m_pw_registry));
        manager->m_pw_registry = nullptr;
    }
    return 0;
}

// This helper is only called from the PipeWire main-loop thread.
void DeviceManager::start_node_monitor_in_loop(
    uint32_t nodeId, const std::string& name, bool isCapture, unsigned channels,
    const std::vector<uint32_t>& positions) {
    std::lock_guard<std::mutex> lock(m_monitorLock);
    DspEngine* engine = m_dspEngine.load(std::memory_order_acquire);
    if (!m_pw_loop || !engine || m_monitors.find(nodeId) != m_monitors.end()) return;
    if (channels > SPA_AUDIO_MAX_CHANNELS || channels > CM5_MAX_CHANNELS) {
        std::cerr << "[PipeWire Monitor] Ignoring unsupported channel count "
                  << channels << " for node " << nodeId << std::endl;
        return;
    }

    auto monitor = std::make_unique<NodeMonitor>();
    monitor->node_id = nodeId;
    monitor->endpoint_name = "pw-node/" + std::to_string(nodeId);
    monitor->dsp_engine = engine;
    monitor->channels.store(channels > 0 ? channels : 2, std::memory_order_relaxed);
    monitor->is_capture = isCapture;

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_STREAM_MONITOR, "true",
        PW_KEY_STREAM_DONT_REMIX, "true",
        PW_KEY_APP_NAME, "cm5audio-meter",
        PW_KEY_TARGET_OBJECT, name.c_str(),
        nullptr);
    if (!props) return;
    if (!isCapture) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    const std::string streamName = "cm5-meter-" + std::to_string(nodeId);
    monitor->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(m_pw_loop), streamName.c_str(), props,
        &monitor_stream_events, monitor.get());
    if (!monitor->stream) return;

    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_audio_info_raw audioInfo{};
    audioInfo.format = SPA_AUDIO_FORMAT_F32;
    if (channels > 0) {
        audioInfo.channels = channels;
        if (positions.size() == channels) {
            std::copy(positions.begin(), positions.end(), audioInfo.position);
        } else {
            audioInfo.flags |= SPA_AUDIO_FLAG_UNPOSITIONED;
        }
    }

    std::cerr << "[FORMAT-DEBUG requested] Node " << nodeId
              << " channels=" << (channels > 0 ? channels : 2)
              << " position_count=" << positions.size()
              << " capture=" << (isCapture ? "true" : "false")
              << std::endl;
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat, &audioInfo);
    if (!params[0]) {
        pw_stream_destroy(monitor->stream);
        return;
    }

    // Register initial meters; param_changed updates the bank to the negotiated width.
    monitor->dsp_engine->register_pipewire_meter_bank(
        monitor->endpoint_name,
        monitor->channels.load(std::memory_order_relaxed), monitor->is_capture);

    const int result = pw_stream_connect(
        monitor->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (result < 0) {
        std::cerr << "[PipeWire Monitor] Failed to connect node " << nodeId
                  << ": " << result << std::endl;
        pw_stream_destroy(monitor->stream);
        monitor->dsp_engine->unregister_pipewire_meter_bank(
            monitor->endpoint_name, monitor->is_capture);
        return;
    }
    m_monitors[nodeId] = std::move(monitor);
}

void DeviceManager::stop_node_monitor_in_loop(uint32_t nodeId) {
    std::lock_guard<std::mutex> lock(m_monitorLock);
    auto it = m_monitors.find(nodeId);
    if (it == m_monitors.end()) return;

    auto& monitor = it->second;
    if (monitor->stream) {
        pw_stream_destroy(monitor->stream);
        monitor->stream = nullptr;
    }
    if (monitor->dsp_engine) {
        monitor->dsp_engine->unregister_pipewire_meter_bank(
            monitor->endpoint_name, monitor->is_capture);
    }
    m_monitors.erase(it);
}

void pipewire_node_info_changed(void* data, const struct pw_node_info* info) {
    auto* watch = static_cast<DeviceManager::PipeWireNodeInfoWatch*>(data);
    if (!watch || !watch->manager || !info) return;

    // Node info also reports state/port changes. Its properties are valid
    // for layout parsing only when the properties change bit is set.
    if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) return;

    DeviceManager* manager = watch->manager;
    const uint32_t nodeId = watch->node_id;
    const PipeWireAudioLayout layout = parse_pipewire_audio_layout(info->props);
    std::string nodeName;
    bool isCapture = true;
    bool wasResolved = false;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(manager->m_pw_graph_lock);
        auto it = manager->m_pw_graph.nodes.find(nodeId);
        if (it == manager->m_pw_graph.nodes.end()) return;

        PipeWireNode& node = it->second;
        wasResolved = node.audio_layout_resolved;
        changed = !wasResolved || node.audio_channels != layout.channels ||
                  node.audio_positions != layout.positions;
        node.audio_channels = layout.channels;
        node.audio_positions = layout.positions;
        node.audio_layout_resolved = true;
        nodeName = node.name;
        isCapture = !node.is_input;
    }

    // pw_node_info.props contains the actual node properties. The registry's
    // initial global dictionary may not include audio.channels/audio.position.
    if (!changed) return;
    if (wasResolved) manager->stop_node_monitor_in_loop(nodeId);
    if (manager->m_dspEngine.load(std::memory_order_acquire) &&
        nodeName.find("cm5-meter") == std::string::npos) {
        manager->start_node_monitor_in_loop(
            nodeId, nodeName, isCapture, layout.channels, layout.positions);
    }
}

static const struct pw_node_events node_info_events = {
    PW_VERSION_NODE_EVENTS,
    .info = pipewire_node_info_changed,
};

void DeviceManager::refresh_pipewire_process_ids_locked() {
    for (auto& [nodeId, node] : m_pw_graph.nodes) {
        (void)nodeId;
        node.process_id = node.reported_process_id;
        if (node.process_id != 0) continue;

        const PipeWireClientInfo* matched = nullptr;
        if (node.client_id != 0) {
            const auto it = m_pw_clients_by_id.find(node.client_id);
            if (it != m_pw_clients_by_id.end() && it->second.process_id != 0)
                matched = &it->second;
        }

        const std::string nodeClientName = !node.client_name.empty()
            ? node.client_name
            : (!node.app_name.empty() ? node.app_name : node.name);
        const auto namesMatch = [&node, &nodeClientName](const PipeWireClientInfo& client) {
            if (!nodeClientName.empty() &&
                (client.name == nodeClientName || client.app_name == nodeClientName))
                return true;
            return !node.app_name.empty() && !client.app_name.empty() &&
                   node.app_name == client.app_name;
        };

        if (!matched && node.object_serial > 0) {
            for (const auto& [clientId, client] : m_pw_clients_by_id) {
                (void)clientId;
                if (client.process_id == 0 || client.object_serial == 0) continue;
                if (client.object_serial + 1 != node.object_serial) continue;
                if (!namesMatch(client)) continue;
                matched = &client;
                break;
            }
        }

        if (!matched && !nodeClientName.empty()) {
            const PipeWireClientInfo* unique = nullptr;
            bool ambiguous = false;
            for (const auto& [clientId, client] : m_pw_clients_by_id) {
                (void)clientId;
                if (client.process_id == 0 || !namesMatch(client)) continue;
                if (node.object_serial > 0 && client.object_serial >= node.object_serial) continue;
                if (unique) { ambiguous = true; break; }
                unique = &client;
            }
            if (!ambiguous) matched = unique;
        }
        if (matched) node.process_id = matched->process_id;
    }
}

void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    auto* manager = static_cast<DeviceManager*>(data);
    std::lock_guard<std::mutex> lock(manager->m_pw_graph_lock);

    if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        PipeWireClientInfo client;
        const char* name = props ? spa_dict_lookup(props, PW_KEY_CLIENT_NAME) : nullptr;
        const char* app_name = props ? spa_dict_lookup(props, PW_KEY_APP_NAME) : nullptr;
        client.name = name ? name : "";
        client.app_name = app_name ? app_name : "";
        client.object_serial = pipewire_property_u64(props, "object.serial");
        client.process_id = pipewire_property_u64(props, PW_KEY_APP_PROCESS_ID);
        manager->m_pw_clients_by_id[id] = std::move(client);
        manager->refresh_pipewire_process_ids_locked();
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* name = props ? spa_dict_lookup(props, "node.name") : nullptr;
        const char* desc = props ? spa_dict_lookup(props, "node.description") : nullptr;
        const char* media_class = props ? spa_dict_lookup(props, "media.class") : nullptr;
        const char* app_name = props ? spa_dict_lookup(props, PW_KEY_APP_NAME) : nullptr;
        const char* client_name = props ? spa_dict_lookup(props, PW_KEY_CLIENT_NAME) : nullptr;
        const uint32_t client_id = static_cast<uint32_t>(pipewire_property_u64(props, PW_KEY_CLIENT_ID));
        const uint64_t object_serial = pipewire_property_u64(props, "object.serial");
        const uint64_t reported_process_id = pipewire_property_u64(props, PW_KEY_APP_PROCESS_ID);

        // Our monitor streams are implementation details, not patchbay nodes.
        // Hide them before adding them to the graph and remember their IDs so
        // their automatically-created links are hidden as well.
        const bool isMeterStream =
            (name && std::strstr(name, "cm5-meter") != nullptr) ||
            (app_name && std::strcmp(app_name, "cm5audio-meter") == 0);
        if (isMeterStream) {
            manager->m_pw_ignored_nodes.insert(id);
            manager->m_pw_graph.nodes.erase(id);
            manager->rebuild_pipewire_visible_links_locked();
            return;
        }

        const std::string mediaClass = media_class ? media_class : "";
        const PipeWireAudioLayout audioLayout = parse_pipewire_audio_layout(props);

        bool is_input = false;
        if (!mediaClass.empty()) {
            is_input = (mediaClass.find("Sink") != std::string::npos ||
                        mediaClass.find("Input") != std::string::npos);
        } else if (name) {
            const std::string n(name);
            is_input = (n.find("alsa_output") == 0);
        }

        manager->m_pw_graph.nodes[id] = {
            id,
            name ? name : "Unknown Node",
            desc ? desc : "",
            is_input,
            mediaClass,
            audioLayout.channels,
            audioLayout.positions,
            false,
            app_name ? app_name : "",
            client_name ? client_name : "",
            client_id,
            object_serial,
            reported_process_id,
            reported_process_id
        };
        manager->refresh_pipewire_process_ids_locked();
        manager->rebuild_pipewire_visible_links_locked();

        // Bind the actual node and wait for pw_node_info.props before creating
        // its meter. Registry global props do not reliably include audio layout.
        const std::string nodeName = name ? name : "";
        const bool isAudio = mediaClass.find("Audio") != std::string::npos;
        if (isAudio && nodeName.find("cm5-meter") == std::string::npos) {
            auto fallback_to_registry_layout = [&]() {
                auto nodeIt = manager->m_pw_graph.nodes.find(id);
                if (nodeIt != manager->m_pw_graph.nodes.end())
                    nodeIt->second.audio_layout_resolved = true;
                if (manager->m_dspEngine.load(std::memory_order_acquire)) {
                    manager->start_node_monitor_in_loop(
                        id, nodeName, !is_input, audioLayout.channels, audioLayout.positions);
                }
            };

            if (!manager->m_pw_registry) {
                fallback_to_registry_layout();
            } else if (manager->m_pw_node_info_watchers.find(id) ==
                       manager->m_pw_node_info_watchers.end()) {
                auto watch = std::make_unique<DeviceManager::PipeWireNodeInfoWatch>();
                watch->manager = manager;
                watch->node_id = id;
                const uint32_t bindVersion = std::min<uint32_t>(version, PW_VERSION_NODE);
                watch->proxy = static_cast<struct pw_node*>(pw_registry_bind(
                    manager->m_pw_registry, id, PW_TYPE_INTERFACE_Node, bindVersion, 0));

                if (!watch->proxy) {
                    std::cerr << "[PipeWire Monitor] Could not bind node info for "
                              << nodeName << " (" << id << "); using fallback layout" << std::endl;
                    fallback_to_registry_layout();
                } else {
                    auto* watchPtr = watch.get();
                    manager->m_pw_node_info_watchers.emplace(id, std::move(watch));
                    spa_zero(watchPtr->listener);
                    const int listenerResult = pw_node_add_listener(
                        watchPtr->proxy, &watchPtr->listener, &node_info_events, watchPtr);
                    if (listenerResult < 0) {
                        std::cerr << "[PipeWire Monitor] Could not listen for node info for "
                                  << nodeName << " (" << id << "): " << listenerResult
                                  << "; using fallback layout" << std::endl;
                        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(watchPtr->proxy));
                        watchPtr->proxy = nullptr;
                        manager->m_pw_node_info_watchers.erase(id);
                        fallback_to_registry_layout();
                    }
                }
            }
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        uint32_t src_node = 0, dst_node = 0, src_port = 0, dst_port = 0;
        if (props) {
            try {
                const char* sn = spa_dict_lookup(props, "link.output.node");
                const char* dn = spa_dict_lookup(props, "link.input.node");
                const char* sp = spa_dict_lookup(props, "link.output.port");
                const char* dp = spa_dict_lookup(props, "link.input.port");
                if (sn) src_node = std::stoul(sn);
                if (dn) dst_node = std::stoul(dn);
                if (sp) src_port = std::stoul(sp);
                if (dp) dst_port = std::stoul(dp);
            } catch (...) {
                return;
            }
        }
        manager->m_pw_links_by_id[id] = {src_port, dst_port, src_node, dst_node};
        manager->rebuild_pipewire_visible_links_locked();
    }
}

void registry_global_remove(void *data, uint32_t id) {
    auto* manager = static_cast<DeviceManager*>(data);

    auto watchIt = manager->m_pw_node_info_watchers.find(id);
    if (watchIt != manager->m_pw_node_info_watchers.end()) {
        auto& watch = watchIt->second;
        spa_hook_remove(&watch->listener);
        if (watch->proxy) {
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(watch->proxy));
            watch->proxy = nullptr;
        }
        manager->m_pw_node_info_watchers.erase(watchIt);
    }
    {
        std::lock_guard<std::mutex> lock(manager->m_pw_graph_lock);
        manager->m_pw_clients_by_id.erase(id);
        manager->m_pw_graph.nodes.erase(id);
        manager->refresh_pipewire_process_ids_locked();
        manager->m_pw_ignored_nodes.erase(id);
        manager->m_pw_links_by_id.erase(id); // The removed global may itself be a link.
        auto& linksById = manager->m_pw_links_by_id;
        for (auto it = linksById.begin(); it != linksById.end();) {
            if (it->second.source_node == id || it->second.dest_node == id)
                it = linksById.erase(it);
            else
                ++it;
        }
        manager->rebuild_pipewire_visible_links_locked();
    }
    // Registry removal callbacks already run in the PipeWire loop context.
    manager->stop_node_monitor_in_loop(id);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

DeviceManager::DeviceManager(bool initializeBackend, bool enablePwPatchbay)
    : m_pw_enabled(enablePwPatchbay) {
    if (m_pw_enabled) {
        std::cout << "[DeviceManager] Initializing PipeWire patchbay backend..." << std::endl;

        pw_init(nullptr, nullptr);

        m_pw_loop = pw_main_loop_new(NULL);
        if (m_pw_loop) {
            m_pw_context = pw_context_new(pw_main_loop_get_loop(m_pw_loop), NULL, 0);
            if (m_pw_context) {
                struct pw_core *core = pw_context_connect(m_pw_context, NULL, 0);
                if (core) {
                    m_pw_registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
                    if (m_pw_registry) {
                        spa_zero(m_registry_listener);
                        pw_registry_add_listener(m_pw_registry, &m_registry_listener, &registry_events, this);
                    }
                }
            }
        }
        if (!m_pw_loop || !m_pw_context) {
            std::cerr << "[DeviceManager] CRITICAL ERROR: Failed to initialize PipeWire context/loop!" << std::endl;
            m_pw_enabled = false;
        } else {
            m_pw_thread = std::thread([this]() {
                pw_main_loop_run(m_pw_loop);
            });
            std::cout << "[DeviceManager] PipeWire patchbay backend active." << std::endl;
        }
    }

    if (!initializeBackend) return;

    ma_backend backends[] = {
        ma_backend_pulseaudio,
    };

    std::cout << "[DeviceManager] Initializing backend search..." << std::endl;
    ma_context_config config = ma_context_config_init();
    if (ma_context_init(backends, std::size(backends), &config, &m_context) != MA_SUCCESS) {
        std::cerr << "[DeviceManager] CRITICAL ERROR: Failed to initialize any available audio backend!" << std::endl;
    } else {
        m_context_initialized = true;
        std::cout << "[DeviceManager] SUCCESS: Context initialized. Active Backend: " << ma_get_backend_name(m_context.backend) << std::endl;
    }
}

DeviceManager::~DeviceManager() {
    stop_all();
    set_dsp_engine(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_monitorLock);
        m_monitors.clear();
    }
    if (m_pw_enabled) {
        if (m_pw_loop) {
            if (m_pw_thread.joinable()) {
                int cleanupResult = -1;
                {
                    std::lock_guard<std::mutex> invokeLock(m_monitorInvokeLock);
                    cleanupResult = pw_loop_invoke(
                        pw_main_loop_get_loop(m_pw_loop), &DeviceManager::invoke_pipewire_cleanup,
                        0, nullptr, 0, true, this);
                }
                if (cleanupResult < 0) {
                    std::cerr << "[DeviceManager] Could not clean up PipeWire node proxies on the loop: "
                              << cleanupResult << std::endl;
                }
                pw_main_loop_quit(m_pw_loop);
                m_pw_thread.join();
            } else {
                invoke_pipewire_cleanup(nullptr, false, 0, nullptr, 0, this);
            }
            pw_main_loop_destroy(m_pw_loop);
        }
        if (m_pw_context) pw_context_destroy(m_pw_context);
    }
    if (m_context_initialized) {
        ma_context_uninit(&m_context);
        m_context_initialized = false;
    }
}

PipeWireGraph DeviceManager::get_pipewire_graph() {
    std::lock_guard<std::mutex> lock(m_pw_graph_lock);
    return m_pw_graph;
}

// Caller must hold m_pw_graph_lock. The public graph omits our meter streams,
// but their registry/link lifetimes remain tracked in m_pw_links_by_id.
void DeviceManager::rebuild_pipewire_visible_links_locked() {
    m_pw_graph.links.clear();
    m_pw_graph.links.reserve(m_pw_links_by_id.size());
    for (const auto& entry : m_pw_links_by_id) {
        const auto& link = entry.second;
        if (m_pw_ignored_nodes.count(link.source_node) ||
            m_pw_ignored_nodes.count(link.dest_node)) continue;
        if (!m_pw_graph.nodes.count(link.source_node) ||
            !m_pw_graph.nodes.count(link.dest_node)) continue;
        m_pw_graph.links.push_back(link);
    }
}

void DeviceManager::set_dsp_engine(DspEngine* engine) {
    DspEngine* previous = m_dspEngine.exchange(engine, std::memory_order_acq_rel);

    if (!engine) {
        std::vector<uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(m_monitorLock);
            ids.reserve(m_monitors.size());
            for (const auto& entry : m_monitors) ids.push_back(entry.first);
        }
        for (uint32_t id : ids) stop_node_monitor(id);
        return;
    }

    if (previous && previous != engine) {
        std::vector<uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(m_monitorLock);
            ids.reserve(m_monitors.size());
            for (const auto& entry : m_monitors) ids.push_back(entry.first);
        }
        for (uint32_t id : ids) stop_node_monitor(id);
    }

    // The registry may have discovered nodes before DspEngine was constructed.
    const PipeWireGraph graph = get_pipewire_graph();
    for (const auto& entry : graph.nodes) {
        const auto& node = entry.second;
        if (node.media_class.find("Audio") == std::string::npos ||
            node.name.find("cm5-meter") != std::string::npos ||
            !node.audio_layout_resolved) continue;
        start_node_monitor(node.id, node.name, !node.is_input,
                           node.audio_channels, node.audio_positions);
    }
}

void DeviceManager::enumerate_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_available_devices.clear();
    if (!m_context_initialized) return;
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playbackCount = 0, captureCount = 0;
    if (ma_context_get_devices(&m_context, &playback, &playbackCount, &capture, &captureCount) != MA_SUCCESS) return;
    const auto append = [&](const ma_device_info* devices, ma_uint32 count, bool isCapture) {
        static const std::set<std::string> virtualAlsaIds = {
            "null", "lavrate", "samplerate", "speexrate", "jack", "oss", "pulse", "speex", "upmix", "vdownmix"
        };
        for (ma_uint32 i = 0; i < count; ++i) {
            const auto& device = devices[i];
            const auto id = stable_device_id(m_context.backend, device.id);
            if (m_context.backend == ma_backend_alsa && virtualAlsaIds.count(id)) continue;
            AudioDeviceInfo info{};
            info.id = device.id;
            info.stable_id = id;
            info.backend = ma_get_backend_name(m_context.backend);
            info.name = device.name;
            info.is_capture = isCapture;
            m_available_devices.push_back(std::move(info));
        }
    };
    append(playback, playbackCount, false);
    append(capture, captureCount, true);
}

std::vector<NativeStreamFormat> DeviceManager::get_device_capabilities(
    const std::string& id, bool capture, std::string* error) {
    if (error) error->clear();
    AudioDeviceInfo selected;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_context_initialized) {
            fail(error, "Audio context is not initialized");
            return {};
        }
        const auto found = std::find_if(m_available_devices.begin(), m_available_devices.end(),
            [&](const AudioDeviceInfo& info) {
                return info.is_capture == capture && endpoint_id(info, m_context.backend) == id;
            });
        if (found == m_available_devices.end()) {
            fail(error, std::string(capture ? "Capture" : "Playback") + " device not found: " + id);
            return {};
        }
        selected = *found;
    }
    ma_device_info detailed{};
    const auto result = ma_context_get_device_info(&m_context,
        capture ? ma_device_type_capture : ma_device_type_playback, &selected.id, &detailed);
    if (result != MA_SUCCESS) {
        fail(error, backend_error("Cannot query device capabilities", result));
        return {};
    }
    auto formats = native_formats(detailed);
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& info : m_available_devices) {
            if (info.is_capture == capture && endpoint_id(info, m_context.backend) == id)
                info.native_formats = formats;
        }
    }
    return formats;
}

std::vector<AudioDeviceInfo> DeviceManager::get_available_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_available_devices;
}

std::vector<std::shared_ptr<HardwareDevice>> DeviceManager::get_active_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<HardwareDevice>> active;
    active.reserve(m_active_devices.size());
    for (auto const& entry : m_active_devices) active.push_back(entry.second);
    return active;
}

bool DeviceManager::activate_device(const std::string& id, unsigned captureChannels,
                                    unsigned playbackChannels, std::string* error) {
    if (error) error->clear();
    if (captureChannels > MA_MAX_CHANNELS || playbackChannels > MA_MAX_CHANNELS)
        return fail(error, "Requested channels exceed the backend limit of " + std::to_string(MA_MAX_CHANNELS));
    const auto requested = [&](bool capture) { return capture ? captureChannels : playbackChannels; };
    const auto activeMismatch = [&](const AudioDeviceInfo& info) {
        return requested(info.is_capture) != 0 && requested(info.is_capture) != info.channels;
    };
    const std::string reconfigureError = "Device is already active with a different channel count; deactivate it before changing channels";
    std::vector<AudioDeviceInfo> candidates;
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_context_initialized) return fail(error, "Audio context is not initialized");
        generation = m_activation_generation;
        bool alreadyActive = false, hasCapture = false, hasPlayback = false;
        for (const auto& entry : m_active_devices) {
            const auto& device = entry.second;
            if (device->get_original_id() != id) continue;
            alreadyActive = true;
            const auto& info = device->get_info();
            if (activeMismatch(info)) return fail(error, reconfigureError);
            (info.is_capture ? hasCapture : hasPlayback) = true;
        }
        for (const auto& baseInfo : m_available_devices) {
            if (endpoint_id(baseInfo, m_context.backend) != id) continue;
            (baseInfo.is_capture ? hasCapture : hasPlayback) = true;
            const std::string key = id + (baseInfo.is_capture ? "_capture" : "_playback");
            if (m_active_devices.count(key) != 0) continue;
            auto info = baseInfo;
            info.requested_channels = requested(info.is_capture);
            info.channels = info.requested_channels;
            candidates.push_back(std::move(info));
        }
        if ((captureChannels != 0 && !hasCapture) || (playbackChannels != 0 && !hasPlayback))
            return fail(error, "Requested device direction is unavailable: " + id);
        if (candidates.empty()) {
            if (alreadyActive) return true;
            return fail(error, "Device not found: " + id);
        }
    }

    std::vector<std::shared_ptr<HardwareDevice>> ready;
    std::string activationError;
    for (const auto& info : candidates) {
        auto device = std::make_shared<HardwareDevice>(info, &m_context, id);
        if (!device->init()) {
            if (!activationError.empty()) activationError += "; ";
            activationError += std::string(info.is_capture ? "Capture" : "Playback") +
                " activation failed for " + id + ": " + device->last_error();
            if (captureChannels != 0 || playbackChannels != 0) return fail(error, activationError);
            continue;
        }
        ready.push_back(std::move(device));
    }

    bool anyActive = false;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (generation != m_activation_generation)
            return fail(error, "Activation cancelled by device removal or stop; after deactivation");
        for (const auto& entry : m_active_devices) {
            if (entry.second->get_original_id() == id && activeMismatch(entry.second->get_info()))
                return fail(error, reconfigureError);
        }
        for (const auto& device : ready) {
            const std::string key = id + (device->get_info().is_capture ? "_capture" : "_playback");
            m_active_devices.emplace(key, device);
        }
        for (const auto& entry : m_active_devices) {
            if (entry.second->get_original_id() == id) anyActive = true;
        }
    }
    if (!anyActive) return fail(error, activationError.empty() ? "Device could not be activated: " + id : activationError);
    if (error && !activationError.empty()) *error = "Only available directions activated: " + activationError;
    return true;
}

void DeviceManager::deactivate_device(const std::string& id) {
    std::vector<std::shared_ptr<HardwareDevice>> retired;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        retired.reserve(m_active_devices.size());
        ++m_activation_generation;
        for (auto it = m_active_devices.begin(); it != m_active_devices.end();) {
            if (it->second->get_original_id() == id) {
                retired.push_back(std::move(it->second));
                it = m_active_devices.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void DeviceManager::stop_all() {
    std::unordered_map<std::string, std::shared_ptr<HardwareDevice>> retired;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        ++m_activation_generation;
        retired.swap(m_active_devices);
    }
}
