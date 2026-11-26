#ifndef CM5AUDIO_MESH_NETWORK_H
#define CM5AUDIO_MESH_NETWORK_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Server; }

class ClientManager;
class DspEngine;

// Outbound CM5Audio-to-CM5Audio peers. Each peer uses the existing CM5Audio
// WebSocket audio protocol and appears locally as a persistent client capture
// source plus client playback sink.
class MeshNetworkManager {
public:
    explicit MeshNetworkManager(ClientManager& clients, DspEngine& dspEngine,
                                int controlPort = 8185);
    ~MeshNetworkManager();

    MeshNetworkManager(const MeshNetworkManager&) = delete;
    MeshNetworkManager& operator=(const MeshNetworkManager&) = delete;

    bool start();
    void stop();
    bool is_running() const { return m_running.load(std::memory_order_acquire); }
    int control_port() const { return m_controlPort; }

private:
    struct Peer;
    struct IncomingPeer {
        std::string key;
        std::string name;
        std::string host;
        unsigned input_channels = 2;
        unsigned output_channels = 2;
        bool connected = false;
    };

    ClientManager& m_clients;
    DspEngine& m_dspEngine;
    int m_controlPort;
    std::string m_instanceId;
    std::atomic<bool> m_running{false};
    std::unique_ptr<httplib::Server> m_httpServer;
    std::thread m_httpThread;
    std::mutex m_lifecycleLock;
    mutable std::mutex m_peersLock;
    std::vector<std::shared_ptr<Peer>> m_peers;
    std::vector<IncomingPeer> m_incomingPeers;
    std::atomic<uint32_t> m_nextPeerId{1};
    std::atomic<uint32_t> m_nextIncomingPlaceholderId{0x7fff0000u};

    std::thread m_monitorThread;

    bool load_settings();
    bool save_settings(std::string& error) const;
    bool add_peer(const std::string& name, const std::string& host,
                  unsigned port, unsigned sendChannels,
                  unsigned receiveChannels, uint32_t& id,
                  std::string& error);
    bool remove_peer(uint32_t id, std::string& error);
    std::shared_ptr<Peer> find_peer(uint32_t id) const;
    void start_peer_worker(const std::shared_ptr<Peer>& peer);
    void peer_worker(const std::shared_ptr<Peer>& peer);
    void monitor_loop();
    void restore_incoming_identity(const IncomingPeer& peer);
    static std::string html_page();
    void set_peer_disconnected(const std::shared_ptr<Peer>& peer,
                               const std::string& reason);
    static bool valid_host(const std::string& host);
};

#endif
