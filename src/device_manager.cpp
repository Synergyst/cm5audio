#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <set>
#include <vector>
#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>
#include <spa/utils/defs.h>

namespace {
// File-static pointer to pw_core accessible by registry callbacks
static struct pw_core* g_pw_core = nullptr;

template <size_t N>
std::string text_id(const char (&value)[N]) {
    return std::string(value, std::find(value, value + N, '\0'));
}

// Never interpret the device-ID union as ALSA on a different backend. Keep raw
// ALSA/Pulse names compatible with saved configurations from earlier releases.
std::string stable_device_id(ma_backend backend, const ma_device_id& id) {
    switch (backend) {
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

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
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

// PipeWire Registry Callbacks
void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    auto* manager = static_cast<DeviceManager*>(data);
    std::lock_guard<std::mutex> lock(manager->m_pw_graph_lock);

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* name = props ? spa_dict_lookup(props, "node.name") : nullptr;
        const char* desc = props ? spa_dict_lookup(props, "node.description") : nullptr;
        const char* dir = props ? spa_dict_lookup(props, "node.direction") : nullptr;
        bool is_input = (dir && strcmp(dir, "input") == 0);

        manager->m_pw_graph.nodes.push_back({
            id,
            name ? name : "Unknown Node",
            desc ? desc : "",
            is_input
        });
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        uint32_t src_node = 0, dst_node = 0, src_port = 0, dst_port = 0;
        if (props) {
            const char* sn = spa_dict_lookup(props, "link.output.node");
            const char* dn = spa_dict_lookup(props, "link.input.node");
            const char* sp = spa_dict_lookup(props, "link.output.port");
            const char* dp = spa_dict_lookup(props, "link.input.port");
            if (sn) src_node = std::stoul(sn);
            if (dn) dst_node = std::stoul(dn);
            if (sp) src_port = std::stoul(sp);
            if (dp) dst_port = std::stoul(dp);
        }
        manager->m_pw_graph.links.push_back({src_port, dst_port, src_node, dst_node});
    }
}

void registry_global_remove(void *data, uint32_t id) {
    auto* manager = static_cast<DeviceManager*>(data);
    std::lock_guard<std::mutex> lock(manager->m_pw_graph_lock);

    auto& nodes = manager->m_pw_graph.nodes;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [id](const auto& n) { return n.id == id; }), nodes.end());
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
        m_pw_loop = pw_main_loop_new(NULL);
        if (m_pw_loop) {
            m_pw_context = pw_context_new(pw_main_loop_get_loop(m_pw_loop), NULL, 0);
            if (m_pw_context) {
                struct pw_core *core = pw_context_connect(m_pw_context, NULL, 0);
                g_pw_core = core;
                if (core) {
                    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
                    if (registry) {
                        static struct spa_hook registry_listener;
                        spa_zero(registry_listener);
                        pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
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
    g_pw_core = nullptr;
    if (m_pw_enabled) {
        if (m_pw_loop) {
            pw_main_loop_quit(m_pw_loop);
            if (m_pw_thread.joinable()) m_pw_thread.join();
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
