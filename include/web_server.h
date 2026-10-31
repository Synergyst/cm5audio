#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>
#include "ring_buffer.h"
#include "constants.h"
#include "audio_processor.h"
#include <nlohmann/json.hpp>

namespace ix { class WebSocket; class WebSocketServer; }
namespace httplib { class Server; class SSLServer; }
class ReliableWebSocketServer;

using NodeId = int32_t;
const NodeId NODE_INVALID = -1;
const NodeId NODE_TONE_GENERATOR = 0;

struct ChannelMeter {
    std::atomic<float> raw_value{0.0f};
    std::atomic<float> raw_peak{0.0f};
    std::atomic<float> rms_db{-60.0f};
    std::atomic<float> peak_db{-60.0f};
    std::atomic<float> peak_hold_db{-60.0f};
    std::atomic<bool> clipped{false};
};

struct ChannelControl {
    std::atomic<float> gain{1.0f};
    std::atomic<bool> mute{false};
};

struct AudioMetrics {
    mutable std::mutex lock; // Protect map structure; channel values remain atomic.
    std::unordered_map<std::string, std::vector<ChannelMeter>> capture;
    std::unordered_map<std::string, std::vector<ChannelMeter>> playback;
};

struct AudioControls {
    mutable std::mutex lock;
    std::unordered_map<std::string, std::array<ChannelControl, CM5_MAX_CONTROL_CHANNELS>> capture;
    std::unordered_map<std::string, std::array<ChannelControl, CM5_MAX_CONTROL_CHANNELS>> playback;
};

struct ToneControls {
    std::atomic<bool> enabled{false};
    std::atomic<float> frequency_hz{440.0f};
    std::atomic<float> amplitude{0.2f};
};

struct WebClientSession {
    uint32_t id = 0;
    std::string remote_ip;
    std::shared_ptr<const std::string> client_key;
    std::shared_ptr<const std::string> client_name;
    std::atomic<unsigned> input_channels{1};
    std::atomic<unsigned> output_channels{2};
    PcmRingBuffer incoming_rb;
    PcmRingBuffer outgoing_rb;
    std::vector<float> io_buffer;
    std::vector<float> input_block;
    std::vector<float> output_block;
    std::array<ChannelMeter, CM5_MAX_CLIENT_CHANNELS> input_metrics;
    std::array<ChannelMeter, CM5_MAX_CLIENT_CHANNELS> output_metrics;
    std::vector<float> packet_block;
    std::atomic<bool> active{true};
    std::atomic<bool> media_ready{false};
    std::weak_ptr<ix::WebSocket> socket;
    std::thread sender_thread;

    std::string get_client_key() const;
    std::string get_client_name() const;
    void set_identity(const std::string& key, const std::string& name);
    void start_sender(const std::shared_ptr<ix::WebSocket>& socket);
    void stop_sender();
    ~WebClientSession();
};

struct AudioRoute {
    uint32_t id = 0;
    NodeId source_node = NODE_INVALID;
    unsigned source_channel = 0;
    NodeId destination_node = NODE_INVALID;
    unsigned destination_channel = 0;
    float gain = 1.0f;
    bool enabled = true;
};

// A graph-native processor instance owns two virtual endpoints: a destination
// for its audio input bus and a source for its audio output bus. This lets the
// existing channel-to-channel route representation remain the patchbay cable
// model while the DSP engine executes the processor between those endpoints.
struct PluginPortMeterBank {
    explicit PluginPortMeterBank(unsigned channelCount)
        : channels(channelCount), meters(channelCount ? std::make_unique<ChannelMeter[]>(channelCount) : nullptr) {}

    unsigned channels = 0;
    std::unique_ptr<ChannelMeter[]> meters;
};

struct PluginPortInstance {
    std::string id;
    std::string name;
    std::string type;       // "audio" or "midi"
    std::string direction;  // "input" or "output"
    std::string endpoint;
    NodeId node = NODE_INVALID; // Audio ports only; MIDI ports are event endpoints.
    unsigned channels = 0;       // Audio ports only.
    std::shared_ptr<PluginPortMeterBank> meter_bank; // Audio input/output meters, allocated off the DSP thread.
    std::shared_ptr<MidiEventQueue> midi_queue; // MIDI inputs only.
    mutable std::array<MidiEvent, CM5_MIDI_MAX_EVENTS_PER_BLOCK> midi_block_events{};
    mutable size_t midi_block_event_count = 0;
};

struct PluginNodeInstance {
    uint32_t id = 0;
    std::string name;
    std::string type;
    std::string input_endpoint;  // Compatibility alias for primary audio input.
    std::string output_endpoint; // Compatibility alias for primary audio output.
    NodeId input_node = NODE_INVALID;
    NodeId output_node = NODE_INVALID;
    NodeId process_node = NODE_INVALID;
    unsigned input_channels = 1;
    unsigned output_channels = 1;
    bool active = true;
    nlohmann::json params = nlohmann::json::object();
    std::vector<PluginPortInstance> ports;
    std::shared_ptr<AudioProcessor> processor;
    // Populated only for the composite plugin_chain node type.
    nlohmann::json chain = nlohmann::json::array();
    float chain_gain = 1.0f;
};

struct PluginChainPreset {
    uint32_t id = 0;
    std::string name;
    nlohmann::json chain = nlohmann::json::array();
    float chain_gain = 1.0f;
    bool active = true;
};

struct MidiRoute {
    uint32_t id = 0;
    std::string source_endpoint;
    std::string destination_endpoint;
    bool enabled = true;
};

class DeviceManager;
class DspEngine;

class ClientManager {
public:
    ClientManager();
    ~ClientManager();
    std::recursive_mutex& control_mutex() const { return m_control_lock; }
    void collect_retired_sessions();
    void stop_sessions();
    void set_device_manager(DeviceManager* dm) { m_deviceManager = dm; }
    void set_dsp_engine(DspEngine* dsp) { m_dspEngine = dsp; }
    std::shared_ptr<WebClientSession> create_session(uint32_t id, const std::string& remoteIp = "");
    void remove_session(uint32_t id);
    std::vector<std::shared_ptr<WebClientSession>> get_active_sessions();
    bool claim_identity(const std::shared_ptr<WebClientSession>& session,
                       const std::string& key, const std::string& name);
    bool load_settings(AudioControls& controls, ToneControls& tone);
    bool save_settings(const AudioControls& controls, const ToneControls& tone) const;
    std::unordered_map<std::string, std::string> known_client_names() const;

    std::shared_ptr<const std::vector<AudioRoute>> route_snapshot() const;
    std::vector<AudioRoute> get_routes() const;
    bool add_route(AudioRoute route, uint32_t& assignedId, std::string& error);

    std::shared_ptr<const std::vector<std::shared_ptr<const PluginNodeInstance>>> plugin_node_snapshot() const;
    std::vector<std::shared_ptr<const PluginNodeInstance>> get_plugin_nodes() const;
    bool create_plugin_node(const std::string& type, const std::string& name,
                            unsigned inputChannels, unsigned outputChannels,
                            const nlohmann::json& params, uint32_t requestedId,
                            std::shared_ptr<const PluginNodeInstance>& created,
                            std::string& error);
    bool create_plugin_node_with_ports(const std::string& type, const std::string& name,
                            const nlohmann::json& portChannels, const nlohmann::json& params,
                            uint32_t requestedId,
                            std::shared_ptr<const PluginNodeInstance>& created,
                            std::string& error);
    bool update_plugin_node(uint32_t id, const std::string& name,
                            unsigned inputChannels, unsigned outputChannels,
                            const nlohmann::json& params, bool active,
                            std::shared_ptr<const PluginNodeInstance>& updated,
                            std::string& error);
    bool update_plugin_node_with_ports(uint32_t id, const std::string& name,
                            const nlohmann::json& portChannels, const nlohmann::json& params,
                            bool active, std::shared_ptr<const PluginNodeInstance>& updated,
                            std::string& error);
    bool create_plugin_chain_node(const std::string& name, const nlohmann::json& chain,
                            float chainGain, bool active, uint32_t requestedId,
                            std::shared_ptr<const PluginNodeInstance>& created,
                            std::string& error);
    bool update_plugin_chain_node(uint32_t id, const std::string& name,
                            const nlohmann::json& chain, float chainGain, bool active,
                            std::shared_ptr<const PluginNodeInstance>& updated,
                            std::string& error);
    std::vector<PluginChainPreset> get_plugin_chain_presets() const;
    bool get_plugin_chain_preset(uint32_t id, PluginChainPreset& preset) const;
    bool save_plugin_chain_preset(const std::string& name, const nlohmann::json& chain,
                            float chainGain, bool active, uint32_t requestedId,
                            PluginChainPreset& saved, std::string& error);
    bool delete_plugin_chain_preset(uint32_t id, std::string& error);
    bool remove_plugin_node(uint32_t id, std::string& error);
    std::shared_ptr<const std::vector<MidiRoute>> midi_route_snapshot() const;
    std::vector<MidiRoute> get_midi_routes() const;
    bool add_midi_route(const std::string& sourceEndpoint, const std::string& destinationEndpoint,
                        uint32_t& assignedId, std::string& error);
    bool remove_midi_route(uint32_t id, std::string& error);
    bool remove_route(uint32_t routeId);
    bool update_route(uint32_t routeId, float gain, bool enabled);
    void update_activated_device(const std::string& id, bool active);
    void set_device_channels(const std::string& id, unsigned capture, unsigned playback);
    std::pair<unsigned, unsigned> device_channels(const std::string& id) const;

    // Node ID Management
    NodeId get_node_id(const std::string& endpoint) const;
    std::string get_endpoint_from_id(NodeId id) const;
    NodeId register_node(const std::string& endpoint);
    void unregister_node(const std::string& endpoint);

private:
    bool endpoint_exists(const std::string& endpoint, bool source) const;
    mutable std::recursive_mutex m_control_lock;
    std::vector<std::shared_ptr<WebClientSession>> m_retired_sessions;
    mutable std::mutex m_lock;
    std::vector<std::shared_ptr<WebClientSession>> m_sessions;
    mutable std::mutex m_route_lock;
    std::shared_ptr<const std::vector<AudioRoute>> m_routes;
    std::unordered_map<std::string, std::string> m_known_client_names;
    std::vector<std::string> m_activatedDevices;
    std::unordered_map<std::string, std::pair<unsigned, unsigned>> m_deviceChannels;
    std::atomic<uint32_t> m_next_route_id{1};
    mutable std::mutex m_plugin_lock;
    std::shared_ptr<const std::vector<std::shared_ptr<const PluginNodeInstance>>> m_pluginNodes;
    std::atomic<uint32_t> m_next_plugin_id{1};
    mutable std::mutex m_chain_preset_lock;
    std::vector<PluginChainPreset> m_pluginChainPresets;
    std::atomic<uint32_t> m_next_chain_preset_id{1};
    mutable std::mutex m_midi_route_lock;
    std::shared_ptr<const std::vector<MidiRoute>> m_midiRoutes;
    std::atomic<uint32_t> m_next_midi_route_id{1};
    DeviceManager* m_deviceManager = nullptr;
    DspEngine* m_dspEngine = nullptr;

    // Node ID Mapping
    mutable std::mutex m_node_lock;
    std::unordered_map<std::string, NodeId> m_endpoint_to_id;
    std::unordered_map<NodeId, std::string> m_id_to_endpoint;
    std::atomic<NodeId> m_next_node_id{1};
};

class WebServer {
public:
    WebServer(AudioMetrics& metrics, AudioControls& controls, ToneControls& tone,
              ClientManager& clientMgr, DeviceManager& deviceMgr, DspEngine& dspEngine, 
              int httpPort = 8182, int wsPort = 8183, bool plainText = false, bool enablePwPatchbay = false);
    ~WebServer();
    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }
    int http_port() const { return m_httpPort; }
    int ws_port() const { return m_wsPort; }

private:
    AudioMetrics& m_metrics;
    AudioControls& m_controls;
    ToneControls& m_tone;
    ClientManager& m_clientMgr;
    DeviceManager& m_deviceMgr;
    DspEngine& m_dspEngine;
    int m_httpPort;
    int m_wsPort;
    bool m_plainText;
    bool m_pwEnabled;
    std::atomic<bool> m_running{false};
    std::mutex m_lifecycleLock;
    uint64_t m_persistedRevision = 0;
    
    std::unique_ptr<httplib::Server> m_httpServer;
    std::unique_ptr<ReliableWebSocketServer> m_wsServer;
    
    std::thread m_httpThread;
    std::thread m_sessionReaper;
};

#endif
