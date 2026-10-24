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
