#include "web_server.h"
#include "dsp_engine.h"
#include "device_manager.h"
#include "processor_registry.h"
#include "reliable_websocket_server.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <nlohmann/json.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "reliable_http_server.h"
#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <cstring>

namespace {

// Internal cache to track DSP chain state for persistence since RouteDsp is owned by DspEngine
struct RouteDspState {
    bool active = false;
    float gain = 1.0f;
    // Keep invalid persisted chains verbatim so saving does not silently discard them.
    nlohmann::json chain = nlohmann::json::array();
    std::string warning;
};
std::unordered_map<uint32_t, RouteDspState> g_dspSettingsCache;
std::mutex g_dspCacheLock;

nlohmann::json plugin_node_json(const PluginNodeInstance& plugin) {
    nlohmann::json ports = nlohmann::json::array();
    for (const auto& port : plugin.ports) {
        nlohmann::json item = {
            {"id", port.id}, {"name", port.name}, {"type", port.type},
            {"direction", port.direction}, {"endpoint", port.endpoint}
        };
        if (port.type == "audio") item["channels"] = port.channels;
        ports.push_back(std::move(item));
    }
    nlohmann::json result = {
        {"id", plugin.id}, {"name", plugin.name}, {"type", plugin.type},
        {"input_endpoint", plugin.input_endpoint}, {"output_endpoint", plugin.output_endpoint},
        {"input_channels", plugin.input_channels}, {"output_channels", plugin.output_channels},
        {"active", plugin.active}, {"params", plugin.params}, {"ports", std::move(ports)}
    };
    if (plugin.type == "plugin_chain") {
        result["chain"] = plugin.chain;
        result["chain_gain"] = plugin.chain_gain;
        result["channels"] = plugin.chain_channels;
        result["processor_count"] = plugin.chain.size();
    }
    return result;
}

std::string json_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\"' || ch == '\\') { result += '\\'; result += ch; }
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else result += ch;
    }
    return result;
}

unsigned query_uint(const httplib::Request& request, const char* name, unsigned fallback) {
    if (!request.has_param(name)) return fallback;
    try {
        const auto text = request.get_param_value(name);
        if (text.empty() || text[0] == '-') return fallback;
        size_t used = 0;
        const auto value = std::stoull(text, &used);
        if (used != text.size() || value > std::numeric_limits<unsigned>::max()) return fallback;
        return static_cast<unsigned>(value);
    } catch (...) { return fallback; }
}

float query_float(const httplib::Request& request, const char* name, float fallback) {
    if (!request.has_param(name)) return fallback;
    try { return std::stof(request.get_param_value(name)); }
    catch (...) { return fallback; }
}

bool query_bool(const httplib::Request& request, const char* name, bool fallback) {
    if (!request.has_param(name)) return fallback;
    const auto value = request.get_param_value(name);
    return value == "1" || value == "true" || value == "on";
}

bool parse_identity_message(const std::string& text, std::string& key, std::string& name,
                            unsigned& inputChannels, unsigned& outputChannels) {
    try {
        const auto object = nlohmann::json::parse(text);
        if (object.value("type", "") != "hello") return false;
        key = object.value("client_id", object.value("client_key", ""));
        name = object.value("client_name", object.value("name", ""));
        if (object.contains("input_channels")) inputChannels = object["input_channels"].get<unsigned>();
        if (object.contains("output_channels")) outputChannels = object["output_channels"].get<unsigned>();
        return !key.empty();
    } catch (...) { return false; }
}

std::string read_dashboard() {
    std::ifstream file("./web_client/dashboard.html", std::ios::in | std::ios::binary);
    if (!file) return "<!doctype html><html><body><h1>Dashboard unavailable</h1></body></html>";
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string read_favicon() {
    std::ifstream file("./web_client/favicon.ico", std::ios::in | std::ios::binary);
    if (!file) return ""; 
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

nlohmann::json stream_layout_json(const AudioDeviceInfo& info) {
    return {{"backend", info.backend}, {"backend_device_id", info.stable_id}, {"requested_channels", info.requested_channels},
            {"client_channels", info.channels}, {"client_sample_rate", info.sample_rate},
            {"client_format", "f32"}, {"client_channel_map", info.client_channel_map},
            {"internal_channels", info.internal_channels},
            {"internal_sample_rate", info.internal_sample_rate},
            {"internal_format", ma_get_format_name(info.internal_format)},
            {"internal_channel_map", info.internal_channel_map}};
}

nlohmann::json build_meter_json(const ChannelMeter* meters, unsigned channels) {
    nlohmann::json arr = nlohmann::json::array();
    if (!meters) return arr;
    for (unsigned i = 0; i < channels; ++i) {
        const auto& m = meters[i];
        nlohmann::json mj;
        mj["ch"] = i;
        mj["raw_value"] = m.raw_value.load(std::memory_order_relaxed);
        mj["raw_peak"] = m.raw_peak.load(std::memory_order_relaxed);
        mj["rms_db"] = m.rms_db.load(std::memory_order_relaxed);
        mj["peak_db"] = m.peak_db.load(std::memory_order_relaxed);
        mj["peak_hold_db"] = m.peak_hold_db.load(std::memory_order_relaxed);
        mj["clipped"] = m.clipped.load(std::memory_order_relaxed);
        arr.push_back(mj);
    }
    return arr;
}

template<class Container>
nlohmann::json build_meter_json_container(const Container& meters, unsigned channels) {
    return build_meter_json(meters.data(), std::min<unsigned>(channels, static_cast<unsigned>(meters.size())));
}

// Each chain stage owns either one processor instance for the entire interleaved
// bus, or one independent mono instance per channel. All instances and the
// scratch buffer are prepared on the control thread; process() never allocates.
struct PreparedPluginChainStage {
    ProcessorChainChannelMode channel_mode = ProcessorChainChannelMode::PER_CHANNEL;
    std::vector<std::unique_ptr<AudioProcessor>> processors;
};

class PluginChainProcessor final : public AudioProcessor {
public:
    PluginChainProcessor(std::vector<PreparedPluginChainStage> stages,
                         float gain, unsigned channels)
        : m_stages(std::move(stages)), m_gain(gain), m_channels(channels),
          m_channelScratch(CM5_MAX_AUDIO_FRAMES, 0.0f) {}

    void process(float* buffer, size_t frames, unsigned channels) override {
        if (!buffer || frames == 0 || channels == 0 ||
            channels != m_channels || frames > m_channelScratch.size())
            return;

        for (auto& stage : m_stages) {
            if (stage.processors.empty()) continue;

            if (stage.channel_mode == ProcessorChainChannelMode::PER_CHANNEL) {
                if (stage.processors.size() != channels) return;
                for (unsigned channel = 0; channel < channels; ++channel) {
                    auto& processor = stage.processors[channel];
                    if (!processor) continue;

                    for (size_t frame = 0; frame < frames; ++frame)
                        m_channelScratch[frame] = buffer[frame * channels + channel];

                    processor->process(m_channelScratch.data(), frames, 1);

                    for (size_t frame = 0; frame < frames; ++frame)
                        buffer[frame * channels + channel] = m_channelScratch[frame];
                }
            } else {
                // MULTICHANNEL processors handle the full interleaved bus.
                // MONO_ONLY processors reach this path only for channels == 1.
                if (stage.processors.front())
                    stage.processors.front()->process(buffer, frames, channels);
            }
        }

        if (m_gain != 1.0f) {
            const size_t count = frames * static_cast<size_t>(channels);
            for (size_t i = 0; i < count; ++i) buffer[i] *= m_gain;
        }
    }

private:
    std::vector<PreparedPluginChainStage> m_stages;
    float m_gain = 1.0f;
    unsigned m_channels = 1;
    std::vector<float> m_channelScratch;
};

void validate_plugin_chain_channel_compatibility(
    const nlohmann::json& rawChain, unsigned channels) {
    if (channels < 1 || channels > CM5_MAX_CHAIN_CHANNELS)
        throw std::invalid_argument("Plugin chain channels must be between 1 and " +
                                    std::to_string(CM5_MAX_CHAIN_CHANNELS));

    for (const auto& item : rawChain) {
        if (!item.is_object() || !item.contains("type") || !item["type"].is_string())
            continue; // The normalizer emits the more specific structure error.
        const std::string type = item["type"].get<std::string>();
        const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(type);
        if (!descriptor) continue; // The normalizer emits the unknown-type error.

        if (descriptor->chain_channel_mode == ProcessorChainChannelMode::MONO_ONLY && channels != 1)
            throw std::invalid_argument(type + " is mono-only and cannot be used in a " +
                                        std::to_string(channels) + "-channel plugin chain");

        const unsigned processorChannels =
            descriptor->chain_channel_mode == ProcessorChainChannelMode::PER_CHANNEL ? 1u : channels;
        if (processorChannels < descriptor->audio_input.minimum ||
            processorChannels > descriptor->audio_input.maximum ||
            processorChannels < descriptor->audio_output.minimum ||
            processorChannels > descriptor->audio_output.maximum) {
            throw std::invalid_argument(type + " does not support the required " +
                                        std::to_string(processorChannels) +
                                        "-channel processing mode in this chain");
        }
    }
}

nlohmann::json normalize_plugin_chain_definition(
    const nlohmann::json& rawChain, float chainGain, unsigned channels) {
    if (!rawChain.is_array()) throw std::invalid_argument("chain must be an array");
    if (rawChain.empty()) throw std::invalid_argument("a plugin chain must contain at least one processor");
    if (rawChain.size() > CM5_MAX_PROCESSORS_PER_ROUTE)
        throw std::invalid_argument("plugin chain exceeds maximum length of " +
                                    std::to_string(CM5_MAX_PROCESSORS_PER_ROUTE));
    if (!std::isfinite(chainGain) || chainGain < 0.0f || chainGain > 8.0f)
        throw std::invalid_argument("chain_gain must be finite and between 0 and 8");
    if (channels < 1 || channels > CM5_MAX_CHAIN_CHANNELS)
        throw std::invalid_argument("Plugin chain channels must be between 1 and " +
                                    std::to_string(CM5_MAX_CHAIN_CHANNELS));

    nlohmann::json normalizedChain = nlohmann::json::array();
    for (const auto& item : rawChain) {
        if (!item.is_object() || !item.contains("type") || !item["type"].is_string())
            throw std::invalid_argument("each chain item must contain a string type");
        const std::string type = item["type"].get<std::string>();
        const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(type);
        if (!descriptor) throw std::invalid_argument("Unknown processor type: " + type);
        const nlohmann::json params = item.value("params", nlohmann::json::object());
        if (!params.is_object()) throw std::invalid_argument("processor params must be an object: " + type);

        nlohmann::json normalized = nlohmann::json::object();
        for (const auto& parameter : descriptor->parameters) {
            const auto found = params.find(parameter.id);
            const nlohmann::json& value = found == params.end() ? parameter.default_value : *found;
            bool valid = false;
            if (parameter.type == "number" || parameter.type == "integer") {
                if (value.is_number()) {
                    const double number = value.get<double>();
                    valid = std::isfinite(number) &&
                        (!parameter.min || number >= *parameter.min) &&
                        (!parameter.max || number <= *parameter.max) &&
                        (parameter.type != "integer" || std::trunc(number) == number);
                }
            } else if (parameter.type == "boolean") {
                valid = value.is_boolean();
            } else if (parameter.type == "string") {
                valid = value.is_string();
            } else if (parameter.type == "enum") {
                valid = std::any_of(parameter.choices.begin(), parameter.choices.end(),
                    [&](const ProcessorChoice& choice) { return choice.value == value; });
            }
            if (!valid) throw std::invalid_argument("Invalid value for " + type + "/" + parameter.id);
            normalized[parameter.id] = value;
        }
        for (auto parameter = params.begin(); parameter != params.end(); ++parameter) {
            if (!normalized.contains(parameter.key()))
                throw std::invalid_argument("Unknown parameter for " + type + ": " + parameter.key());
        }
        normalizedChain.push_back({{"type", type}, {"params", std::move(normalized)}});
    }

    validate_plugin_chain_channel_compatibility(normalizedChain, channels);
    return normalizedChain;
}

struct PreparedPluginChain {
    nlohmann::json chain = nlohmann::json::array();
    std::vector<PreparedPluginChainStage> stages;
};

PreparedPluginChain prepare_plugin_chain(
    const nlohmann::json& rawChain, float chainGain, unsigned channels) {
    PreparedPluginChain prepared;
    prepared.chain = normalize_plugin_chain_definition(rawChain, chainGain, channels);
    prepared.stages.reserve(prepared.chain.size());

    for (const auto& item : prepared.chain) {
        const std::string type = item["type"].get<std::string>();
        const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(type);
        if (!descriptor) throw std::invalid_argument("Unknown processor type: " + type);

        PreparedPluginChainStage stage;
        stage.channel_mode = descriptor->chain_channel_mode;
        const unsigned instanceCount =
            stage.channel_mode == ProcessorChainChannelMode::PER_CHANNEL ? channels : 1u;
        stage.processors.reserve(instanceCount);
        for (unsigned instance = 0; instance < instanceCount; ++instance) {
            nlohmann::json normalized;
            auto processor = ProcessorRegistry::instance().create(
                type, item["params"], normalized);
            stage.processors.push_back(std::move(processor));
        }
        prepared.stages.push_back(std::move(stage));
    }
    return prepared;
}

nlohmann::json chain_preset_json(const PluginChainPreset& preset) {
    return {{"id", preset.id}, {"name", preset.name}, {"chain", preset.chain},
            {"chain_gain", preset.chain_gain}, {"active", preset.active},
            {"channels", preset.channels},
            {"processor_count", preset.chain.size()}};
}

struct PreparedRouteDsp {
    RouteDspState state;
    std::vector<std::unique_ptr<AudioProcessor>> chain;
};

// All parsing, validation, normalization and processor allocation happen on the
// control thread, before either the live route or its cached state is changed.
PreparedRouteDsp prepare_route_dsp(const nlohmann::json& body) {
    if (!body.is_object()) throw std::invalid_argument("DSP settings must be an object");
    PreparedRouteDsp prepared;
    if (body.contains("active")) {
        if (!body["active"].is_boolean()) throw std::invalid_argument("active must be a boolean");
        prepared.state.active = body["active"].get<bool>();
    }
    if (body.contains("gain")) {
        if (!body["gain"].is_number()) throw std::invalid_argument("gain must be a number");
        const double gain = body["gain"].get<double>();
        if (!std::isfinite(gain) || gain < 0.0 || gain > 8.0)
            throw std::invalid_argument("gain must be finite and between 0 and 8");
        prepared.state.gain = static_cast<float>(gain);
    }
    if (!body.contains("chain")) return prepared;
    const auto& chain = body["chain"];
    if (!chain.is_array()) throw std::invalid_argument("chain must be an array");
    if (chain.size() > CM5_MAX_PROCESSORS_PER_ROUTE)
        throw std::invalid_argument("DSP chain exceeds maximum length of " + std::to_string(CM5_MAX_PROCESSORS_PER_ROUTE));
    prepared.chain.reserve(chain.size());
    for (const auto& item : chain) {
        if (!item.is_object()) throw std::invalid_argument("Each processor must be an object");
        if (!item.contains("type") || !item["type"].is_string())
            throw std::invalid_argument("Each processor must have a string type");
        if (!item.contains("params") || !item["params"].is_object())
            throw std::invalid_argument("Each processor must have a params object");
        const auto type = item["type"].get<std::string>();
        nlohmann::json normalized;
        auto processor = ProcessorRegistry::instance().create(type, item["params"], normalized);
        prepared.chain.push_back(std::move(processor));
        prepared.state.chain.push_back({{"type", type}, {"params", std::move(normalized)}});
    }
    return prepared;
}

nlohmann::json route_dsp_json(const RouteDspState& state) {
    nlohmann::json result = {{"active", state.active}, {"gain", state.gain}, {"chain", state.chain}};
    if (!state.warning.empty()) result["warning"] = state.warning;
    return result;
}

// Do not narrow an unchecked ID: e.g. 4294967297 must never address route 1.
bool parse_route_dsp_id(const httplib::Request& request, uint32_t& routeId) {
    if (request.matches.size() <= 1) return false;
    try {
        const std::string text = request.matches[1];
        size_t consumed = 0;
        const auto value = std::stoull(text, &consumed);
        if (consumed != text.size() || value == 0 || value > std::numeric_limits<uint32_t>::max()) return false;
        routeId = static_cast<uint32_t>(value);
        return true;
    } catch (...) { return false; }
}

bool parse_plugin_instance_id(const httplib::Request& request, uint32_t& pluginId) {
    if (request.matches.size() <= 1) return false;
    try {
        const std::string text = request.matches[1];
        size_t consumed = 0;
        const auto value = std::stoull(text, &consumed);
        if (consumed != text.size() || value == 0 || value > std::numeric_limits<uint32_t>::max()) return false;
        pluginId = static_cast<uint32_t>(value);
        return true;
    } catch (...) { return false; }
}

bool parse_persisted_route_id(const nlohmann::json& value, uint32_t& routeId) {
    if (!value.is_number_integer()) return false;
    if (!value.is_number_unsigned() && value.get<int64_t>() <= 0) return false;
    const auto id = value.get<uint64_t>();
    if (id == 0 || id > std::numeric_limits<uint32_t>::max()) return false;
    routeId = static_cast<uint32_t>(id);
    return true;
}

bool route_exists(const ClientManager& manager, uint32_t routeId) {
    const auto routes = manager.route_snapshot();
    return std::any_of(routes->begin(), routes->end(),
        [routeId](const auto& route) { return route.id == routeId; });
}

void dsp_error(httplib::Response& response, int status, const std::string& message) {
    response.status = status;
    response.set_content(nlohmann::json({{"status", "error"}, {"message", message}}).dump(), "application/json");
}

} // namespace

WebClientSession::~WebClientSession() { stop_sender(); }

std::string WebClientSession::get_client_key() const {
    const auto key = std::atomic_load(&client_key);
    return key ? *key : std::string();
}

std::string WebClientSession::get_client_name() const {
    const auto name = std::atomic_load(&client_name);
    return name ? *name : std::string();
}

void WebClientSession::set_identity(const std::string& key, const std::string& name) {
    std::atomic_store(&client_key, std::make_shared<const std::string>(key));
    std::atomic_store(&client_name, std::make_shared<const std::string>(name));
}

void WebClientSession::start_sender(const std::shared_ptr<ix::WebSocket>& socket) {
    // Allocate before launching; allocation failure must not terminate a worker.
    std::vector<uint8_t> block(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CLIENT_CHANNELS * sizeof(float));
    sender_thread = std::thread([this, socket, block = std::move(block)]() mutable {
        try {
        while (active.load(std::memory_order_acquire)) {
            if (!media_ready.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const unsigned channels = std::clamp(output_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CLIENT_CHANNELS);
            const size_t bytes = CM5_MAX_AUDIO_FRAMES * channels * sizeof(float);
            const size_t got = outgoing_rb.read(block.data(), bytes);
            if (got) socket->sendBinary(std::string(reinterpret_cast<char*>(block.data()), got));
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        } catch (const std::exception& error) {
            active.store(false, std::memory_order_release);
            std::cerr << "[WS] Sender stopped: " << error.what() << std::endl;
        }
    });
}

void WebClientSession::stop_sender() {
    active.store(false, std::memory_order_release);
    // Called only by the external retirement owner, never by an IX callback.
    if (sender_thread.joinable()) sender_thread.join();
}

ClientManager::ClientManager()
    : m_routes(std::make_shared<const std::vector<AudioRoute>>()),
      m_pluginNodes(std::make_shared<const std::vector<std::shared_ptr<const PluginNodeInstance>>>()),
      m_midiRoutes(std::make_shared<const std::vector<MidiRoute>>()) {
}

ClientManager::~ClientManager() { stop_sessions(); }

void ClientManager::collect_retired_sessions() {
    std::vector<std::shared_ptr<WebClientSession>> retired;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        retired.swap(m_retired_sessions);
    }
    for (const auto& session : retired) session->stop_sender();
}

void ClientManager::stop_sessions() {
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (const auto& session : m_sessions) {
            session->active.store(false, std::memory_order_release);
            m_retired_sessions.push_back(session);
        }
        m_sessions.clear();
    }
    collect_retired_sessions();
}

NodeId ClientManager::get_node_id(const std::string& endpoint) const {
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (endpoint == "tone/generator") return NODE_TONE_GENERATOR;
    auto it = m_endpoint_to_id.find(endpoint);
    return (it != m_endpoint_to_id.end()) ? it->second : NODE_INVALID;
}

std::string ClientManager::get_endpoint_from_id(NodeId id) const {
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (id == NODE_TONE_GENERATOR) return "tone/generator";
    auto it = m_id_to_endpoint.find(id);
    return (it != m_id_to_endpoint.end()) ? it->second : "";
}

NodeId ClientManager::register_node(const std::string& endpoint) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (endpoint == "tone/generator") return NODE_TONE_GENERATOR;
    
    auto it = m_endpoint_to_id.find(endpoint);
    if (it != m_endpoint_to_id.end()) return it->second;
    
    // Persistent hardware/client endpoint IDs remain allocated while offline,
    // but deleted plugin endpoints are released and can be reused. Search the
    // bounded ID space instead of monotonically exhausting it after node edits.
    NodeId start = m_next_node_id.load(std::memory_order_relaxed);
    if (start < 1 || start >= 256) start = 1;
    NodeId id = NODE_INVALID;
    for (NodeId offset = 0; offset < 255; ++offset) {
        const NodeId candidate = 1 + ((start - 1 + offset) % 255);
        if (m_id_to_endpoint.find(candidate) == m_id_to_endpoint.end()) {
            id = candidate;
            NodeId next = candidate + 1;
            if (next >= 256) next = 1;
            m_next_node_id.store(next, std::memory_order_relaxed);
            break;
        }
    }
    if (id == NODE_INVALID) return NODE_INVALID;
    m_endpoint_to_id[endpoint] = id;
    m_id_to_endpoint[id] = endpoint;
    
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return id;
}

void ClientManager::unregister_node(const std::string&) {
    // Node identity survives disconnection. Binding is refreshed independently;
    // routes and saved endpoint strings must not be destroyed by going offline.
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
}

void ClientManager::update_activated_device(const std::string& id, bool active) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = std::find(m_activatedDevices.begin(), m_activatedDevices.end(), id);
    if (active && it == m_activatedDevices.end()) {
        m_activatedDevices.push_back(id);
        register_node("hardware/" + id + "/capture");
        register_node("hardware/" + id + "/playback");
    } else if (!active && it != m_activatedDevices.end()) {
        m_activatedDevices.erase(it);
        unregister_node("hardware/" + id + "/capture");
        unregister_node("hardware/" + id + "/playback");
    }
    // Successful activation can follow a failed restore of an already-listed ID.
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
}

void ClientManager::set_device_channels(const std::string& id, unsigned capture, unsigned playback) {
    if (capture > CM5_MAX_CHANNELS || playback > CM5_MAX_CHANNELS)
        throw std::invalid_argument("Device channels exceed application limit");
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    m_deviceChannels[id] = {capture, playback};
}

std::pair<unsigned, unsigned> ClientManager::device_channels(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    const auto found = m_deviceChannels.find(id);
    return found == m_deviceChannels.end() ? std::pair<unsigned, unsigned>{0, 0} : found->second;
}

std::shared_ptr<WebClientSession> ClientManager::create_session(uint32_t id, const std::string& remoteIp) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_lock);
    auto session = std::make_shared<WebClientSession>();
    session->id = id;
    session->remote_ip = remoteIp;
    session->set_identity("connection-" + std::to_string(id), "Client " + std::to_string(id));
    session->io_buffer.resize(CM5_BUFFER_FRAMES * CM5_MAX_CLIENT_CHANNELS);
    session->input_block.resize(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CLIENT_CHANNELS);
    session->output_block.resize(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CLIENT_CHANNELS);
    session->packet_block.resize(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CLIENT_CHANNELS);
    session->active.store(true, std::memory_order_relaxed);
    m_sessions.push_back(session);
    return session;
}

void ClientManager::remove_session(uint32_t id) {
    // IX can invoke Close while holding transport locks. Never wait for the
    // control transaction mutex here; snapshots retain identity independently.
    std::shared_ptr<WebClientSession> removed;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
            [id](const auto& session) { return session->id == id; });
        if (it == m_sessions.end()) return;
        removed = *it;
        removed->active.store(false, std::memory_order_release);
        m_retired_sessions.push_back(removed);
        std::string key = removed->get_client_key();
        unregister_node("client/" + key + "/capture");
        unregister_node("client/" + key + "/playback");
        m_sessions.erase(it);
    }
    // The reaper joins outside the callback and outside IX transport locks.
}

std::vector<std::shared_ptr<WebClientSession>> ClientManager::get_active_sessions() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<WebClientSession>> active;
    for (auto& session : m_sessions)
        if (session->active.load(std::memory_order_relaxed)) active.push_back(session);
    return active;
}

bool ClientManager::claim_identity(const std::shared_ptr<WebClientSession>& session,
                                   const std::string& key, const std::string& name) {
    if (key.empty()) return false;
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_lock);
    for (const auto& other : m_sessions) {
        if (other.get() != session.get() && other->active.load(std::memory_order_relaxed) &&
            other->get_client_key() == key) return false;
    }
    if (register_node("client/" + key + "/capture") == NODE_INVALID ||
        register_node("client/" + key + "/playback") == NODE_INVALID) return false;
    const std::string displayName = name.empty() ? key : name;
    session->set_identity(key, displayName);
    m_known_client_names[key] = displayName;
    register_node("client/" + key + "/capture");
    register_node("client/" + key + "/playback");
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::load_settings(AudioControls& controls, ToneControls& tone) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> controlsLock(controls.lock);
    const char* configuredPath = std::getenv("CM5AUDIO_SETTINGS");
    const std::string path = configuredPath && *configuredPath ? configuredPath : "./cm5audio_settings.json";
    std::ifstream file(path);
    if (!file) return false;
    try {
        const auto settings = nlohmann::json::parse(file);
        if (m_dspEngine && settings.contains("global_dsp") && settings["global_dsp"].is_object()) {
            const auto& dsp = settings["global_dsp"];
            const float gain = dsp.value("gain", 1.0f), threshold = dsp.value("threshold", 0.7f), ratio = dsp.value("ratio", 4.0f);
            if (std::isfinite(gain) && gain >= 0 && std::isfinite(threshold) && threshold >= 0 && std::isfinite(ratio) && ratio >= 1) {
                m_dspEngine->get_gain_processor()->set_gain(gain);
                m_dspEngine->get_compressor()->set_threshold(threshold);
                m_dspEngine->get_compressor()->set_ratio(ratio);
            }
        }
        if (settings.contains("tone") && settings["tone"].is_object()) {
            const auto& value = settings["tone"];
            tone.enabled.store(value.value("enabled", tone.enabled.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            tone.frequency_hz.store(std::clamp(value.value("frequency_hz", tone.frequency_hz.load(std::memory_order_relaxed)), 1.0f, 20000.0f), std::memory_order_relaxed);
            tone.amplitude.store(std::clamp(value.value("amplitude", tone.amplitude.load(std::memory_order_relaxed)), 0.0f, 1.0f), std::memory_order_relaxed);
        }
        if (settings.contains("hardware") && settings["hardware"].is_object()) {
            const auto& hardware = settings["hardware"];
            for (const char* type : {"capture", "playback"}) {
                if (!hardware.contains(type) || !hardware[type].is_array()) continue;
                const auto& array = hardware[type];
                for (const auto& item : array) {
                    if (!item.contains("endpoint")) continue;
                    std::string epId = item["endpoint"];
                    auto& bank = (std::string(type) == "capture") ? controls.capture[epId] : controls.playback[epId];
                    unsigned ch = item.value("channel", 0u);
                    if (ch < CM5_MAX_CONTROL_CHANNELS) {
                        bank[ch].gain.store(std::clamp(item.value("gain", 1.0f), 0.0f, 8.0f), std::memory_order_relaxed);
                        bank[ch].mute.store(item.value("mute", false), std::memory_order_relaxed);
                    }
                }
            }
        }
        m_deviceChannels.clear();
        if (settings.contains("device_streams") && settings["device_streams"].is_object()) {
            for (const auto& entry : settings["device_streams"].items()) {
                const auto capture = entry.value().value("capture_channels", 0u);
                const auto playback = entry.value().value("playback_channels", 0u);
                if (capture <= CM5_MAX_CHANNELS && playback <= CM5_MAX_CHANNELS)
                    m_deviceChannels[entry.key()] = {capture, playback};
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_activatedDevices.clear();
            if (settings.contains("activated_devices") && settings["activated_devices"].is_array()) {
                for (const auto& id_val : settings["activated_devices"]) {
                    if (id_val.is_string()) {
                        std::string id = id_val.get<std::string>();
                        m_activatedDevices.push_back(id);
                        if (m_deviceManager) {
                            const auto channels = device_channels(id);
                            std::string error;
                            if (!m_deviceManager->activate_device(id, channels.first, channels.second, &error))
                                std::cerr << "[Device] Restore failed for " << id << ": " << error << std::endl;
                        }
                        register_node("hardware/" + id + "/capture");
                        register_node("hardware/" + id + "/playback");
                    }
                }
            }
        }
        if (settings.contains("plugin_chain_presets") && settings["plugin_chain_presets"].is_array()) {
            for (const auto& value : settings["plugin_chain_presets"]) {
                try {
                    if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned() ||
                        !value.contains("name") || !value["name"].is_string() || !value.contains("chain"))
                        continue;
                    const uint64_t rawId = value["id"].get<uint64_t>();
                    if (rawId == 0 || rawId > std::numeric_limits<uint32_t>::max()) continue;
                    PluginChainPreset restored;
                    std::string error;
                    if (!save_plugin_chain_preset(value["name"].get<std::string>(), value["chain"],
                            value.value("chain_gain", 1.0f), value.value("active", true),
                            value.value("channels", 1u), static_cast<uint32_t>(rawId), restored, error))
                        std::cerr << "[Plugin Chain] Preset restore skipped for " << rawId << ": " << error << std::endl;
                } catch (const std::exception& error) {
                    std::cerr << "[Plugin Chain] Invalid saved preset: " << error.what() << std::endl;
                }
            }
        }
        if (settings.contains("plugin_nodes") && settings["plugin_nodes"].is_array()) {
            for (const auto& value : settings["plugin_nodes"]) {
                try {
                    if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned())
                        continue;
                    const uint64_t rawId = value["id"].get<uint64_t>();
                    if (rawId == 0 || rawId > std::numeric_limits<uint32_t>::max()) continue;
                    std::shared_ptr<const PluginNodeInstance> restored;
                    std::string error;
                    const std::string type = value.value("type", std::string());
                    const std::string name = value.value("name", std::string());
                    const unsigned inputChannels = value.value("input_channels", 1u);
                    const unsigned outputChannels = value.value("output_channels", 1u);
                    const nlohmann::json params = value.value("params", nlohmann::json::object());
                    const nlohmann::json portChannels = value.value("port_channels", nlohmann::json::object());
                    bool restoredOk = false;
                    if (type == "plugin_chain") {
                        const nlohmann::json chain = value.value("chain", nlohmann::json::array());
                        const unsigned chainChannels = value.value("channels", inputChannels);
                        restoredOk = create_plugin_chain_node(name, chain,
                            value.value("chain_gain", 1.0f), value.value("active", true),
                            chainChannels, static_cast<uint32_t>(rawId), restored, error);
                    } else if (value.contains("port_channels") && portChannels.is_object()) {
                        restoredOk = create_plugin_node_with_ports(type, name, portChannels, params,
                            static_cast<uint32_t>(rawId), restored, error);
                    } else {
                        restoredOk = create_plugin_node(type, name, inputChannels, outputChannels, params,
                            static_cast<uint32_t>(rawId), restored, error);
                    }
                    if (!restoredOk)
                        std::cerr << "[Plugin] Restore skipped for instance " << rawId << ": " << error << std::endl;
                    else if (type != "plugin_chain" && value.contains("active") && value["active"].is_boolean() && !value["active"].get<bool>()) {
                        std::shared_ptr<const PluginNodeInstance> updated;
                        const bool updateOk = value.contains("port_channels")
                            ? update_plugin_node_with_ports(static_cast<uint32_t>(rawId), name, portChannels, params, false, updated, error)
                            : update_plugin_node(static_cast<uint32_t>(rawId), name, inputChannels, outputChannels, params, false, updated, error);
                        if (!updateOk)
                            std::cerr << "[Plugin] Restore bypass state failed for instance " << rawId << ": " << error << std::endl;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "[Plugin] Invalid saved plugin node: " << error.what() << std::endl;
                }
            }
        }
        if (settings.contains("midi_routes") && settings["midi_routes"].is_array()) {
            for (const auto& value : settings["midi_routes"]) {
                try {
                    if (!value.is_object() || !value.contains("source") || !value["source"].is_string() ||
                        !value.contains("destination") || !value["destination"].is_string())
                        continue;
                    uint32_t ignored = 0;
                    std::string error;
                    if (!add_midi_route(value["source"].get<std::string>(),
                        value["destination"].get<std::string>(), ignored, error))
                        std::cerr << "[MIDI] Route restore skipped: " << error << std::endl;
                } catch (const std::exception& error) {
                    std::cerr << "[MIDI] Invalid saved MIDI route: " << error.what() << std::endl;
                }
            }
        }
        {
            std::lock_guard<std::mutex> routeLock(m_route_lock);
            std::lock_guard<std::mutex> sessionLock(m_lock);
            {
                std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                g_dspSettingsCache.clear();
            }
            std::vector<AudioRoute> routes;
            uint32_t nextId = 1;
            if (settings.contains("routes") && settings["routes"].is_array()) {
                for (const auto& value : settings["routes"]) {
                    AudioRoute route;
                    if (!value.is_object() || !value.contains("id") || !parse_persisted_route_id(value["id"], route.id)) continue;
                    std::string src_ep = value.value("source", "");
                    std::string dst_ep = value.value("destination", "");
                    route.source_node = src_ep.empty() ? NODE_INVALID : register_node(src_ep);
                    route.destination_node = dst_ep.empty() ? NODE_INVALID : register_node(dst_ep);
                    route.source_channel = value.value("source_channel", CM5_MAX_CHANNELS);
                    route.destination_channel = value.value("destination_channel", CM5_MAX_CHANNELS);
                    route.gain = value.value("gain", 1.0f);
                    route.enabled = value.value("enabled", true);
                    if (!route.id || src_ep.empty() || dst_ep.empty() ||
                        route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS ||
                        route.source_node == NODE_INVALID || route.destination_node == NODE_INVALID ||
                        !std::isfinite(route.gain) || route.gain < 0.0f || route.gain > 8.0f) continue;
                    routes.push_back(route);
                    const auto maxId = std::numeric_limits<uint32_t>::max();
                    nextId = std::max(nextId, route.id == maxId ? maxId : route.id + 1);

                    if (value.contains("dsp")) {
                        const auto& dsp = value["dsp"];
                        PreparedRouteDsp prepared;
                        RouteDspState state;
                        // Preserve the complete original chain on any validation failure,
                        // even if it is not an array. Invalid gains fall back to unity.
                        if (dsp.is_object()) {
                            if (dsp.contains("chain")) state.chain = dsp["chain"];
                            if (dsp.contains("gain") && dsp["gain"].is_number()) {
                                const double gain = dsp["gain"].get<double>();
                                if (std::isfinite(gain) && gain >= 0.0 && gain <= 8.0) state.gain = static_cast<float>(gain);
                            }
                        }
                        try {
                            if (m_dspEngine && !m_dspEngine->supports_route_dsp(route.id))
                                throw std::invalid_argument("Route ID exceeds DSP capacity");
                            prepared = prepare_route_dsp(dsp);
                            state = std::move(prepared.state);
                        } catch (const std::exception& error) {
                            prepared.chain.clear();
                            state.active = false;
                            state.warning = std::string("DSP bypassed: ") + error.what();
                            std::cerr << "[DSP] Route " << route.id << ": " << state.warning << std::endl;
                        }
                        // Cache even without an engine; normal startup installs the
                        // engine before restoring settings. Never index unsupported IDs.
                        std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                        if (m_dspEngine && m_dspEngine->supports_route_dsp(route.id)) {
                            m_dspEngine->update_route_dsp(route.id, std::move(prepared.chain), state.gain, state.active);
                        }
                        g_dspSettingsCache[route.id] = std::move(state);
                    }
                }
            }
            std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
            m_next_route_id.store(nextId, std::memory_order_relaxed);
            m_known_client_names.clear();
            if (settings.contains("clients") && settings["clients"].is_object()) {
                for (auto const& [key, name] : settings["clients"].items()) {
                    m_known_client_names[key] = name.get<std::string>();
                }
            }
        }
        if (m_dspEngine) m_dspEngine->mark_graph_dirty();
        return true;
    } catch (...) { return false; }
}

bool ClientManager::save_settings(const AudioControls& controls, const ToneControls& tone) const {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> controlsLock(controls.lock);
    const char* configuredPath = std::getenv("CM5AUDIO_SETTINGS");
    const std::string path = configuredPath && *configuredPath ? configuredPath : "./cm5audio_settings.json";
    std::string temporaryPath;
    nlohmann::json settings;
    settings["version"] = 1;
    settings["device_streams"] = nlohmann::json::object();
    for (const auto& entry : m_deviceChannels)
        settings["device_streams"][entry.first] = {{"capture_channels", entry.second.first},
                                                  {"playback_channels", entry.second.second}};
    if (m_dspEngine) settings["global_dsp"] = {
        {"gain", m_dspEngine->get_gain_processor()->get_gain()},
        {"threshold", m_dspEngine->get_compressor()->get_threshold()},
        {"ratio", m_dspEngine->get_compressor()->get_ratio()}};
    settings["tone"] = {
        {"enabled", tone.enabled.load(std::memory_order_relaxed)},
        {"frequency_hz", tone.frequency_hz.load(std::memory_order_relaxed)},
        {"amplitude", tone.amplitude.load(std::memory_order_relaxed)}
    };
    settings["plugin_nodes"] = nlohmann::json::array();
    for (const auto& plugin : get_plugin_nodes()) {
        nlohmann::json portChannels = nlohmann::json::object();
        for (const auto& port : plugin->ports)
            if (port.type == "audio") portChannels[port.id] = port.channels;
        nlohmann::json item = {
            {"id", plugin->id}, {"name", plugin->name}, {"type", plugin->type},
            {"input_channels", plugin->input_channels}, {"output_channels", plugin->output_channels},
            {"port_channels", std::move(portChannels)},
            {"active", plugin->active}, {"params", plugin->params}
        };
        if (plugin->type == "plugin_chain") {
            item["chain"] = plugin->chain;
            item["chain_gain"] = plugin->chain_gain;
            item["channels"] = plugin->chain_channels;
        }
        settings["plugin_nodes"].push_back(std::move(item));
    }
    settings["plugin_chain_presets"] = nlohmann::json::array();
    for (const auto& preset : get_plugin_chain_presets())
        settings["plugin_chain_presets"].push_back(chain_preset_json(preset));
    settings["midi_routes"] = nlohmann::json::array();
    for (const auto& route : get_midi_routes())
        settings["midi_routes"].push_back({{"id", route.id}, {"source", route.source_endpoint},
                                           {"destination", route.destination_endpoint}, {"enabled", route.enabled}});
    settings["hardware"]["capture"] = nlohmann::json::array();
    for (auto const& [id, bank] : controls.capture) {
        for (unsigned i = 0; i < CM5_MAX_CONTROL_CHANNELS; ++i) {
            settings["hardware"]["capture"].push_back({
                {"endpoint", id}, {"channel", i},
                {"gain", bank[i].gain.load(std::memory_order_relaxed)},
                {"mute", bank[i].mute.load(std::memory_order_relaxed)}
            });
        }
    }
    settings["hardware"]["playback"] = nlohmann::json::array();
    for (auto const& [id, bank] : controls.playback) {
        for (unsigned i = 0; i < CM5_MAX_CONTROL_CHANNELS; ++i) {
            settings["hardware"]["playback"].push_back({
                {"endpoint", id}, {"channel", i},
                {"gain", bank[i].gain.load(std::memory_order_relaxed)},
                {"mute", bank[i].mute.load(std::memory_order_relaxed)}
            });
        }
    }
    {
        auto* mutableThis = const_cast<ClientManager*>(this);
        std::lock_guard<std::mutex> lock(mutableThis->m_lock);
        settings["activated_devices"] = nlohmann::json::array();
        for (const auto& id : mutableThis->m_activatedDevices) {
            settings["activated_devices"].push_back(id);
        }
    }
    {
        std::lock_guard<std::mutex> routeLock(m_route_lock);
        std::lock_guard<std::mutex> sessionLock(m_lock);
        settings["routes"] = nlohmann::json::array();
        for (const auto& route : *std::atomic_load(&m_routes)) {
            nlohmann::json rj;
            rj["id"] = route.id;
            rj["source"] = get_endpoint_from_id(route.source_node);
            rj["source_channel"] = route.source_channel;
            rj["destination"] = get_endpoint_from_id(route.destination_node);
            rj["destination_channel"] = route.destination_channel;
            rj["gain"] = route.gain;
            rj["enabled"] = route.enabled;

            {
                std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                if (g_dspSettingsCache.count(route.id)) {
                    const auto& dsp = g_dspSettingsCache[route.id];
                    rj["dsp"] = {
                        {"active", dsp.active},
                        {"gain", dsp.gain},
                        {"chain", dsp.chain}
                    };
                }
            }
            settings["routes"].push_back(rj);
        }
        settings["clients"] = nlohmann::json::object();
        for (auto const& [key, name] : m_known_client_names) settings["clients"][key] = name;
        for (auto const& session : m_sessions) {
            const std::string key = session->get_client_key();
            if (!key.empty()) settings["clients"][key] = session->get_client_name();
        }
    }
    // Unique files also protect against a separate process using this path.
    int fd = -1;
    try {
        const std::string bytes = settings.dump(2) + "\n";
        std::string pattern = path + ".tmp.XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        fd = ::mkstemp(name.data());
        if (fd < 0) return false;
        temporaryPath = name.data();
        size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("settings write failed");
            offset += static_cast<size_t>(written);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("settings flush failed");
        const int closed = ::close(fd);
        fd = -1;
        if (closed != 0) throw std::runtime_error("settings close failed");
        if (::rename(temporaryPath.c_str(), path.c_str()) != 0)
            throw std::runtime_error("settings rename failed");
        return true;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        if (!temporaryPath.empty()) ::unlink(temporaryPath.c_str());
        return false;
    }
}

std::unordered_map<std::string, std::string> ClientManager::known_client_names() const {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_known_client_names;
}

std::shared_ptr<const std::vector<AudioRoute>> ClientManager::route_snapshot() const {
    return std::atomic_load(&m_routes);
}

std::vector<AudioRoute> ClientManager::get_routes() const {
    const auto routes = route_snapshot();
    return *routes;
}

std::shared_ptr<const std::vector<std::shared_ptr<const PluginNodeInstance>>>
ClientManager::plugin_node_snapshot() const {
    return std::atomic_load(&m_pluginNodes);
}

std::vector<std::shared_ptr<const PluginNodeInstance>> ClientManager::get_plugin_nodes() const {
    const auto snapshot = plugin_node_snapshot();
    return snapshot ? *snapshot : std::vector<std::shared_ptr<const PluginNodeInstance>>{};
}

bool ClientManager::create_plugin_chain_node(
    const std::string& name, const nlohmann::json& chain, float chainGain, bool active,
    unsigned channels, uint32_t requestedId,
    std::shared_ptr<const PluginNodeInstance>& created, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::vector<std::string> allocatedEndpoints;
    try {
        auto prepared = prepare_plugin_chain(chain, chainGain, channels);
        const auto existing = plugin_node_snapshot();
        uint32_t id = requestedId;
        if (id == 0) {
            id = m_next_plugin_id.fetch_add(1, std::memory_order_relaxed);
            if (id == 0 || id == std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("Plugin instance ID space exhausted");
        } else if (existing && std::any_of(existing->begin(), existing->end(),
                       [id](const auto& item) { return item && item->id == id; })) {
            throw std::invalid_argument("Plugin instance ID already exists");
        }

        const std::string base = "plugin/" + std::to_string(id) + "/";
        auto node = std::make_shared<PluginNodeInstance>();
        node->id = id;
        node->name = name.empty() ? "Plugin Chain" : name;
        node->type = "plugin_chain";
        node->active = active;
        node->chain = std::move(prepared.chain);
        node->chain_gain = chainGain;
        node->chain_channels = channels;
        node->input_channels = channels;
        node->output_channels = channels;
        node->params = nlohmann::json::object();

        node->process_node = register_node(base + "process");
        if (node->process_node == NODE_INVALID)
            throw std::runtime_error("Patchbay node capacity exhausted while allocating plugin chain process node");
        allocatedEndpoints.push_back(base + "process");

        PluginPortInstance input;
        input.id = "audio_in";
        input.name = "Audio Input";
        input.type = "audio";
        input.direction = "input";
        input.endpoint = base + "input";
        input.channels = channels;
        input.meter_bank = std::make_shared<PluginPortMeterBank>(channels);
        input.node = register_node(input.endpoint);
        if (input.node == NODE_INVALID)
            throw std::runtime_error("Patchbay node capacity exhausted while allocating plugin chain input");
        allocatedEndpoints.push_back(input.endpoint);

        PluginPortInstance output;
        output.id = "audio_out";
        output.name = "Audio Output";
        output.type = "audio";
        output.direction = "output";
        output.endpoint = base + "output";
        output.channels = channels;
        output.meter_bank = std::make_shared<PluginPortMeterBank>(channels);
        output.node = register_node(output.endpoint);
        if (output.node == NODE_INVALID)
            throw std::runtime_error("Patchbay node capacity exhausted while allocating plugin chain output");
        allocatedEndpoints.push_back(output.endpoint);

        node->input_endpoint = input.endpoint;
        node->input_node = input.node;
        node->output_endpoint = output.endpoint;
        node->output_node = output.node;
        node->ports.push_back(std::move(input));
        node->ports.push_back(std::move(output));
        node->processor = std::make_shared<PluginChainProcessor>(
            std::move(prepared.stages), chainGain, channels);

        auto next = std::make_shared<std::vector<std::shared_ptr<const PluginNodeInstance>>>(
            existing ? *existing : std::vector<std::shared_ptr<const PluginNodeInstance>>{});
        next->push_back(node);
        {
            std::lock_guard<std::mutex> lock(m_plugin_lock);
            std::atomic_store(&m_pluginNodes,
                std::static_pointer_cast<const std::vector<std::shared_ptr<const PluginNodeInstance>>>(next));
        }
        if (id >= m_next_plugin_id.load(std::memory_order_relaxed) && id < std::numeric_limits<uint32_t>::max())
            m_next_plugin_id.store(id + 1, std::memory_order_relaxed);
        if (m_dspEngine) m_dspEngine->mark_graph_dirty();
        created = node;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        std::lock_guard<std::mutex> nodeLock(m_node_lock);
        for (const auto& endpoint : allocatedEndpoints) {
            const auto found = m_endpoint_to_id.find(endpoint);
            if (found == m_endpoint_to_id.end()) continue;
            m_id_to_endpoint.erase(found->second);
            m_endpoint_to_id.erase(found);
        }
        return false;
    }
}

bool ClientManager::update_plugin_chain_node(
    uint32_t id, const std::string& name, const nlohmann::json& chain,
    float chainGain, bool active, unsigned channels,
    std::shared_ptr<const PluginNodeInstance>& updated, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    try {
        const auto current = plugin_node_snapshot();
        const auto found = std::find_if(current->begin(), current->end(),
            [id](const auto& item) { return item && item->id == id; });
        if (found == current->end()) throw std::invalid_argument("Plugin chain instance not found");
        if ((*found)->type != "plugin_chain") throw std::invalid_argument("Plugin instance is not a chain node");

        auto prepared = prepare_plugin_chain(chain, chainGain, channels);
        const auto routeSnapshot = route_snapshot();
        auto node = std::make_shared<PluginNodeInstance>(**found);

        for (auto& port : node->ports) {
            if (port.type != "audio") continue;
            if (port.channels != channels) {
                if (routeSnapshot) {
                    for (const auto& route : *routeSnapshot) {
                        if (route.source_node == port.node && route.source_channel >= channels)
                            throw std::invalid_argument("Cannot reduce chain width: an output cable uses a channel being removed");
                        if (route.destination_node == port.node && route.destination_channel >= channels)
                            throw std::invalid_argument("Cannot reduce chain width: an input cable uses a channel being removed");
                    }
                }
                port.channels = channels;
                port.meter_bank = std::make_shared<PluginPortMeterBank>(channels);
            }
        }

        if (!name.empty()) node->name = name;
        node->chain = std::move(prepared.chain);
        node->chain_gain = chainGain;
        node->chain_channels = channels;
        node->input_channels = channels;
        node->output_channels = channels;
        node->active = active;
        node->processor = std::make_shared<PluginChainProcessor>(
            std::move(prepared.stages), chainGain, channels);

        auto next = std::make_shared<std::vector<std::shared_ptr<const PluginNodeInstance>>>(*current);
        (*next)[static_cast<size_t>(std::distance(current->begin(), found))] = node;
        {
            std::lock_guard<std::mutex> lock(m_plugin_lock);
            std::atomic_store(&m_pluginNodes,
                std::static_pointer_cast<const std::vector<std::shared_ptr<const PluginNodeInstance>>>(next));
        }
        if (m_dspEngine) m_dspEngine->mark_graph_dirty();
        updated = node;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::vector<PluginChainPreset> ClientManager::get_plugin_chain_presets() const {
    std::lock_guard<std::mutex> lock(m_chain_preset_lock);
    return m_pluginChainPresets;
}

bool ClientManager::get_plugin_chain_preset(uint32_t id, PluginChainPreset& preset) const {
    std::lock_guard<std::mutex> lock(m_chain_preset_lock);
    const auto found = std::find_if(m_pluginChainPresets.begin(), m_pluginChainPresets.end(),
        [id](const PluginChainPreset& item) { return item.id == id; });
    if (found == m_pluginChainPresets.end()) return false;
    preset = *found;
    return true;
}

bool ClientManager::save_plugin_chain_preset(
    const std::string& name, const nlohmann::json& chain, float chainGain,
    bool active, unsigned channels, uint32_t requestedId,
    PluginChainPreset& saved, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    try {
        std::string cleanName = name;
        const auto first = cleanName.find_first_not_of(" \t\r\n");
        const auto last = cleanName.find_last_not_of(" \t\r\n");
        cleanName = first == std::string::npos ? std::string() : cleanName.substr(first, last - first + 1);
        if (cleanName.empty() || cleanName.size() > 80)
            throw std::invalid_argument("Preset name must contain 1 to 80 characters");
        auto normalizedChain = normalize_plugin_chain_definition(chain, chainGain, channels);
        std::lock_guard<std::mutex> lock(m_chain_preset_lock);
        uint32_t id = requestedId;
        if (id == 0) {
            id = m_next_chain_preset_id.fetch_add(1, std::memory_order_relaxed);
            if (id == 0 || id == std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("Plugin chain preset ID space exhausted");
        } else if (std::any_of(m_pluginChainPresets.begin(), m_pluginChainPresets.end(),
                       [id](const PluginChainPreset& item) { return item.id == id; })) {
            throw std::invalid_argument("Plugin chain preset ID already exists");
        }
        saved.id = id;
        saved.name = cleanName;
        saved.chain = std::move(normalizedChain);
        saved.chain_gain = chainGain;
        saved.active = active;
        saved.channels = channels;
        m_pluginChainPresets.push_back(saved);
        if (id >= m_next_chain_preset_id.load(std::memory_order_relaxed) && id < std::numeric_limits<uint32_t>::max())
            m_next_chain_preset_id.store(id + 1, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ClientManager::update_plugin_chain_preset(
    uint32_t id, const std::string& name, const nlohmann::json& chain,
    float chainGain, bool active, unsigned channels,
    PluginChainPreset& updated, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    try {
        if (id == 0) throw std::invalid_argument("Plugin chain preset ID must be nonzero");
        std::string cleanName = name;
        const auto first = cleanName.find_first_not_of(" \t\r\n");
        const auto last = cleanName.find_last_not_of(" \t\r\n");
        cleanName = first == std::string::npos ? std::string() : cleanName.substr(first, last - first + 1);
        if (cleanName.empty() || cleanName.size() > 80)
            throw std::invalid_argument("Preset name must contain 1 to 80 characters");

        // Validate and normalize every processor and parameter before replacing
        // the stored preset, so a bad edit never partially mutates the library.
        auto normalizedChain = normalize_plugin_chain_definition(chain, chainGain, channels);
        std::lock_guard<std::mutex> lock(m_chain_preset_lock);
        const auto found = std::find_if(m_pluginChainPresets.begin(), m_pluginChainPresets.end(),
            [id](const PluginChainPreset& item) { return item.id == id; });
        if (found == m_pluginChainPresets.end())
            throw std::invalid_argument("Plugin chain preset not found");

        updated.id = id;
        updated.name = cleanName;
        updated.chain = std::move(normalizedChain);
        updated.chain_gain = chainGain;
        updated.active = active;
        updated.channels = channels;
        *found = updated;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ClientManager::delete_plugin_chain_preset(uint32_t id, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_chain_preset_lock);
    const auto found = std::find_if(m_pluginChainPresets.begin(), m_pluginChainPresets.end(),
        [id](const PluginChainPreset& item) { return item.id == id; });
    if (found == m_pluginChainPresets.end()) { error = "Plugin chain preset not found"; return false; }
    m_pluginChainPresets.erase(found);
    return true;
}

bool ClientManager::create_plugin_node(
    const std::string& type, const std::string& name,
    unsigned inputChannels, unsigned outputChannels,
    const nlohmann::json& params, uint32_t requestedId,
    std::shared_ptr<const PluginNodeInstance>& created, std::string& error) {
    nlohmann::json channelMap = nlohmann::json::object();
    const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(type);
    if (!descriptor) { error = "Unknown plugin type: " + type; return false; }
    for (const auto& port : descriptor->ports) {
        if (port.type != ProcessorPortType::AUDIO) continue;
        if (port.direction == ProcessorPortDirection::INPUT && !channelMap.contains(port.id))
            channelMap[port.id] = inputChannels;
        else if (port.direction == ProcessorPortDirection::OUTPUT && !channelMap.contains(port.id))
            channelMap[port.id] = outputChannels;
    }
    return create_plugin_node_with_ports(type, name, channelMap, params,
                                         requestedId, created, error);
}

bool ClientManager::create_plugin_node_with_ports(
    const std::string& type, const std::string& name,
    const nlohmann::json& portChannels, const nlohmann::json& params,
    uint32_t requestedId,
    std::shared_ptr<const PluginNodeInstance>& created, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::vector<std::string> allocatedEndpoints;
    try {
        const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(type);
        if (!descriptor) throw std::invalid_argument("Unknown plugin type: " + type);
        if (!portChannels.is_object())
            throw std::invalid_argument("port_channels must be an object keyed by audio port ID");

        uint32_t id = requestedId;
        const auto existing = plugin_node_snapshot();
        if (id == 0) {
            id = m_next_plugin_id.fetch_add(1, std::memory_order_relaxed);
            if (id == 0 || id == std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("Plugin instance ID space exhausted");
        } else {
            if (std::any_of(existing->begin(), existing->end(),
                [id](const auto& node) { return node->id == id; }))
                throw std::invalid_argument("Plugin instance ID already exists");
        }

        nlohmann::json normalized;
        auto processor = ProcessorRegistry::instance().create(type, params, normalized);
        auto node = std::make_shared<PluginNodeInstance>();
        node->id = id;
        node->name = name.empty() ? descriptor->name : name;
        node->type = type;
        node->input_channels = 0;
        node->output_channels = 0;
        node->process_node = register_node("plugin/" + std::to_string(id) + "/process");
        if (node->process_node == NODE_INVALID)
            throw std::runtime_error("Patchbay node capacity exhausted while allocating plugin process node");
        allocatedEndpoints.push_back("plugin/" + std::to_string(id) + "/process");

        bool primaryInputSet = false;
        bool primaryOutputSet = false;
        for (const auto& declared : descriptor->ports) {
            PluginPortInstance port;
            port.id = declared.id;
            port.name = declared.name;
            port.type = processor_port_type_name(declared.type);
            port.direction = processor_port_direction_name(declared.direction);
            if (declared.type == ProcessorPortType::AUDIO) {
                unsigned channels = declared.channels.default_channels;
                const auto selected = portChannels.find(declared.id);
                if (selected != portChannels.end()) {
                    if (!selected->is_number_unsigned() && !selected->is_number_integer())
                        throw std::invalid_argument("Channel count for port '" + declared.id + "' must be an integer");
                    const int64_t raw = selected->get<int64_t>();
                    if (raw < 0 || raw > static_cast<int64_t>(CM5_MAX_CHANNELS))
                        throw std::invalid_argument("Channel count for port '" + declared.id + "' is outside the supported range");
                    channels = static_cast<unsigned>(raw);
                }
                if (channels < declared.channels.minimum || channels > declared.channels.maximum)
                    throw std::invalid_argument("Channel count for port '" + declared.id + "' is outside the plugin's declared range");
                port.channels = channels;
                port.meter_bank = std::make_shared<PluginPortMeterBank>(channels);
                const std::string base = "plugin/" + std::to_string(id) + "/";
                if (declared.id == "audio_in" && declared.direction == ProcessorPortDirection::INPUT)
                    port.endpoint = base + "input"; // Preserve existing saved endpoint IDs.
                else if (declared.id == "audio_out" && declared.direction == ProcessorPortDirection::OUTPUT)
                    port.endpoint = base + "output";
                else
                    port.endpoint = base + declared.id;
                port.node = register_node(port.endpoint);
                if (port.node == NODE_INVALID)
                    throw std::runtime_error("Patchbay node capacity exhausted while allocating plugin port '" + declared.id + "'");
                allocatedEndpoints.push_back(port.endpoint);
                if (declared.direction == ProcessorPortDirection::INPUT && !primaryInputSet) {
                    node->input_endpoint = port.endpoint;
                    node->input_node = port.node;
                    node->input_channels = channels;
                    primaryInputSet = true;
                } else if (declared.direction == ProcessorPortDirection::OUTPUT && !primaryOutputSet) {
                    node->output_endpoint = port.endpoint;
                    node->output_node = port.node;
                    node->output_channels = channels;
                    primaryOutputSet = true;
                }
            } else {
                port.endpoint = "plugin/" + std::to_string(id) + "/" + declared.id;
                if (declared.direction == ProcessorPortDirection::INPUT)
                    port.midi_queue = std::make_shared<MidiEventQueue>();
            }
            node->ports.emplace_back(std::move(port));
        }

        // Reject misspelled/unknown port settings rather than silently ignoring them.
        for (auto it = portChannels.begin(); it != portChannels.end(); ++it) {
            const auto declared = std::find_if(descriptor->ports.begin(), descriptor->ports.end(),
                [&](const ProcessorPortDescriptor& port) {
                    return port.id == it.key() && port.type == ProcessorPortType::AUDIO;
                });
            if (declared == descriptor->ports.end())
                throw std::invalid_argument("Unknown audio port ID in port_channels: " + it.key());
        }
        if (!primaryInputSet) { node->input_endpoint.clear(); node->input_node = NODE_INVALID; node->input_channels = 0; }
        if (!primaryOutputSet) { node->output_endpoint.clear(); node->output_node = NODE_INVALID; node->output_channels = 0; }

        node->active = true;
        node->params = std::move(normalized);
        node->processor = std::shared_ptr<AudioProcessor>(std::move(processor));
        for (const auto& port : node->ports)
            if (port.type == "midi" && port.direction == "input" && port.midi_queue)
                node->processor->attach_midi_input(port.id, port.midi_queue);

        auto next = std::make_shared<std::vector<std::shared_ptr<const PluginNodeInstance>>>(
            existing ? *existing : std::vector<std::shared_ptr<const PluginNodeInstance>>{});
        next->push_back(node);
        {
            std::lock_guard<std::mutex> lock(m_plugin_lock);
            std::atomic_store(&m_pluginNodes,
                std::static_pointer_cast<const std::vector<std::shared_ptr<const PluginNodeInstance>>>(next));
        }
        if (id >= m_next_plugin_id.load(std::memory_order_relaxed) && id < std::numeric_limits<uint32_t>::max())
            m_next_plugin_id.store(id + 1, std::memory_order_relaxed);
        if (m_dspEngine) m_dspEngine->mark_graph_dirty();
        created = node;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        std::lock_guard<std::mutex> nodeLock(m_node_lock);
        for (const auto& endpoint : allocatedEndpoints) {
            const auto found = m_endpoint_to_id.find(endpoint);
            if (found == m_endpoint_to_id.end()) continue;
            m_id_to_endpoint.erase(found->second);
            m_endpoint_to_id.erase(found);
        }
        return false;
    }
}

bool ClientManager::update_plugin_node(
    uint32_t id, const std::string& name,
    unsigned inputChannels, unsigned outputChannels,
    const nlohmann::json& params, bool active,
    std::shared_ptr<const PluginNodeInstance>& updated, std::string& error) {
    const auto current = plugin_node_snapshot();
    const auto found = std::find_if(current->begin(), current->end(),
        [id](const auto& node) { return node->id == id; });
    if (found == current->end()) { error = "Plugin instance not found"; return false; }
    nlohmann::json channels = nlohmann::json::object();
    bool inputSet = false, outputSet = false;
    for (const auto& port : (*found)->ports) {
        if (port.type != "audio") continue;
        if (port.direction == "input" && !inputSet) { channels[port.id] = inputChannels; inputSet = true; }
        else if (port.direction == "output" && !outputSet) { channels[port.id] = outputChannels; outputSet = true; }
        else channels[port.id] = port.channels;
    }
    return update_plugin_node_with_ports(id, name, channels, params, active, updated, error);
}

bool ClientManager::update_plugin_node_with_ports(
    uint32_t id, const std::string& name,
    const nlohmann::json& portChannels, const nlohmann::json& params, bool active,
    std::shared_ptr<const PluginNodeInstance>& updated, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    try {
        const auto current = plugin_node_snapshot();
        const auto found = std::find_if(current->begin(), current->end(),
            [id](const auto& node) { return node->id == id; });
        if (found == current->end()) throw std::invalid_argument("Plugin instance not found");
        const auto& old = *found;
        const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(old->type);
        if (!descriptor) throw std::invalid_argument("Plugin type is no longer registered: " + old->type);
        if (!portChannels.is_object()) throw std::invalid_argument("port_channels must be an object");

        std::vector<PluginPortInstance> ports = old->ports;
        // Keep each MIDI input queue stable across processor re-instantiation.
        // The PipeWire source service reference-counts queue attachments, so the
        // old processor can retire after graph adoption without detaching the
        // replacement processor's still-active input queue.
        for (auto& port : ports) {
            if (port.type != "audio") continue;
            const auto declared = std::find_if(descriptor->ports.begin(), descriptor->ports.end(),
                [&](const ProcessorPortDescriptor& item) { return item.id == port.id; });
            if (declared == descriptor->ports.end()) throw std::invalid_argument("Plugin port schema changed unexpectedly");
            const auto selected = portChannels.find(port.id);
            if (selected != portChannels.end()) {
                if (!selected->is_number_integer()) throw std::invalid_argument("Channel count for port '" + port.id + "' must be an integer");
                const int64_t raw = selected->get<int64_t>();
                if (raw < 0 || raw > static_cast<int64_t>(CM5_MAX_CHANNELS)) throw std::invalid_argument("Invalid channel count for port '" + port.id + "'");
                port.channels = static_cast<unsigned>(raw);
            }
            if (port.channels < declared->channels.minimum || port.channels > declared->channels.maximum)
                throw std::invalid_argument("Channel count for port '" + port.id + "' is outside the plugin's declared range");
            if (!port.meter_bank || port.meter_bank->channels != port.channels)
                port.meter_bank = std::make_shared<PluginPortMeterBank>(port.channels);
            for (const auto& route : *route_snapshot()) {
                if (route.destination_node == port.node && route.destination_channel >= port.channels)
                    throw std::invalid_argument("Cannot reduce port '" + port.id + "' while a connected cable uses a removed input channel");
                if (route.source_node == port.node && route.source_channel >= port.channels)
                    throw std::invalid_argument("Cannot reduce port '" + port.id + "' while a connected cable uses a removed output channel");
            }
        }
        for (auto it = portChannels.begin(); it != portChannels.end(); ++it) {
            const auto declared = std::find_if(descriptor->ports.begin(), descriptor->ports.end(),
                [&](const ProcessorPortDescriptor& port) { return port.id == it.key() && port.type == ProcessorPortType::AUDIO; });
            if (declared == descriptor->ports.end()) throw std::invalid_argument("Unknown audio port ID in port_channels: " + it.key());
        }

        nlohmann::json normalized;
        auto processor = ProcessorRegistry::instance().create(old->type, params, normalized);
        auto node = std::make_shared<PluginNodeInstance>();
        node->id = old->id;
        node->name = name.empty() ? old->name : name;
        node->type = old->type;
        node->process_node = old->process_node;
        node->input_channels = 0;
        node->output_channels = 0;
        node->ports = std::move(ports);
        for (const auto& port : node->ports) {
            if (port.type != "audio") continue;
            if (port.direction == "input" && node->input_node == NODE_INVALID) {
                node->input_node = port.node; node->input_endpoint = port.endpoint; node->input_channels = port.channels;
            } else if (port.direction == "output" && node->output_node == NODE_INVALID) {
                node->output_node = port.node; node->output_endpoint = port.endpoint; node->output_channels = port.channels;
            }
        }
        node->active = active;
        node->params = std::move(normalized);
        node->processor = std::shared_ptr<AudioProcessor>(std::move(processor));
        for (const auto& port : node->ports)
            if (port.type == "midi" && port.direction == "input" && port.midi_queue)
                node->processor->attach_midi_input(port.id, port.midi_queue);

        auto next = std::make_shared<std::vector<std::shared_ptr<const PluginNodeInstance>>>(*current);
        (*next)[static_cast<size_t>(std::distance(current->begin(), found))] = node;
        {
            std::lock_guard<std::mutex> lock(m_plugin_lock);
            std::atomic_store(&m_pluginNodes,
                std::static_pointer_cast<const std::vector<std::shared_ptr<const PluginNodeInstance>>>(next));
        }
        if (m_dspEngine) m_dspEngine->mark_graph_dirty();
        updated = node;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::shared_ptr<const std::vector<MidiRoute>> ClientManager::midi_route_snapshot() const {
    return std::atomic_load(&m_midiRoutes);
}

std::vector<MidiRoute> ClientManager::get_midi_routes() const {
    const auto snapshot = midi_route_snapshot();
    return snapshot ? *snapshot : std::vector<MidiRoute>{};
}

bool ClientManager::add_midi_route(const std::string& sourceEndpoint,
                                   const std::string& destinationEndpoint,
                                   uint32_t& assignedId, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_midi_route_lock);
    NodeId sourceProcess = NODE_INVALID;
    NodeId destinationProcess = NODE_INVALID;
    const auto plugins = get_plugin_nodes();
    for (const auto& plugin : plugins) {
        if (!plugin) continue;
        for (const auto& port : plugin->ports) {
            if (port.type != "midi") continue;
            if (port.endpoint == sourceEndpoint && port.direction == "output")
                sourceProcess = plugin->process_node;
            if (port.endpoint == destinationEndpoint && port.direction == "input")
                destinationProcess = plugin->process_node;
        }
    }
    if (sourceProcess == NODE_INVALID || destinationProcess == NODE_INVALID) {
        error = "MIDI route must connect a declared MIDI output to a declared MIDI input";
        return false;
    }
    if (sourceProcess == destinationProcess) {
        error = "A plugin cannot connect a MIDI output back to its own MIDI input";
        return false;
    }
    const auto current = midi_route_snapshot();
    std::unordered_map<std::string, NodeId> midiOutputOwners;
    std::unordered_map<std::string, NodeId> midiInputOwners;
    std::unordered_map<NodeId, NodeId> audioOutputOwners;
    std::unordered_map<NodeId, NodeId> audioInputOwners;
    for (const auto& plugin : plugins) {
        if (!plugin) continue;
        for (const auto& port : plugin->ports) {
            if (port.type == "midi") {
                if (port.direction == "output") midiOutputOwners[port.endpoint] = plugin->process_node;
                else midiInputOwners[port.endpoint] = plugin->process_node;
            } else if (port.type == "audio") {
                if (port.direction == "output" && port.node != NODE_INVALID)
                    audioOutputOwners[port.node] = plugin->process_node;
                else if (port.direction == "input" && port.node != NODE_INVALID)
                    audioInputOwners[port.node] = plugin->process_node;
            }
        }
    }
    std::unordered_map<NodeId, std::vector<NodeId>> midiAdjacency;
    for (const auto& route : *current) {
        if (route.source_endpoint == sourceEndpoint && route.destination_endpoint == destinationEndpoint) {
            error = "MIDI connection already exists";
            return false;
        }
        const auto source = midiOutputOwners.find(route.source_endpoint);
        const auto destination = midiInputOwners.find(route.destination_endpoint);
        if (source != midiOutputOwners.end() && destination != midiInputOwners.end())
            midiAdjacency[source->second].push_back(destination->second);
    }
    // Audio and MIDI connections both impose plugin execution dependencies.
    // Include existing audio plugin-to-plugin routes when checking a proposed
    // MIDI edge, otherwise a mixed audio/MIDI feedback loop could deadlock the
    // topological scheduler even though neither graph alone contains a cycle.
    const auto audioRoutes = route_snapshot();
    if (audioRoutes) {
        for (const auto& route : *audioRoutes) {
            const auto source = audioOutputOwners.find(route.source_node);
            const auto destination = audioInputOwners.find(route.destination_node);
            if (route.enabled && source != audioOutputOwners.end() &&
                destination != audioInputOwners.end())
                midiAdjacency[source->second].push_back(destination->second);
        }
    }
    // Reject cycles across the combined plugin processing graph so the DSP
    // scheduler never silently omits nodes from its same-block topological order.
    std::vector<NodeId> stack{destinationProcess};
    std::unordered_set<NodeId> visited;
    while (!stack.empty()) {
        const NodeId node = stack.back();
        stack.pop_back();
        if (node == sourceProcess) {
            error = "MIDI connection would create a processing cycle";
            return false;
        }
        if (!visited.insert(node).second) continue;
        const auto next = midiAdjacency.find(node);
        if (next != midiAdjacency.end())
            stack.insert(stack.end(), next->second.begin(), next->second.end());
    }

    const uint32_t id = m_next_midi_route_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0 || id == std::numeric_limits<uint32_t>::max()) {
        error = "MIDI route ID space exhausted";
        return false;
    }
    auto next = std::make_shared<std::vector<MidiRoute>>(*current);
    next->push_back({id, sourceEndpoint, destinationEndpoint, true});
    std::atomic_store(&m_midiRoutes,
        std::static_pointer_cast<const std::vector<MidiRoute>>(next));
    assignedId = id;
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::remove_midi_route(uint32_t id, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    std::lock_guard<std::mutex> lock(m_midi_route_lock);
    const auto current = midi_route_snapshot();
    auto next = std::make_shared<std::vector<MidiRoute>>(*current);
    const auto found = std::find_if(next->begin(), next->end(),
        [id](const MidiRoute& route) { return route.id == id; });
    if (found == next->end()) { error = "MIDI route not found"; return false; }
    next->erase(found);
    std::atomic_store(&m_midiRoutes,
        std::static_pointer_cast<const std::vector<MidiRoute>>(next));
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::remove_plugin_node(uint32_t id, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    const auto current = plugin_node_snapshot();
    const auto found = std::find_if(current->begin(), current->end(),
        [id](const auto& node) { return node->id == id; });
    if (found == current->end()) {
        error = "Plugin instance not found";
        return false;
    }
    const auto removed = *found;
    std::unordered_set<NodeId> removedNodes;
    std::unordered_set<std::string> removedEndpoints;
    if (removed->process_node != NODE_INVALID) removedNodes.insert(removed->process_node);
    for (const auto& port : removed->ports) {
        removedEndpoints.insert(port.endpoint);
        if (port.type == "audio" && port.node != NODE_INVALID)
            removedNodes.insert(port.node);
    }
    auto nextPlugins = std::make_shared<std::vector<std::shared_ptr<const PluginNodeInstance>>>(*current);
    nextPlugins->erase(nextPlugins->begin() + std::distance(current->begin(), found));
    {
        std::lock_guard<std::mutex> lock(m_plugin_lock);
        std::atomic_store(&m_pluginNodes,
            std::static_pointer_cast<const std::vector<std::shared_ptr<const PluginNodeInstance>>>(nextPlugins));
    }

    std::vector<uint32_t> removedRoutes;
    {
        std::lock_guard<std::mutex> routeLock(m_route_lock);
        auto routes = *std::atomic_load(&m_routes);
        routes.erase(std::remove_if(routes.begin(), routes.end(), [&](const AudioRoute& route) {
            const bool connected = removedNodes.count(route.source_node) || removedNodes.count(route.destination_node);
            if (connected) removedRoutes.push_back(route.id);
            return connected;
        }), routes.end());
        std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    }
    {
        std::lock_guard<std::mutex> midiLock(m_midi_route_lock);
        auto routes = *midi_route_snapshot();
        routes.erase(std::remove_if(routes.begin(), routes.end(), [&](const MidiRoute& route) {
            return removedEndpoints.count(route.source_endpoint) ||
                   removedEndpoints.count(route.destination_endpoint);
        }), routes.end());
        std::atomic_store(&m_midiRoutes, std::make_shared<const std::vector<MidiRoute>>(std::move(routes)));
    }
    if (m_dspEngine) {
        for (uint32_t routeId : removedRoutes) {
            if (m_dspEngine->supports_route_dsp(routeId))
                m_dspEngine->update_route_dsp(routeId, {}, 1.0f, false);
        }
    }
    {
        std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
        for (uint32_t routeId : removedRoutes) g_dspSettingsCache.erase(routeId);
    }
    {
        std::lock_guard<std::mutex> nodeLock(m_node_lock);
        for (const auto& endpoint : removedEndpoints) {
            const auto entry = m_endpoint_to_id.find(endpoint);
            if (entry == m_endpoint_to_id.end()) continue;
            m_id_to_endpoint.erase(entry->second);
            m_endpoint_to_id.erase(entry);
        }
        const std::string processEndpoint = "plugin/" + std::to_string(id) + "/process";
        const auto entry = m_endpoint_to_id.find(processEndpoint);
        if (entry != m_endpoint_to_id.end()) {
            m_id_to_endpoint.erase(entry->second);
            m_endpoint_to_id.erase(entry);
        }
    }
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::endpoint_exists(const std::string& endpoint, bool source) const {
    if (source && endpoint == "tone/generator") return true;
    if (endpoint.compare(0, 9, "hardware/") == 0) {
        const std::string suffix = source ? "/capture" : "/playback";
        if (endpoint.size() < 10 + suffix.size() || endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        return true; 
    }
    const std::string prefix = "client/";
    const std::string suffix = source ? "/capture" : "/playback";
    if (endpoint.size() <= prefix.size() + suffix.size() || endpoint.compare(0, prefix.size(), prefix) != 0 ||
        endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string key = endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - suffix.size());
    if (key.empty()) return false;
    for (auto& session : m_sessions)
        if (session->get_client_key() == key && session->active.load(std::memory_order_relaxed)) return true;
    return m_known_client_names.find(key) != m_known_client_names.end();
}

bool ClientManager::add_route(AudioRoute route, uint32_t& assignedId, std::string& error) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    if (route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS) {
        error = "channel index exceeds the application channel limit"; return false;
    }
    if (!std::isfinite(route.gain) || route.gain < 0.0f || route.gain > 8.0f) {
        error = "gain must be finite and between 0 and 8"; return false;
    }
    if (route.source_node < 0 || route.destination_node < 0 ||
        route.source_node >= 256 || route.destination_node >= 256) {
        error = "source or destination endpoint is invalid or unregistered"; return false;
    }
    // Validate known live widths without guessing a width for disconnected
    // endpoints. Saved/offline routes retain identity and are revalidated by DSP
    // when an endpoint reconnects with a different layout.
    const auto source = get_endpoint_from_id(route.source_node);
    const auto destination = get_endpoint_from_id(route.destination_node);
    if (source.empty() || destination.empty()) { error = "unregistered endpoint"; return false; }
    auto check = [&](const std::string& endpoint, unsigned channel, bool isSource) {
        if (endpoint == "tone/generator") return isSource && channel == 0;
        for (const auto& plugin : get_plugin_nodes()) {
            if (!plugin) continue;
            for (const auto& port : plugin->ports) {
                if (port.type != "audio" || endpoint != port.endpoint) continue;
                const bool portIsSource = port.direction == "output";
                return isSource == portIsSource && channel < port.channels;
            }
        }
        if (m_deviceManager) for (const auto& dev : m_deviceManager->get_active_devices()) {
            const auto id = "hardware/" + dev->get_original_id() +
                            (dev->get_info().is_capture ? "/capture" : "/playback");
            if (endpoint == id) return isSource == dev->get_info().is_capture && channel < dev->get_info().channels;
        }
        for (const auto& session : get_active_sessions()) {
            const auto base = "client/" + session->get_client_key();
            if (endpoint == base + "/capture") return isSource && channel < session->input_channels.load();
            if (endpoint == base + "/playback") return !isSource && channel < session->output_channels.load();
        }
        return true; // Offline or internal virtual endpoint, bounded above.
    };
    if (!check(source, route.source_channel, true) || !check(destination, route.destination_channel, false)) {
        error = "channel index or direction is invalid for the endpoint's active layout";
        return false;
    }

    // Audio and MIDI cables share one plugin execution graph. Prevent adding an
    // audio cable that closes a cycle through an existing MIDI connection.
    // The DSP worker uses same-block topological ordering, so such a cycle
    // would otherwise leave the involved processors unscheduled.
    const auto plugins = get_plugin_nodes();
    std::unordered_map<NodeId, NodeId> audioOutputOwners;
    std::unordered_map<NodeId, NodeId> audioInputOwners;
    for (const auto& plugin : plugins) {
        if (!plugin) continue;
        for (const auto& port : plugin->ports) {
            if (port.type != "audio" || port.node == NODE_INVALID) continue;
            if (port.direction == "output") audioOutputOwners[port.node] = plugin->process_node;
            else audioInputOwners[port.node] = plugin->process_node;
        }
    }
    const auto proposedSource = audioOutputOwners.find(route.source_node);
    const auto proposedDestination = audioInputOwners.find(route.destination_node);
    if (proposedSource != audioOutputOwners.end() &&
        proposedDestination != audioInputOwners.end()) {
        const NodeId sourceProcess = proposedSource->second;
        const NodeId destinationProcess = proposedDestination->second;
        if (sourceProcess == destinationProcess) {
            error = "A plugin cannot route an audio output back to its own audio input";
            return false;
        }

        std::unordered_map<NodeId, std::vector<NodeId>> adjacency;
        const auto existingAudioRoutes = route_snapshot();
        if (existingAudioRoutes) for (const auto& existing : *existingAudioRoutes) {
            if (!existing.enabled) continue;
            const auto from = audioOutputOwners.find(existing.source_node);
            const auto to = audioInputOwners.find(existing.destination_node);
            if (from != audioOutputOwners.end() && to != audioInputOwners.end())
                adjacency[from->second].push_back(to->second);
        }
        const auto existingMidiRoutes = midi_route_snapshot();
        std::unordered_map<std::string, NodeId> midiOutputOwners;
        std::unordered_map<std::string, NodeId> midiInputOwners;
        for (const auto& plugin : plugins) {
            if (!plugin) continue;
            for (const auto& port : plugin->ports) {
                if (port.type != "midi") continue;
                if (port.direction == "output") midiOutputOwners[port.endpoint] = plugin->process_node;
                else midiInputOwners[port.endpoint] = plugin->process_node;
            }
        }
        if (existingMidiRoutes) for (const auto& existing : *existingMidiRoutes) {
            if (!existing.enabled) continue;
            const auto from = midiOutputOwners.find(existing.source_endpoint);
            const auto to = midiInputOwners.find(existing.destination_endpoint);
            if (from != midiOutputOwners.end() && to != midiInputOwners.end())
                adjacency[from->second].push_back(to->second);
        }

        std::vector<NodeId> stack{destinationProcess};
        std::unordered_set<NodeId> visited;
        while (!stack.empty()) {
            const NodeId current = stack.back();
            stack.pop_back();
            if (current == sourceProcess) {
                error = "Audio connection would create a combined audio/MIDI processing cycle";
                return false;
            }
            if (!visited.insert(current).second) continue;
            const auto next = adjacency.find(current);
            if (next != adjacency.end())
                stack.insert(stack.end(), next->second.begin(), next->second.end());
        }
    }

    std::lock_guard<std::mutex> routeLock(m_route_lock);
    std::lock_guard<std::mutex> sessionLock(m_lock);
    auto routes = *std::atomic_load(&m_routes);
    for (const auto& existing : routes) {
        if (existing.source_node == route.source_node &&
            existing.source_channel == route.source_channel &&
            existing.destination_node == route.destination_node &&
            existing.destination_channel == route.destination_channel) {
            error = "This routing connection already exists";
            return false;
        }
    }
    if (m_next_route_id.load(std::memory_order_relaxed) == std::numeric_limits<uint32_t>::max()) {
        error = "route ID space exhausted"; return false;
    }
    route.id = assignedId = m_next_route_id.fetch_add(1);
    routes.push_back(std::move(route));
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::remove_route(uint32_t routeId) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    {
        std::lock_guard<std::mutex> lock(m_route_lock);
        auto routes = *std::atomic_load(&m_routes);
        const auto oldSize = routes.size();
        routes.erase(std::remove_if(routes.begin(), routes.end(), [routeId](const auto& route) { return route.id == routeId; }), routes.end());
        if (routes.size() == oldSize) return false;
        std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
        if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    }
    // Retire any processor chain associated with the now-deleted route. The
    // engine's three-slot publication keeps an in-flight audio reader safe.
    if (m_dspEngine && m_dspEngine->supports_route_dsp(routeId))
        m_dspEngine->update_route_dsp(routeId, {}, 1.0f, false);
    // Never acquire the route/session locks while holding the DSP cache lock.
    // POST rechecks the immutable route snapshot under this same cache lock.
    std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
    g_dspSettingsCache.erase(routeId);
    return true;
}

bool ClientManager::update_route(uint32_t routeId, float gain, bool enabled) {
    std::lock_guard<std::recursive_mutex> control(m_control_lock);
    if (!std::isfinite(gain) || gain < 0.0f || gain > 8.0f) return false;
    std::lock_guard<std::mutex> lock(m_route_lock);
    auto routes = *std::atomic_load(&m_routes);
    auto it = std::find_if(routes.begin(), routes.end(), [routeId](const auto& route) { return route.id == routeId; });
    if (it == routes.end()) return false;
    it->gain = gain;
    it->enabled = enabled;
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls, ToneControls& tone,
                     ClientManager& clientMgr, DeviceManager& deviceMgr, DspEngine& dspEngine, int httpPort, int wsPort, bool plainText, bool enablePwPatchbay)
    : m_metrics(metrics), m_controls(controls), m_tone(tone),
      m_clientMgr(clientMgr), m_deviceMgr(deviceMgr), m_dspEngine(dspEngine),
      m_httpPort(httpPort), m_wsPort(wsPort), m_plainText(plainText), m_pwEnabled(enablePwPatchbay) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleLock);
    if (m_running.load()) return true;
    const std::string certPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_CERT"); return value && *value ? std::string(value) : "./certs/server.crt"; }();
    const std::string keyPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_KEY"); return value && *value ? std::string(value) : "./certs/server.key"; }();
    const std::string p12Path = [] { const char* value = std::getenv("CM5AUDIO_TLS_P12"); return value && *value ? std::string(value) : "./certs/server.p12"; }();
    const bool tlsEnabled = !m_plainText && std::ifstream(certPath).good() && std::ifstream(keyPath).good();

    try {
        if (tlsEnabled) {
            auto ssl = std::make_unique<ReliableHttpServer<httplib::SSLServer>>(certPath.c_str(), keyPath.c_str());
            if (!ssl->is_valid()) throw std::runtime_error("Invalid TLS certificate/key");
            m_httpServer = std::move(ssl);
        } else {
            std::cerr << (m_plainText ? "[HTTP] Plain HTTP enabled" : "[HTTP] TLS certificate not found; serving plain HTTP") << std::endl;
            m_httpServer = std::make_unique<ReliableHttpServer<httplib::Server>>();
        }
        // Use address reuse for restart/TIME_WAIT, not SO_REUSEPORT (which can
        // silently share an occupied application port with another instance).
        m_httpServer->set_socket_options([](socket_t socket) {
            const int reuse = 1;
            ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        });
        m_httpServer->set_read_timeout(1);
        m_httpServer->set_write_timeout(1);
        m_httpServer->set_keep_alive_timeout(1);
        m_httpServer->set_payload_max_length(1024 * 1024);
        // Serialize accepted mutations through adoption and (where requested) save.
        // GETs use their own snapshot/container locks and remain concurrent.
        const auto post = [this](const std::string& path, httplib::Server::Handler handler) {
            m_httpServer->Post(path, [this, path, handler](const httplib::Request& request, httplib::Response& response) {
                std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
                if (!m_running.load()) { dsp_error(response, 503, "server is stopping"); return; }
                handler(request, response);
                if (response.status >= 400 || path == "/api/settings/save") return;
                const uint64_t target = m_dspEngine.request_adoption();
                const bool adopted = m_dspEngine.wait_for_revision(target, std::chrono::milliseconds(1500));
                auto body = nlohmann::json::parse(response.body);
                body["accepted_revision"] = target;
                body["adopted_revision"] = m_dspEngine.adopted_revision();
                body["adopted"] = adopted;
                body["persisted"] = false;
                if (!adopted) {
                    response.status = 503;
                    body["status"] = "pending";
                    body["message"] = "Configuration accepted, but engine adoption was not confirmed; refresh before retrying";
                } else {
                    const bool persistMutation =
                        path == "/api/tone" ||
                        path == "/api/devices/activate" ||
                        path == "/api/devices/deactivate" ||
                        path.find("/api/plugin-chain-presets") == 0 ||
                        path == "/api/plugins/chains" ||
                        path == "/api/plugins/extract-chain" ||
                        (path.find("/api/plugins/") == 0 && path.find("/chain") != std::string::npos);
                    if (persistMutation) {
                        m_dspEngine.collect_retired_endpoints();
                        const bool saved = m_clientMgr.save_settings(m_controls, m_tone);
                        body["persisted"] = saved;
                        if (saved) m_persistedRevision = target;
                        else { response.status = 500; body["status"] = "error"; body["message"] = "Configuration applied, but settings persistence failed"; }
                    }
                }
                body["persisted_revision"] = m_persistedRevision;
                response.set_content(body.dump(), "application/json");
            });
        };
        m_httpServer->set_mount_point("/client", "./web_client");
        m_httpServer->Get("/server.crt", [certPath](const httplib::Request&, httplib::Response& response) {
            std::ifstream certificate(certPath, std::ios::in | std::ios::binary);
            if (!certificate) {
                response.status = 404;
                response.set_content("Certificate unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << certificate.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.crt");
            response.set_content(contents.str(), "application/x-signed-certificate");
        });
        m_httpServer->Get("/server.key", [keyPath](const httplib::Request&, httplib::Response& response) {
            std::ifstream privateKey(keyPath, std::ios::in | std::ios::binary);
            if (!privateKey) {
                response.status = 404;
                response.set_content("Private key unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << privateKey.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.key");
            response.set_content(contents.str(), "application/x-pem-private-key");
        });
        m_httpServer->Get("/server.p12", [p12Path](const httplib::Request&, httplib::Response& response) {
            std::ifstream bundle(p12Path, std::ios::in | std::ios::binary);
            if (!bundle) {
                response.status = 404;
                response.set_content("PKCS#12 bundle unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << bundle.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.p12");
            response.set_content(contents.str(), "application/x-pkcs12");
        });

        auto meters = [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            j["capture"] = nlohmann::json::object();
            j["playback"] = nlohmann::json::object();
            // Tone generator is an internal one-channel graph source rather
            // than a hardware capture endpoint. Publish it separately while
            // using the same meter JSON shape as all other endpoints.
            j["tone"] = build_meter_json(&m_tone.meter, 1);
            {
                std::lock_guard<std::mutex> metersLock(m_metrics.lock);
                for (auto const& [id, meters_array] : m_metrics.capture) {
                    j["capture"][id] = build_meter_json_container(meters_array, static_cast<unsigned>(meters_array.size()));
                }
                for (auto const& [id, meters_array] : m_metrics.playback) {
                    j["playback"][id] = build_meter_json_container(meters_array, static_cast<unsigned>(meters_array.size()));
                }
            }

            // Plugin meter values are atomic and owned by each immutable graph
            // snapshot. Do not take the hardware metrics mutex while reading them.
            j["plugins"] = nlohmann::json::object();
            for (const auto& plugin : m_clientMgr.get_plugin_nodes()) {
                if (!plugin) continue;
                nlohmann::json pluginMeters = nlohmann::json::object();
                for (const auto& port : plugin->ports) {
                    if (port.type != "audio" || !port.meter_bank) continue;
                    pluginMeters[port.id] = build_meter_json(
                        port.meter_bank->meters.get(), port.meter_bank->channels);
                }
                j["plugins"][std::to_string(plugin->id)] = std::move(pluginMeters);
            }
            response.set_content(j.dump(2), "application/json");
        };
        m_httpServer->Get("/api/devices", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            m_deviceMgr.enumerate_devices();
            const auto devices = m_deviceMgr.get_available_devices();
            const auto active = m_deviceMgr.get_active_devices();
            nlohmann::json result = nlohmann::json::array();
            for (const auto& device : devices) {
                const auto selected = m_clientMgr.device_channels(device.stable_id);
                nlohmann::json item = {{"id", device.stable_id}, {"name", device.name},
                    {"is_capture", device.is_capture}, {"backend", device.backend},
                    {"channels", 0}, {"sample_rate", 48000}, {"active", false},
                    {"capture_channels", selected.first}, {"playback_channels", selected.second},
                    {"max_channels", CM5_MAX_CHANNELS}};
                for (const auto& endpoint : active) {
                    if (endpoint->get_original_id() != device.stable_id ||
                        endpoint->get_info().is_capture != device.is_capture) continue;
                    item["active"] = true;
                    item["channels"] = endpoint->get_info().channels;
                    item["layout"] = stream_layout_json(endpoint->get_info());
                }
                result.push_back(std::move(item));
            }
            response.set_content(result.dump(2), "application/json");
        });

        // Explicit probing only: miniaudio warns that detailed info may open a
        // device. Never probe during enumeration or periodic dashboard refresh.
        m_httpServer->Get("/api/devices/capabilities", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            if (!request.has_param("id")) { dsp_error(response, 400, "id is required"); return; }
            try {
                std::string error;
                const auto formats = m_deviceMgr.get_device_capabilities(request.get_param_value("id"),
                    query_bool(request, "is_capture", false), &error);
                if (!error.empty()) { dsp_error(response, 400, error); return; }
                nlohmann::json tuples = nlohmann::json::array();
                for (const auto& format : formats)
                    tuples.push_back({{"format", ma_get_format_name(format.format)},
                        {"channels", format.channels}, {"sample_rate", format.sample_rate}, {"flags", format.flags}});
                response.set_content(nlohmann::json({{"id", request.get_param_value("id")},
                    {"is_capture", query_bool(request, "is_capture", false)}, {"native_formats", tuples},
                    {"note", "Reported tuples may contain wildcards and may be incomplete; opening validates the selection. OS profiles remain externally managed."}}).dump(2), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        post("/api/devices/activate", [this](const httplib::Request& request, httplib::Response& response) {
            const std::string id = request.has_param("id") ? request.get_param_value("id") : "";
            if (id.empty()) { dsp_error(response, 400, "id parameter is required"); return; }
            auto selected = m_clientMgr.device_channels(id);
            if (request.has_param("capture_channels")) selected.first = query_uint(request, "capture_channels", CM5_MAX_CHANNELS + 1);
            if (request.has_param("playback_channels")) selected.second = query_uint(request, "playback_channels", CM5_MAX_CHANNELS + 1);
            if (selected.first > CM5_MAX_CHANNELS || selected.second > CM5_MAX_CHANNELS) {
                dsp_error(response, 400, "channels must be 0 (automatic) or 1..254"); return;
            }
            for (const auto& endpoint : m_deviceMgr.get_active_devices()) {
                if (endpoint->get_original_id() != id) continue;
                const auto& info = endpoint->get_info();
                const auto field = info.is_capture ? "capture_channels" : "playback_channels";
                const auto requested = info.is_capture ? selected.first : selected.second;
                if (request.has_param(field) && requested != info.requested_channels) {
                    dsp_error(response, 400, "Deactivate the device before changing its channel selection (including automatic mode)");
                    return;
                }
            }
            if (m_clientMgr.register_node("hardware/" + id + "/capture") == NODE_INVALID ||
                m_clientMgr.register_node("hardware/" + id + "/playback") == NODE_INVALID) {
                dsp_error(response, 400, "Endpoint node capacity exhausted"); return;
            }
            std::string error;
            if (m_deviceMgr.activate_device(id, selected.first, selected.second, &error)) {
                m_clientMgr.set_device_channels(id, selected.first, selected.second);
                m_clientMgr.update_activated_device(id, true);
                nlohmann::json result = {{"status", "ok"}};
                nlohmann::json directions = nlohmann::json::array();
                for (const auto& endpoint : m_deviceMgr.get_active_devices()) {
                    if (endpoint->get_original_id() == id)
                        directions.push_back(endpoint->get_info().is_capture ? "capture" : "playback");
                }
                result["active_directions"] = directions;
                if (!error.empty()) result["warning"] = error;
                response.set_content(result.dump(), "application/json");
            } else { dsp_error(response, 400, error.empty() ? "failed to activate device" : error); }
        });

        post("/api/devices/deactivate", [this](const httplib::Request& request, httplib::Response& response) {
            const std::string id = request.has_param("id") ? request.get_param_value("id") : "";
            if (id.empty()) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"id parameter is required\"}", "application/json");
                return;
            }
            m_deviceMgr.deactivate_device(id);
            m_clientMgr.update_activated_device(id, false);
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        m_httpServer->Get("/api/meters", meters);
        m_httpServer->Get("/api/metrics", meters);
        m_httpServer->Get("/api/raw", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard<std::mutex> metersLock(m_metrics.lock);
            nlohmann::json j;
            j["capture"] = nlohmann::json::object();
            for (auto const& [id, meters_array] : m_metrics.capture) {
                nlohmann::json arr = nlohmann::json::array();
                for (unsigned i = 0; i < meters_array.size(); ++i) {
                    const auto& meter = meters_array[i];
                    arr.push_back({{"ch", i}, {"value", meter.raw_value.load(std::memory_order_relaxed)}, {"peak", meter.raw_peak.load(std::memory_order_relaxed)}});
                }
                j["capture"][id] = arr;
            }
            j["playback"] = nlohmann::json::object();
            for (auto const& [id, meters_array] : m_metrics.playback) {
                nlohmann::json arr = nlohmann::json::array();
                for (unsigned i = 0; i < meters_array.size(); ++i) {
                    const auto& meter = meters_array[i];
                    arr.push_back({{"ch", i}, {"value", meter.raw_value.load(std::memory_order_relaxed)}, {"peak", meter.raw_peak.load(std::memory_order_relaxed)}});
                }
                j["playback"][id] = arr;
            }
            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Get("/api/tone", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            j["enabled"] = m_tone.enabled.load();
            j["frequency_hz"] = m_tone.frequency_hz.load();
            j["amplitude"] = m_tone.amplitude.load();
            response.set_content(j.dump(2), "application/json");
        });
        post("/api/tone", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const bool enabled = query_bool(request, "enabled", m_tone.enabled.load());
                const float frequency = query_float(request, "frequency", m_tone.frequency_hz.load());
                const float amplitude = query_float(request, "amplitude", m_tone.amplitude.load());
                if (request.has_param("frequency")) {
                    if (!std::isfinite(frequency) || frequency < 1.0f || frequency > 20000.0f) throw std::invalid_argument("frequency must be 1..20000 Hz");
                }
                if (request.has_param("amplitude")) {
                    if (!std::isfinite(amplitude) || amplitude < 0.0f || amplitude > 1.0f) throw std::invalid_argument("amplitude must be 0..1");
                }
                m_tone.enabled.store(enabled);
                m_tone.frequency_hz.store(frequency);
                m_tone.amplitude.store(amplitude);
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Get("/api/dsp/available", [](const httplib::Request&, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_content(ProcessorRegistry::instance().discovery().dump(2), "application/json");
        });

        m_httpServer->Get("/api/plugins", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json nodes = nlohmann::json::array();
            for (const auto& plugin : m_clientMgr.get_plugin_nodes())
                nodes.push_back(plugin_node_json(*plugin));
            nlohmann::json midiRoutes = nlohmann::json::array();
            for (const auto& route : m_clientMgr.get_midi_routes())
                midiRoutes.push_back({{"id", route.id}, {"source", route.source_endpoint},
                                      {"destination", route.destination_endpoint}, {"enabled", route.enabled}});
            response.set_header("Cache-Control", "no-store");
            response.set_content(nlohmann::json({{"plugin_nodes", nodes}, {"midi_routes", midiRoutes}}).dump(2), "application/json");
        });

        m_httpServer->Get("/api/plugin-chain-presets", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json presets = nlohmann::json::array();
            for (const auto& preset : m_clientMgr.get_plugin_chain_presets())
                presets.push_back(chain_preset_json(preset));
            response.set_header("Cache-Control", "no-store");
            response.set_content(nlohmann::json({{"presets", presets}}).dump(2), "application/json");
        });

        post("/api/plugin-chain-presets", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("name") || !body["name"].is_string() ||
                    !body.contains("chain"))
                    throw std::invalid_argument("name and chain are required");
                PluginChainPreset saved;
                std::string error;
                if (!m_clientMgr.save_plugin_chain_preset(body["name"].get<std::string>(), body["chain"],
                        body.value("chain_gain", 1.0f), body.value("active", true),
                        body.value("channels", 1u), 0, saved, error))
                    throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"preset", chain_preset_json(saved)}}).dump(2), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        post(R"(/api/plugin-chain-presets/(\d+)/update)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t presetId = 0;
            try {
                if (!parse_plugin_instance_id(request, presetId))
                    throw std::invalid_argument("invalid plugin chain preset ID");
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object()) throw std::invalid_argument("Preset update must be an object");
                PluginChainPreset current;
                if (!m_clientMgr.get_plugin_chain_preset(presetId, current))
                    throw std::invalid_argument("Plugin chain preset not found");
                const std::string name = body.value("name", current.name);
                const nlohmann::json chain = body.value("chain", current.chain);
                const float chainGain = body.value("chain_gain", current.chain_gain);
                const bool active = body.value("active", current.active);
                const unsigned channels = body.value("channels", current.channels);
                PluginChainPreset updated;
                std::string error;
                if (!m_clientMgr.update_plugin_chain_preset(presetId, name, chain,
                        chainGain, active, channels, updated, error))
                    throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"},
                    {"preset", chain_preset_json(updated)}}).dump(2), "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        post(R"(/api/plugin-chain-presets/(\d+)/instantiate)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t presetId = 0;
            try {
                if (!parse_plugin_instance_id(request, presetId)) throw std::invalid_argument("invalid plugin chain preset ID");
                PluginChainPreset preset;
                if (!m_clientMgr.get_plugin_chain_preset(presetId, preset)) throw std::invalid_argument("Plugin chain preset not found");
                const auto body = request.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(request.body);
                const std::string nodeName = body.value("name", preset.name);
                std::shared_ptr<const PluginNodeInstance> created;
                std::string error;
                if (!m_clientMgr.create_plugin_chain_node(nodeName, preset.chain, preset.chain_gain,
                        preset.active, preset.channels, 0, created, error)) throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"plugin", plugin_node_json(*created)}}).dump(2), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        m_httpServer->Delete(R"(/api/plugin-chain-presets/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            uint32_t presetId = 0;
            if (!parse_plugin_instance_id(request, presetId)) { dsp_error(response, 400, "invalid plugin chain preset ID"); return; }
            std::string error;
            if (!m_clientMgr.delete_plugin_chain_preset(presetId, error)) { dsp_error(response, 404, error); return; }
            const bool persisted = m_clientMgr.save_settings(m_controls, m_tone);
            response.status = persisted ? 200 : 500;
            response.set_content(nlohmann::json({{"status", persisted ? "ok" : "error"},
                {"persisted", persisted}, {"message", persisted ? "Preset deleted" : "Preset deleted in memory, but settings could not be saved"}}).dump(2), "application/json");
        });

        post("/api/plugins/chains", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("chain")) throw std::invalid_argument("chain is required");
                std::shared_ptr<const PluginNodeInstance> created;
                std::string error;
                if (!m_clientMgr.create_plugin_chain_node(body.value("name", std::string("Plugin Chain")),
                        body["chain"], body.value("chain_gain", 1.0f), body.value("active", true),
                        body.value("channels", 1u), 0, created, error)) throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"plugin", plugin_node_json(*created)}}).dump(2), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        post(R"(/api/plugins/(\d+)/chain)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t id = 0;
            try {
                if (!parse_plugin_instance_id(request, id)) throw std::invalid_argument("invalid plugin chain instance ID");
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("chain")) throw std::invalid_argument("chain is required");
                const auto current = m_clientMgr.get_plugin_nodes();
                const auto found = std::find_if(current.begin(), current.end(),
                    [id](const auto& item) { return item && item->id == id; });
                if (found == current.end()) throw std::invalid_argument("Plugin chain instance not found");
                std::shared_ptr<const PluginNodeInstance> updated;
                std::string error;
                if (!m_clientMgr.update_plugin_chain_node(id, body.value("name", (*found)->name),
                        body["chain"], body.value("chain_gain", (*found)->chain_gain),
                        body.value("active", (*found)->active),
                        body.value("channels", (*found)->chain_channels), updated, error))
                    throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"plugin", plugin_node_json(*updated)}}).dump(2), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        post(R"(/api/plugins/(\d+)/replace-chain-preset)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t pluginId = 0;
            try {
                if (!parse_plugin_instance_id(request, pluginId))
                    throw std::invalid_argument("invalid plugin chain instance ID");
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("preset_id") || !body["preset_id"].is_number_integer())
                    throw std::invalid_argument("preset_id is required");
                const int64_t rawPresetId = body["preset_id"].get<int64_t>();
                if (rawPresetId <= 0 || static_cast<uint64_t>(rawPresetId) > std::numeric_limits<uint32_t>::max())
                    throw std::invalid_argument("invalid plugin chain preset ID");

                const auto nodes = m_clientMgr.get_plugin_nodes();
                const auto nodeIt = std::find_if(nodes.begin(), nodes.end(),
                    [pluginId](const auto& item) { return item && item->id == pluginId; });
                if (nodeIt == nodes.end()) throw std::invalid_argument("Plugin chain node not found");
                if ((*nodeIt)->type != "plugin_chain")
                    throw std::invalid_argument("Target plugin node is not a composite chain");

                PluginChainPreset preset;
                if (!m_clientMgr.get_plugin_chain_preset(static_cast<uint32_t>(rawPresetId), preset))
                    throw std::invalid_argument("Plugin chain preset not found");

                // update_plugin_chain_node retains the live node ID, port IDs,
                // meter banks, and existing graph cables.
                std::shared_ptr<const PluginNodeInstance> updated;
                std::string error;
                if (!m_clientMgr.update_plugin_chain_node(pluginId, (*nodeIt)->name,
                        preset.chain, preset.chain_gain, preset.active, preset.channels, updated, error))
                    throw std::invalid_argument(error);

                response.set_content(nlohmann::json({{"status", "ok"},
                    {"plugin", plugin_node_json(*updated)},
                    {"replaced_from_preset_id", preset.id},
                    {"replaced_from_preset_name", preset.name}}).dump(2), "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        post("/api/plugins", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("type") || !body["type"].is_string())
                    throw std::invalid_argument("type is required and must be a string");
                const std::string type = body["type"].get<std::string>();
                const auto* descriptor = ProcessorRegistry::instance().find(type);
                if (!descriptor) throw std::invalid_argument("Unknown plugin type: " + type);
                const std::string name = body.value("name", descriptor->name);
                const nlohmann::json params = body.value("params", nlohmann::json::object());
                std::shared_ptr<const PluginNodeInstance> created;
                std::string error;
                bool ok = false;
                if (body.contains("port_channels")) {
                    ok = m_clientMgr.create_plugin_node_with_ports(type, name, body["port_channels"],
                        params, 0, created, error);
                } else {
                    const unsigned inputChannels = body.value("input_channels", descriptor->audio_input.default_channels);
                    const unsigned outputChannels = body.value("output_channels", descriptor->audio_output.default_channels);
                    ok = m_clientMgr.create_plugin_node(type, name, inputChannels, outputChannels,
                        params, 0, created, error);
                }
                if (!ok) throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"plugin", plugin_node_json(*created)}}).dump(2), "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        post(R"(/api/plugins/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t id = 0;
            if (!parse_plugin_instance_id(request, id)) { dsp_error(response, 400, "invalid plugin instance ID"); return; }
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object()) throw std::invalid_argument("Plugin update must be an object");
                const auto current = m_clientMgr.get_plugin_nodes();
                const auto found = std::find_if(current.begin(), current.end(),
                    [id](const auto& item) { return item->id == id; });
                if (found == current.end()) throw std::invalid_argument("Plugin instance not found");
                const auto& old = *found;
                const std::string name = body.value("name", old->name);
                const bool active = body.value("active", old->active);
                const nlohmann::json params = body.value("params", old->params);
                std::shared_ptr<const PluginNodeInstance> updated;
                std::string error;
                bool ok = false;
                if (old->type == "plugin_chain") {
                    const nlohmann::json chain = body.value("chain", old->chain);
                    const float chainGain = body.value("chain_gain", old->chain_gain);
                    ok = m_clientMgr.update_plugin_chain_node(id, name, chain, chainGain,
                        active, body.value("channels", old->chain_channels), updated, error);
                } else if (body.contains("port_channels")) {
                    ok = m_clientMgr.update_plugin_node_with_ports(id, name, body["port_channels"],
                        params, active, updated, error);
                } else {
                    const unsigned inputChannels = body.value("input_channels", old->input_channels);
                    const unsigned outputChannels = body.value("output_channels", old->output_channels);
                    ok = m_clientMgr.update_plugin_node(id, name, inputChannels, outputChannels,
                        params, active, updated, error);
                }
                if (!ok) throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"plugin", plugin_node_json(*updated)}}).dump(2), "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        m_httpServer->Get("/api/midi/routes", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json routes = nlohmann::json::array();
            for (const auto& route : m_clientMgr.get_midi_routes())
                routes.push_back({{"id", route.id}, {"source", route.source_endpoint},
                                  {"destination", route.destination_endpoint}, {"enabled", route.enabled}});
            response.set_header("Cache-Control", "no-store");
            response.set_content(nlohmann::json({{"midi_routes", routes}}).dump(2), "application/json");
        });

        post("/api/midi/routes", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("source") || !body["source"].is_string() ||
                    !body.contains("destination") || !body["destination"].is_string())
                    throw std::invalid_argument("source and destination MIDI endpoints are required");
                uint32_t id = 0;
                std::string error;
                if (!m_clientMgr.add_midi_route(body["source"].get<std::string>(),
                    body["destination"].get<std::string>(), id, error))
                    throw std::invalid_argument(error);
                response.set_content(nlohmann::json({{"status", "ok"}, {"id", id}}).dump(2), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        m_httpServer->Delete(R"(/api/midi/routes/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            if (!m_running.load()) { dsp_error(response, 503, "server is stopping"); return; }
            uint32_t id = 0;
            if (!parse_plugin_instance_id(request, id)) { dsp_error(response, 400, "invalid MIDI route ID"); return; }
            std::string error;
            if (!m_clientMgr.remove_midi_route(id, error)) { dsp_error(response, 404, error); return; }
            const uint64_t target = m_dspEngine.request_adoption();
            const bool adopted = m_dspEngine.wait_for_revision(target, std::chrono::milliseconds(1500));
            response.status = adopted ? 200 : 503;
            response.set_content(nlohmann::json({{"status", adopted ? "ok" : "pending"},
                {"accepted_revision", target}, {"adopted_revision", m_dspEngine.adopted_revision()},
                {"adopted", adopted}, {"persisted", false}}).dump(), "application/json");
        });

        post("/api/plugins/extract-chain", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("route_id") || !body["route_id"].is_number_integer())
                    throw std::invalid_argument("route_id is required");
                const uint64_t rawRouteId = body["route_id"].get<uint64_t>();
                if (rawRouteId == 0 || rawRouteId > std::numeric_limits<uint32_t>::max())
                    throw std::invalid_argument("invalid route ID");
                const uint32_t routeId = static_cast<uint32_t>(rawRouteId);
                const auto routes = m_clientMgr.route_snapshot();
                const auto routeIt = std::find_if(routes->begin(), routes->end(),
                    [routeId](const AudioRoute& route) { return route.id == routeId; });
                if (routeIt == routes->end()) throw std::invalid_argument("Route not found");
                const AudioRoute originalRoute = *routeIt;

                RouteDspState originalDsp;
                {
                    std::lock_guard<std::mutex> lock(g_dspCacheLock);
                    const auto cached = g_dspSettingsCache.find(routeId);
                    if (cached != g_dspSettingsCache.end()) originalDsp = cached->second;
                }
                if (!originalDsp.warning.empty())
                    throw std::invalid_argument("Cannot convert a route with invalid saved DSP state: " + originalDsp.warning);
                if (!originalDsp.chain.is_array() || originalDsp.chain.empty())
                    throw std::invalid_argument("This route does not contain a processor chain to upgrade");

                const std::string nodeName = body.value("name", std::string("Plugin Chain"));
                std::shared_ptr<const PluginNodeInstance> chainNode;
                std::string error;
                if (!m_clientMgr.create_plugin_chain_node(nodeName, originalDsp.chain,
                        originalDsp.gain, originalDsp.active, body.value("channels", 1u),
                        0, chainNode, error))
                    throw std::invalid_argument(error);

                AudioRoute upstream;
                upstream.source_node = originalRoute.source_node;
                upstream.source_channel = originalRoute.source_channel;
                upstream.destination_node = chainNode->input_node;
                upstream.destination_channel = 0;
                upstream.gain = 1.0f;
                upstream.enabled = originalRoute.enabled;
                uint32_t upstreamId = 0;
                if (!m_clientMgr.add_route(upstream, upstreamId, error)) {
                    m_clientMgr.remove_plugin_node(chainNode->id, error);
                    throw std::invalid_argument("Could not create upstream chain-node connection: " + error);
                }
                if (!m_dspEngine.supports_route_dsp(upstreamId)) {
                    m_clientMgr.remove_route(upstreamId);
                    m_clientMgr.remove_plugin_node(chainNode->id, error);
                    throw std::invalid_argument("Upstream route ID exceeds the DSP route limit");
                }

                AudioRoute downstream;
                downstream.source_node = chainNode->output_node;
                downstream.source_channel = 0;
                downstream.destination_node = originalRoute.destination_node;
                downstream.destination_channel = originalRoute.destination_channel;
                downstream.gain = originalRoute.gain;
                downstream.enabled = originalRoute.enabled;
                uint32_t downstreamId = 0;
                if (!m_clientMgr.add_route(downstream, downstreamId, error)) {
                    m_clientMgr.remove_route(upstreamId);
                    m_clientMgr.remove_plugin_node(chainNode->id, error);
                    throw std::invalid_argument("Could not create downstream chain-node connection: " + error);
                }
                if (!m_dspEngine.supports_route_dsp(downstreamId)) {
                    m_clientMgr.remove_route(downstreamId);
                    m_clientMgr.remove_route(upstreamId);
                    m_clientMgr.remove_plugin_node(chainNode->id, error);
                    throw std::invalid_argument("Downstream route ID exceeds the DSP route limit");
                }

                if (!m_clientMgr.remove_route(routeId)) {
                    m_clientMgr.remove_route(downstreamId);
                    m_clientMgr.remove_route(upstreamId);
                    m_clientMgr.remove_plugin_node(chainNode->id, error);
                    throw std::runtime_error("Original route could not be removed; chain conversion was rolled back");
                }
                m_dspEngine.update_route_dsp(routeId, {}, 1.0f, false);
                {
                    std::lock_guard<std::mutex> lock(g_dspCacheLock);
                    g_dspSettingsCache.erase(routeId);
                }
                response.set_content(nlohmann::json({{"status", "ok"},
                    {"plugin_id", chainNode->id}, {"plugin", plugin_node_json(*chainNode)},
                    {"upstream_route_id", upstreamId}, {"downstream_route_id", downstreamId}}).dump(2), "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        post("/api/plugins/extract", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("route_id") || !body["route_id"].is_number_integer() ||
                    !body.contains("processor_index") || !body["processor_index"].is_number_unsigned())
                    throw std::invalid_argument("route_id and processor_index are required");
                const uint64_t rawRouteId = body["route_id"].get<uint64_t>();
                const uint64_t rawIndex = body["processor_index"].get<uint64_t>();
                if (rawRouteId == 0 || rawRouteId > std::numeric_limits<uint32_t>::max())
                    throw std::invalid_argument("invalid route ID");
                routeId = static_cast<uint32_t>(rawRouteId);
                const auto routes = m_clientMgr.route_snapshot();
                const auto routeIt = std::find_if(routes->begin(), routes->end(),
                    [routeId](const AudioRoute& route) { return route.id == routeId; });
                if (routeIt == routes->end()) throw std::invalid_argument("Route not found");
                const AudioRoute originalRoute = *routeIt;
                RouteDspState originalDsp;
                {
                    std::lock_guard<std::mutex> lock(g_dspCacheLock);
                    const auto cached = g_dspSettingsCache.find(routeId);
                    if (cached != g_dspSettingsCache.end()) originalDsp = cached->second;
                }
                if (!originalDsp.warning.empty())
                    throw std::invalid_argument("Cannot extract from a route with invalid saved DSP state: " + originalDsp.warning);
                if (!originalDsp.chain.is_array() || rawIndex >= originalDsp.chain.size())
                    throw std::invalid_argument("Selected processor index is outside the saved route chain");

                const auto selected = originalDsp.chain[static_cast<size_t>(rawIndex)];
                if (!selected.is_object() || !selected.contains("type") || !selected["type"].is_string() ||
                    !selected.contains("params") || !selected["params"].is_object())
                    throw std::invalid_argument("Selected processor has invalid saved data");

                nlohmann::json beforeChain = nlohmann::json::array();
                nlohmann::json afterChain = nlohmann::json::array();
                for (size_t i = 0; i < originalDsp.chain.size(); ++i) {
                    if (i < rawIndex) beforeChain.push_back(originalDsp.chain[i]);
                    else if (i > rawIndex) afterChain.push_back(originalDsp.chain[i]);
                }
                auto beforePrepared = prepare_route_dsp({{"active", originalDsp.active}, {"gain", 1.0}, {"chain", beforeChain}});
                auto afterPrepared = prepare_route_dsp({{"active", originalDsp.active}, {"gain", originalDsp.gain}, {"chain", afterChain}});

                std::shared_ptr<const PluginNodeInstance> plugin;
                std::string error;
                const std::string type = selected["type"].get<std::string>();
                const auto* descriptor = ProcessorRegistry::instance().find(type);
                if (!descriptor) throw std::invalid_argument("Selected processor is no longer registered: " + type);
                if (!m_clientMgr.create_plugin_node(type, descriptor->name, 1, 1,
                        selected["params"], 0, plugin, error))
                    throw std::invalid_argument(error);
                if (!originalDsp.active) {
                    std::shared_ptr<const PluginNodeInstance> bypassed;
                    if (!m_clientMgr.update_plugin_node(plugin->id, plugin->name, 1, 1,
                            plugin->params, false, bypassed, error)) {
                        m_clientMgr.remove_plugin_node(plugin->id, error);
                        throw std::invalid_argument(error);
                    }
                    plugin = bypassed;
                }

                AudioRoute beforeRoute;
                beforeRoute.source_node = originalRoute.source_node;
                beforeRoute.source_channel = originalRoute.source_channel;
                beforeRoute.destination_node = plugin->input_node;
                beforeRoute.destination_channel = 0;
                beforeRoute.gain = 1.0f;
                beforeRoute.enabled = originalRoute.enabled;
                uint32_t beforeId = 0;
                if (!m_clientMgr.add_route(beforeRoute, beforeId, error)) {
                    m_clientMgr.remove_plugin_node(plugin->id, error);
                    throw std::invalid_argument("Could not create upstream connection: " + error);
                }
                if (!m_dspEngine.supports_route_dsp(beforeId)) {
                    m_clientMgr.remove_route(beforeId);
                    m_clientMgr.remove_plugin_node(plugin->id, error);
                    throw std::invalid_argument("New route ID exceeds the current route-DSP capacity");
                }

                AudioRoute afterRoute;
                afterRoute.source_node = plugin->output_node;
                afterRoute.source_channel = 0;
                afterRoute.destination_node = originalRoute.destination_node;
                afterRoute.destination_channel = originalRoute.destination_channel;
                afterRoute.gain = originalRoute.gain;
                afterRoute.enabled = originalRoute.enabled;
                uint32_t afterId = 0;
                if (!m_clientMgr.add_route(afterRoute, afterId, error)) {
                    m_clientMgr.remove_route(beforeId);
                    m_clientMgr.remove_plugin_node(plugin->id, error);
                    throw std::invalid_argument("Could not create downstream connection: " + error);
                }
                if (!m_dspEngine.supports_route_dsp(afterId)) {
                    m_clientMgr.remove_route(afterId);
                    m_clientMgr.remove_route(beforeId);
                    m_clientMgr.remove_plugin_node(plugin->id, error);
                    throw std::invalid_argument("New route ID exceeds the current route-DSP capacity");
                }

                m_dspEngine.update_route_dsp(beforeId, std::move(beforePrepared.chain),
                                             beforePrepared.state.gain, beforePrepared.state.active);
                m_dspEngine.update_route_dsp(afterId, std::move(afterPrepared.chain),
                                             afterPrepared.state.gain, afterPrepared.state.active);
                {
                    std::lock_guard<std::mutex> lock(g_dspCacheLock);
                    g_dspSettingsCache[beforeId] = beforePrepared.state;
                    g_dspSettingsCache[afterId] = afterPrepared.state;
                }
                if (!m_clientMgr.remove_route(routeId)) {
                    m_clientMgr.remove_route(afterId);
                    m_clientMgr.remove_route(beforeId);
                    m_clientMgr.remove_plugin_node(plugin->id, error);
                    throw std::runtime_error("The original route could not be removed; extraction was rolled back");
                }

                response.set_content(nlohmann::json({{"status", "ok"}, {"plugin_id", plugin->id},
                    {"input_endpoint", plugin->input_endpoint}, {"output_endpoint", plugin->output_endpoint},
                    {"upstream_route_id", beforeId}, {"downstream_route_id", afterId}}).dump(2), "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        m_httpServer->Delete(R"(/api/plugins/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            if (!m_running.load()) { dsp_error(response, 503, "server is stopping"); return; }
            uint32_t id = 0;
            if (!parse_plugin_instance_id(request, id)) { dsp_error(response, 400, "invalid plugin instance ID"); return; }
            std::string error;
            if (!m_clientMgr.remove_plugin_node(id, error)) { dsp_error(response, 404, error); return; }
            const uint64_t target = m_dspEngine.request_adoption();
            const bool adopted = m_dspEngine.wait_for_revision(target, std::chrono::milliseconds(1500));
            bool persisted = false;
            if (adopted) {
                m_dspEngine.collect_retired_endpoints();
                persisted = m_clientMgr.save_settings(m_controls, m_tone);
                if (persisted) m_persistedRevision = target;
            }
            response.status = !adopted ? 503 : (persisted ? 200 : 500);
            response.set_content(nlohmann::json({{"status", !adopted ? "pending" : (persisted ? "ok" : "error")},
                {"accepted_revision", target}, {"adopted_revision", m_dspEngine.adopted_revision()},
                {"adopted", adopted}, {"persisted", persisted},
                {"message", !adopted ? "Plugin removal was not adopted" : (persisted ? "Plugin node removed" : "Plugin removed, but settings could not be saved")}}).dump(2), "application/json");
        });

        m_httpServer->Get("/api/dsp", [this](const httplib::Request&, httplib::Response& response) {
            auto* gainProc = m_dspEngine.get_gain_processor();
            auto* compProc = m_dspEngine.get_compressor();
            nlohmann::json j;
            j["gain"] = gainProc ? gainProc->get_gain() : 1.0f;
            j["threshold"] = compProc ? compProc->get_threshold() : 0.5f;
            j["ratio"] = compProc ? compProc->get_ratio() : 4.0f;
            response.set_content(j.dump(2), "application/json");
        });
        post("/api/dsp", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                auto* gainProc = m_dspEngine.get_gain_processor();
                auto* compProc = m_dspEngine.get_compressor();
                const float g = query_float(request, "gain", gainProc->get_gain());
                const float t = query_float(request, "threshold", compProc->get_threshold());
                const float r = query_float(request, "ratio", compProc->get_ratio());
                if (g < 0.0f || !std::isfinite(g)) throw std::invalid_argument("gain must be >= 0");
                if (t < 0.0f || !std::isfinite(t)) throw std::invalid_argument("threshold must be >= 0");
                if (r < 1.0f || !std::isfinite(r)) throw std::invalid_argument("ratio must be >= 1");
                gainProc->set_gain(g);
                compProc->set_threshold(t);
                compProc->set_ratio(r);
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& e) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(e.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Get("/api/clients", [this](const httplib::Request&, httplib::Response& response) {
            const auto sessions = m_clientMgr.get_active_sessions();
            nlohmann::json j;
            j["clients"] = nlohmann::json::array();
            for (const auto& s : sessions) {
                nlohmann::json sj;
                sj["id"] = s->id;
                sj["client_id"] = s->get_client_key();
                sj["name"] = s->get_client_name();
                sj["ip"] = s->remote_ip;
                sj["input_channels"] = s->input_channels.load();
                sj["output_channels"] = s->output_channels.load();
                sj["input_meters"] = build_meter_json_container(s->input_metrics, s->input_channels.load());
                sj["output_meters"] = build_meter_json_container(s->output_metrics, s->output_channels.load());
                j["clients"].push_back(sj);
            }
            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Get("/api/pipewire/graph", [this](const httplib::Request&, httplib::Response& response) {
            if (!m_pwEnabled) {
                nlohmann::json disabled;
                disabled["enabled"] = false;
                disabled["nodes"] = nlohmann::json::array();
                disabled["links"] = nlohmann::json::array();
                response.set_content(disabled.dump(), "application/json");
                return;
            }

            const auto graph = m_deviceMgr.get_pipewire_graph();
            nlohmann::json j;
            j["enabled"] = true;
            j["nodes"] = nlohmann::json::array();
            for (const auto& [id, node] : graph.nodes) {
                j["nodes"].push_back({
                    {"id", node.id},
                    {"name", node.name},
                    {"description", node.description},
                    {"app_name", node.app_name},
                    {"client_name", node.client_name},
                    {"client_id", node.client_id},
                    {"process_id", node.process_id},
                    {"object_serial", node.object_serial},
                    {"media_class", node.media_class},
                    {"is_input", node.is_input}
                });
            }

            j["links"] = nlohmann::json::array();
            for (const auto& link : graph.links) {
                j["links"].push_back({
                    {"source_node", link.source_node},
                    {"dest_node", link.dest_node},
                    {"source_port", link.source_port},
                    {"dest_port", link.dest_port}
                });
            }

            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Get("/api/graph", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            j["revision"] = m_dspEngine.requested_revision();
            j["adopted_revision"] = m_dspEngine.adopted_revision();
            j["persisted_revision"] = m_persistedRevision;
            
            std::unordered_map<std::string, nlohmann::json> endpointsMap;

            const auto activeHw = m_deviceMgr.get_active_devices();
            for (const auto& dev : activeHw) {
                const std::string id = dev->get_original_id();
                const std::string name = dev->get_info().name;
                if (dev->get_info().is_capture) {
                    std::string epId = "hardware/" + id + "/capture";
                    nlohmann::json ep;
                    ep["id"] = epId; ep["direction"] = "source"; ep["channels"] = dev->get_info().channels; ep["name"] = name;
                    ep["layout"] = stream_layout_json(dev->get_info());
                    endpointsMap[epId] = ep;
                } else {
                    std::string epId = "hardware/" + id + "/playback";
                    nlohmann::json ep;
                    ep["id"] = epId; ep["direction"] = "destination"; ep["channels"] = dev->get_info().channels; ep["name"] = name;
                    ep["layout"] = stream_layout_json(dev->get_info());
                    endpointsMap[epId] = ep;
                }
            }
            
            nlohmann::json toneEp;
            toneEp["id"] = "tone/generator"; toneEp["direction"] = "source"; toneEp["channels"] = 1;
            endpointsMap["tone/generator"] = toneEp;
            
            const auto sessions = m_clientMgr.get_active_sessions();
            for (const auto& s : sessions) {
                std::string key = s->get_client_key();
                std::string name = s->get_client_name();
                
                std::string capId = "client/" + key + "/capture";
                nlohmann::json capEp;
                capEp["id"] = capId; capEp["name"] = name; capEp["connected"] = true; capEp["direction"] = "source"; capEp["channels"] = s->input_channels.load();
                endpointsMap[capId] = capEp;

                std::string playId = "client/" + key + "/playback";
                nlohmann::json playEp;
                playEp["id"] = playId; playEp["name"] = name; playEp["connected"] = true; playEp["direction"] = "destination"; playEp["channels"] = s->output_channels.load();
                endpointsMap[playId] = playEp;
            }
            for (const auto& known : m_clientMgr.known_client_names()) {
                const std::string capId = "client/" + known.first + "/capture";
                const std::string playId = "client/" + known.first + "/playback";
                if (endpointsMap.find(capId) == endpointsMap.end()) {
                    nlohmann::json capEp;
                    capEp["id"] = capId; capEp["name"] = known.second; capEp["connected"] = false; capEp["direction"] = "source"; capEp["channels"] = 1;
                    endpointsMap[capId] = capEp;
                }
                if (endpointsMap.find(playId) == endpointsMap.end()) {
                    nlohmann::json playEp;
                    playEp["id"] = playId; playEp["name"] = known.second; playEp["connected"] = false; playEp["direction"] = "destination"; playEp["channels"] = 2;
                    endpointsMap[playId] = playEp;
                }
            }

            nlohmann::json pluginNodes = nlohmann::json::array();
            for (const auto& plugin : m_clientMgr.get_plugin_nodes()) {
                nlohmann::json ports = nlohmann::json::array();
                for (const auto& port : plugin->ports) {
                    nlohmann::json portJson = {
                        {"id", port.id}, {"name", port.name}, {"type", port.type},
                        {"direction", port.direction}, {"endpoint", port.endpoint}
                    };
                    if (port.type == "audio") {
                        portJson["channels"] = port.channels;
                        nlohmann::json endpoint = {
                            {"id", port.endpoint},
                            {"name", plugin->name + " — " + port.name},
                            {"direction", port.direction == "input" ? "destination" : "source"},
                            {"channels", port.channels},
                            {"kind", port.direction == "input" ? "plugin_input" : "plugin_output"},
                            {"plugin_id", plugin->id}, {"port_id", port.id}
                        };
                        endpointsMap[port.endpoint] = std::move(endpoint);
                    }
                    ports.push_back(std::move(portJson));
                }
                nlohmann::json item = plugin_node_json(*plugin);
                item["ports"] = std::move(ports);
                pluginNodes.push_back(std::move(item));
            }
            j["plugin_nodes"] = std::move(pluginNodes);

            nlohmann::json midiRoutes = nlohmann::json::array();
            for (const auto& route : m_clientMgr.get_midi_routes())
                midiRoutes.push_back({{"id", route.id}, {"source", route.source_endpoint},
                                      {"destination", route.destination_endpoint}, {"enabled", route.enabled}});
            j["midi_routes"] = std::move(midiRoutes);

            nlohmann::json endpoints = nlohmann::json::array();
            for (auto const& [id, ep] : endpointsMap) endpoints.push_back(ep);
            j["endpoints"] = endpoints;

            nlohmann::json routes = nlohmann::json::array();
            const auto routeList = m_clientMgr.get_routes();
            for (const auto& r : routeList) {
                nlohmann::json rj;
                rj["id"] = r.id;
                rj["source"] = m_clientMgr.get_endpoint_from_id(r.source_node);
                rj["source_channel"] = r.source_channel;
                rj["destination"] = m_clientMgr.get_endpoint_from_id(r.destination_node);
                rj["destination_channel"] = r.destination_channel;
                rj["gain"] = r.gain;
                rj["enabled"] = r.enabled;
                {
                    std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                    const auto cached = g_dspSettingsCache.find(r.id);
                    rj["dsp_active"] = cached != g_dspSettingsCache.end() && cached->second.active;
                }
                routes.push_back(rj);
            }
            j["routes"] = routes;
            response.set_content(j.dump(2), "application/json");
        });

        post("/api/routes", [this](const httplib::Request& request, httplib::Response& response) {
            AudioRoute route;
            // FIXED: Updated parameter names to match the frontend dashboard
            std::string src_ep = request.has_param("source_node") ? request.get_param_value("source_node") : "";
            std::string dst_ep = request.has_param("destination_node") ? request.get_param_value("destination_node") : "";
            route.source_node = m_clientMgr.get_node_id(src_ep);
            route.destination_node = m_clientMgr.get_node_id(dst_ep);
            route.source_channel = query_uint(request, "source_channel", CM5_MAX_CHANNELS);
            route.destination_channel = query_uint(request, "destination_channel", CM5_MAX_CHANNELS);
            route.gain = query_float(request, "gain", 1.0f);
            route.enabled = query_bool(request, "enabled", true);
            uint32_t id = 0; std::string error;
            if (!m_clientMgr.add_route(std::move(route), id, error)) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error) + "\"}", "application/json");
                return;
            }
            response.set_content("{\"status\":\"ok\",\"id\":\"" + std::to_string(id) + "\"}", "application/json");
        });

        m_httpServer->Delete(R"(/api/routes/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
            if (!m_running.load()) { dsp_error(response, 503, "server is stopping"); return; }
            uint32_t routeId = 0;
            if (!parse_route_dsp_id(request, routeId)) { dsp_error(response, 400, "invalid route id"); return; }
            if (!m_clientMgr.remove_route(routeId)) { response.status = 404; response.set_content("{\"status\":\"error\"}", "application/json"); return; }
            const auto target = m_dspEngine.request_adoption();
            const bool adopted = m_dspEngine.wait_for_revision(target, std::chrono::milliseconds(1500));
            response.status = adopted ? 200 : 503;
            response.set_content(nlohmann::json({{"status", adopted ? "ok" : "pending"},
                {"accepted_revision", target}, {"adopted_revision", m_dspEngine.adopted_revision()},
                {"adopted", adopted}, {"persisted", false}}).dump(), "application/json");
        });

        post("/api/routes/update", [this](const httplib::Request& request, httplib::Response& response) {
            const uint32_t id = query_uint(request, "id", 0);
            if (!id || !m_clientMgr.update_route(id, query_float(request, "gain", 1.0f), query_bool(request, "enabled", true))) {
                response.status = 400; response.set_content("{\"status\":\"error\"}", "application/json"); return;
            }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        m_httpServer->Get(R"(/api/routes/(\d+)/dsp)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            if (!parse_route_dsp_id(request, routeId)) { dsp_error(response, 400, "invalid route id"); return; }
            std::lock_guard<std::mutex> lock(g_dspCacheLock);
            if (!route_exists(m_clientMgr, routeId)) { dsp_error(response, 404, "route not found"); return; }
            if (!m_dspEngine.supports_route_dsp(routeId)) { dsp_error(response, 400, "Route ID exceeds DSP capacity"); return; }
            const auto cached = g_dspSettingsCache.find(routeId);
            const auto state = cached == g_dspSettingsCache.end() ? RouteDspState{} : cached->second;
            response.set_content(route_dsp_json(state).dump(2), "application/json");
        });

        post(R"(/api/routes/(\d+)/dsp)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            if (!parse_route_dsp_id(request, routeId)) { dsp_error(response, 400, "invalid route id"); return; }
            if (!route_exists(m_clientMgr, routeId)) { dsp_error(response, 404, "route not found"); return; }
            if (!m_dspEngine.supports_route_dsp(routeId)) { dsp_error(response, 400, "Route ID exceeds DSP capacity"); return; }

            try {
                auto prepared = prepare_route_dsp(nlohmann::json::parse(request.body));
                auto result = route_dsp_json(prepared.state);
                result["status"] = "ok";
                const auto content = result.dump(2);
                {
                    // Serialize engine publication and its canonical cache update.
                    // Snapshot reads do not acquire the route lock, avoiding inversion
                    // with settings persistence and route removal.
                    std::lock_guard<std::mutex> lock(g_dspCacheLock);
                    if (!route_exists(m_clientMgr, routeId)) { dsp_error(response, 404, "route not found"); return; }
                    m_dspEngine.update_route_dsp(routeId, std::move(prepared.chain), prepared.state.gain, prepared.state.active);
                    g_dspSettingsCache[routeId] = std::move(prepared.state);
                }
                response.set_content(content, "application/json");
            } catch (const std::exception& error) {
                dsp_error(response, 400, error.what());
            }
        });

        // Full inspector acceptance: validate every field before changing either
        // route state or DSP. Legacy endpoints remain available to API clients.
        post(R"(/api/routes/(\d+)/apply)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            if (!parse_route_dsp_id(request, routeId)) { dsp_error(response, 400, "invalid route id"); return; }
            if (!route_exists(m_clientMgr, routeId)) { dsp_error(response, 404, "route not found"); return; }
            if (!m_dspEngine.supports_route_dsp(routeId)) { dsp_error(response, 400, "Route ID exceeds DSP capacity"); return; }
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object() || !body.contains("gain") || !body["gain"].is_number() ||
                    !body.contains("enabled") || !body["enabled"].is_boolean() || !body.contains("dsp"))
                    throw std::invalid_argument("gain, enabled and dsp are required");
                const double gain = body["gain"].get<double>();
                if (!std::isfinite(gain) || gain < 0 || gain > 8) throw std::invalid_argument("gain must be finite and 0..8");
                auto prepared = prepare_route_dsp(body["dsp"]);
                auto result = route_dsp_json(prepared.state);
                result["status"] = "ok";
                result["route_gain"] = gain;
                result["enabled"] = body["enabled"];
                // All request validation and processor construction precede publication.
                // Ordinary route and DSP use separate publications: the final
                // adoption barrier confirms both; this is not sample-atomic across
                // those two groups or an allocation-failure rollback transaction.
                if (!m_clientMgr.update_route(routeId, static_cast<float>(gain), body["enabled"].get<bool>()))
                    throw std::invalid_argument("route update failed");
                {
                    std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                    m_dspEngine.update_route_dsp(routeId, std::move(prepared.chain), prepared.state.gain, prepared.state.active);
                    g_dspSettingsCache[routeId] = std::move(prepared.state);
                }
                response.set_content(result.dump(), "application/json");
            } catch (const std::exception& error) { dsp_error(response, 400, error.what()); }
        });

        post("/api/settings/save", [this](const httplib::Request&, httplib::Response& response) {
            const auto target = m_dspEngine.request_adoption();
            const bool adopted = m_dspEngine.wait_for_revision(target, std::chrono::milliseconds(1500));
            const bool persisted = adopted && m_clientMgr.save_settings(m_controls, m_tone);
            if (persisted) m_persistedRevision = target;
            response.status = !adopted ? 503 : persisted ? 200 : 500;
            response.set_content(nlohmann::json({{"status", persisted ? "ok" : adopted ? "error" : "pending"},
                {"accepted_revision", target}, {"adopted_revision", m_dspEngine.adopted_revision()},
                {"persisted_revision", m_persistedRevision}, {"adopted", adopted}, {"persisted", persisted},
                {"message", persisted ? "Applied configuration saved" : adopted ? "Applied, but settings could not be saved" : "Engine adoption not confirmed; settings were not saved"}}).dump(), "application/json");
        });

        post("/api/control", [this](const httplib::Request& request, httplib::Response& response) {
            // These banks are legacy storage, not connected sample controls. Do
            // not acknowledge a hardware gain/mute change that cannot be heard.
            dsp_error(response, 501, "Hardware channel gain/mute is not implemented in the sample path; use route gain or a Gain processor");
        });

        post("/api/reset_clips", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::mutex> metersLock(m_metrics.lock);
            try {
                if (request.has_param("all")) {
                    for (auto& [id, bank] : m_metrics.capture) {
                        for (auto& m : bank) { m.clipped.store(false); m.peak_hold_db.store(-60.0f); }
                    }
                    for (auto& [id, bank] : m_metrics.playback) {
                        for (auto& m : bank) { m.clipped.store(false); m.peak_hold_db.store(-60.0f); }
                    }
                }
                else {
                    const std::string endpoint = request.has_param("endpoint") ? request.get_param_value("endpoint") : "";
                    const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                    const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                    if (endpoint.empty() || (type != "capture" && type != "playback") || channel < 0 || channel >= static_cast<int>(CM5_MAX_CHANNELS)) throw std::invalid_argument("invalid parameters");
                    auto& banks = (type == "playback") ? m_metrics.playback : m_metrics.capture;
                    const auto found = banks.find(endpoint);
                    if (found == banks.end() || static_cast<size_t>(channel) >= found->second.size())
                        throw std::invalid_argument("endpoint or channel has no meter bank");
                    found->second[channel].clipped.store(false);
                    found->second[channel].peak_hold_db.store(-60.0f);
                }
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& response) { response.set_content(read_favicon(), "image/x-icon"); });

        m_httpServer->Get("/", [](const httplib::Request&, httplib::Response& response) { response.set_content(read_dashboard(), "text/html"); });
        const char* bind = std::getenv("CM5AUDIO_BIND_ADDRESS");
        const std::string host = bind && *bind ? bind : "0.0.0.0";
        if (m_httpPort == 0) m_httpPort = m_httpServer->bind_to_any_port(host);
        else if (!m_httpServer->bind_to_port(host, m_httpPort)) throw std::runtime_error("HTTP bind failed");
        if (m_httpPort < 0) throw std::runtime_error("HTTP bind failed");
        m_wsServer = std::make_unique<ReliableWebSocketServer>(m_wsPort, host);
        if (m_plainText) std::cerr << "[WS] Plain WS enabled" << std::endl;
        if (tlsEnabled) {
            ix::SocketTLSOptions tls;
            tls.certFile = certPath; tls.keyFile = keyPath; tls.caFile = "NONE"; tls.tls = true;
            m_wsServer->setTLSOptions(tls);
        }
        static std::atomic<uint32_t> nextId{1};
        m_wsServer->setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> weakSocket, std::shared_ptr<ix::ConnectionState> state) {
            auto socket = weakSocket.lock();
            if (!socket) return;
            const uint32_t id = nextId.fetch_add(1);
            const std::string remote = state ? state->getRemoteIp() : "";
            // Connection callback precedes the handshake in IX. Create no session
            // or sender until Open; failed handshakes then own no application work.
            auto holder = std::make_shared<std::shared_ptr<WebClientSession>>();
            socket->setOnMessageCallback([this, holder, id, remote, weakSocket](const ix::WebSocketMessagePtr& message) {
                auto socket = weakSocket.lock();
                if (!socket) return;
                if (message->type == ix::WebSocketMessageType::Close) {
                    m_clientMgr.remove_session(id); // Marks retirement; never joins here.
                    return;
                }
                if (!m_running.load()) { socket->close(); return; }
                if (message->type == ix::WebSocketMessageType::Open) {
                    {
                        std::lock_guard<std::recursive_mutex> control(m_clientMgr.control_mutex());
                        if (!m_running.load()) return;
                        *holder = m_clientMgr.create_session(id, remote);
                        (*holder)->socket = socket;
                    }
                    socket->sendText(nlohmann::json({{"type", "assigned"}, {"connection_id", id},
                        {"client_id", (*holder)->get_client_key()}, {"name", (*holder)->get_client_name()}}).dump());
                    return;
                }
                const auto session = *holder;
                if (!session || !session->active.load()) return;
                if (message->type == ix::WebSocketMessageType::Message && message->binary) {
                    if (session->media_ready.load(std::memory_order_acquire))
                        session->incoming_rb.write(message->str.data(), message->str.size());
                } else if (message->type == ix::WebSocketMessageType::Message) {
                    std::string key, name;
                    unsigned input = session->input_channels.load(), output = session->output_channels.load();
                    if (!parse_identity_message(message->str, key, name, input, output)) return;
                    const unsigned finalIn = std::clamp(input, 1u, CM5_MAX_CLIENT_CHANNELS);
                    const unsigned finalOut = std::clamp(output, 1u, CM5_MAX_CLIENT_CHANNELS);
                    std::unique_lock<std::recursive_mutex> control(m_clientMgr.control_mutex());
                    if (session->media_ready.load()) {
                        // Live resizing/renaming races ring-buffer users. Reconnect
                        // for different identity/channel counts; identical hello is idempotent.
                        const bool same = key == session->get_client_key() && finalIn == session->input_channels.load() && finalOut == session->output_channels.load();
                        control.unlock();
                        socket->sendText(nlohmann::json({{"type", same ? "identity_accepted" : "identity_conflict"},
                            {"client_id", key}, {"message", same ? "Already configured" : "Reconnect to change identity or channels"}}).dump());
                        return;
                    }
                    session->input_channels.store(finalIn);
                    session->output_channels.store(finalOut);
                    if (!session->incoming_rb.init(ma_format_f32, finalIn, 48000) ||
                        !session->outgoing_rb.init(ma_format_f32, finalOut, 48000)) { control.unlock(); socket->close(); return; }
                    // Allocate/start before claiming identity publishes an endpoint.
                    session->start_sender(socket);
                    if (!m_clientMgr.claim_identity(session, key, name)) {
                        session->active.store(false);
                        m_clientMgr.remove_session(id);
                        control.unlock();
                        socket->sendText(nlohmann::json({{"type", "identity_conflict"}, {"client_id", key}}).dump());
                        socket->close();
                    } else {
                        session->media_ready.store(true, std::memory_order_release);
                        m_dspEngine.mark_graph_dirty();
                        control.unlock();
                        socket->sendText(nlohmann::json({{"type", "identity_accepted"},
                            {"client_id", key}, {"name", session->get_client_name()}}).dump());
                    }
                }
            });
        });
        const auto result = m_wsServer->listen();
        if (!result.first) throw std::runtime_error("WebSocket bind failed: " + result.second);
        m_wsPort = m_wsServer->getPort();
        m_running.store(true);
        m_httpThread = std::thread([this] {
            if (!m_httpServer->listen_after_bind()) m_running.store(false);
        });
        m_httpServer->wait_until_ready();
        m_wsServer->start();
        m_sessionReaper = std::thread([this] {
            while (m_running.load()) {
                m_clientMgr.collect_retired_sessions();
                m_dspEngine.collect_retired_endpoints();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        std::cout << "[HTTP] Dashboard listening on " << (tlsEnabled ? "https" : "http") << "://" << host << ":" << m_httpPort << std::endl;
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[Server] Startup failed: " << error.what() << std::endl;
        m_running.store(false);
        if (m_httpServer) {
            m_httpServer->stop();
            if (auto* plain = dynamic_cast<ReliableHttpServer<httplib::Server>*>(m_httpServer.get())) plain->cancel_connections();
            if (auto* tls = dynamic_cast<ReliableHttpServer<httplib::SSLServer>*>(m_httpServer.get())) tls->cancel_connections();
        }
        if (m_wsServer) m_wsServer->shutdown();
        if (m_httpThread.joinable()) m_httpThread.join();
        if (m_sessionReaper.joinable()) m_sessionReaper.join();
        m_clientMgr.stop_sessions();
        m_httpServer.reset();
        m_wsServer.reset();
        return false;
    }
}

void WebServer::stop() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleLock);
    if (!m_httpServer && !m_wsServer && !m_httpThread.joinable() && !m_sessionReaper.joinable()) return;
    m_running.store(false);
    // Join HTTP workers before ending DSP: accepted mutations either finish their
    // adoption barrier or fail, rather than being interrupted midway through save.
    if (m_httpServer) {
        m_httpServer->stop();
        if (auto* plain = dynamic_cast<ReliableHttpServer<httplib::Server>*>(m_httpServer.get())) plain->cancel_connections();
        if (auto* tls = dynamic_cast<ReliableHttpServer<httplib::SSLServer>*>(m_httpServer.get())) tls->cancel_connections();
    }
    if (m_httpThread.joinable()) m_httpThread.join();
    m_dspEngine.stop(); // Endpoint owners remain valid through the last audio block.
    if (m_wsServer) m_wsServer->shutdown(); // Exactly one IX shutdown caller.
    if (m_sessionReaper.joinable()) m_sessionReaper.join();
    m_clientMgr.stop_sessions();
    m_dspEngine.collect_retired_endpoints();
    m_wsServer.reset();
    m_httpServer.reset();
}
