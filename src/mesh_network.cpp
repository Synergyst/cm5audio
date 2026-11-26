#include "mesh_network.h"

#include "web_server.h"
#include "dsp_engine.h"
#include "httplib.h"
#include "reliable_http_server.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace {
constexpr unsigned kMeshSampleRate = 48000;
constexpr unsigned kMeshRingFrames = kMeshSampleRate;
constexpr uint32_t kMeshSessionIdBase = 0x80000000u;

std::string mesh_key(const std::string& instanceId, uint32_t id) {
    return "cm5mesh-" + instanceId + "-" + std::to_string(id);
}

std::string generate_instance_id() {
    std::array<unsigned char, 16> bytes{};
    try {
        std::random_device random;
        for (auto& byte : bytes) byte = static_cast<unsigned char>(random() & 0xffu);
    } catch (...) {
        // Some minimal Linux images do not expose random_device entropy. The
        // process/time-seeded fallback is for stable identity, not a secret.
        const auto ticks = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::mt19937_64 fallback(ticks ^ (static_cast<uint64_t>(::getpid()) << 32));
        for (size_t i = 0; i < bytes.size(); i += 8) {
            const uint64_t word = fallback();
            for (size_t j = 0; j < 8; ++j)
                bytes[i + j] = static_cast<unsigned char>((word >> (j * 8)) & 0xffu);
        }
    }

    std::ostringstream value;
    for (unsigned index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value << '-';
        value << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned>(bytes[index]);
    }
    return value.str();
}

uint32_t mesh_session_id(uint32_t id) {
    return kMeshSessionIdBase + id;
}

std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string strip_mesh_status(const std::string& value) {
    for (const char* marker : {"🟢 ", "🔴 "}) {
        const std::string prefix(marker);
        if (value.compare(0, prefix.size(), prefix) == 0)
            return value.substr(prefix.size());
    }
    return value;
}

bool valid_name(const std::string& value) {
    return !value.empty() && value.size() <= 80 &&
           value.find('\n') == std::string::npos &&
           value.find('\r') == std::string::npos;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

nlohmann::json error_json(const std::string& message) {
    return {{"status", "error"}, {"message", message}};
}

} // namespace

struct MeshNetworkManager::Peer {
    uint32_t id = 0;
    std::string name;
    std::string host;
    unsigned port = 8183;
    unsigned send_channels = 2;
    unsigned receive_channels = 2;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> successful_connections{0};
    std::mutex state_lock;
    std::condition_variable state_changed;
    std::string last_error;
    bool connection_closed = false;
    bool identity_accepted = false;
    std::shared_ptr<ix::WebSocket> socket;
    std::shared_ptr<WebClientSession> session;
    std::thread worker;
};

MeshNetworkManager::MeshNetworkManager(ClientManager& clients,
                                       DspEngine& dspEngine,
                                       int controlPort)
    : m_clients(clients), m_dspEngine(dspEngine), m_controlPort(controlPort),
      m_instanceId(generate_instance_id()) {
}

MeshNetworkManager::~MeshNetworkManager() {
    stop();
}

bool MeshNetworkManager::valid_host(const std::string& host) {
    if (host.empty() || host.size() > 255) return false;
    if (host.front() == '.' || host.back() == '.') return false;
    for (const unsigned char ch : host) {
        if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == ':' ||
              ch == '[' || ch == ']' || ch == '%'))
            return false;
    }
    return true;
}


std::string MeshNetworkManager::html_page() {
    const std::string page = read_file("./web_client/mesh.html");
    if (!page.empty()) return page;
    return "<!doctype html><html><body><h1>CM5Audio Mesh UI unavailable</h1>"
           "<p>Run cm5audio from its project directory so web_client/mesh.html is available.</p>"
           "</body></html>";
}

bool MeshNetworkManager::load_settings() {
    std::ifstream file("./cm5audio_mesh.json");
    if (!file) {
        m_instanceId = generate_instance_id();
        return true; // A missing mesh settings file is a normal first-run state.
    }

    try {
        const auto document = nlohmann::json::parse(file);
        if (!document.is_object()) return false;
        m_instanceId = document.value("instance_id", std::string());
        if (m_instanceId.empty() || m_instanceId.size() > 48 ||
            !std::all_of(m_instanceId.begin(), m_instanceId.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '-';
            })) m_instanceId = generate_instance_id();
        if (!document.contains("peers") || !document["peers"].is_array())
            return false;

        std::vector<std::shared_ptr<Peer>> loaded;
        std::vector<IncomingPeer> loadedIncoming;
        uint32_t nextId = 1;
        for (const auto& value : document["peers"]) {
            try {
                if (!value.is_object()) continue;
                const uint32_t id = value.at("id").get<uint32_t>();
                const std::string name = trim_copy(value.at("name").get<std::string>());
                const std::string host = trim_copy(value.at("host").get<std::string>());
                const unsigned port = value.at("port").get<unsigned>();
                const unsigned sendChannels = value.value("send_channels", 2u);
                const unsigned receiveChannels = value.value("receive_channels", 2u);

                if (id == 0 || id >= 0x7fffffffu || !valid_name(name) ||
                    !valid_host(host) || port == 0 || port > 65535 ||
                    sendChannels == 0 || sendChannels > CM5_MAX_CLIENT_CHANNELS ||
                    receiveChannels == 0 || receiveChannels > CM5_MAX_CLIENT_CHANNELS)
                    continue;
                if (std::any_of(loaded.begin(), loaded.end(),
                    [id](const auto& peer) { return peer->id == id; }))
                    continue;

                auto peer = std::make_shared<Peer>();
                peer->id = id;
                peer->name = name;
                peer->host = host;
                peer->port = port;
                peer->send_channels = sendChannels;
                peer->receive_channels = receiveChannels;
                loaded.push_back(peer);
                nextId = std::max(nextId, id + 1);
            } catch (...) {
                // One damaged peer entry must not prevent valid peers from loading.
            }
        }

        const std::string ownPrefix = "cm5mesh-" + m_instanceId + "-";
        if (document.contains("incoming_peers") && document["incoming_peers"].is_array()) {
            for (const auto& value : document["incoming_peers"]) {
                try {
                    if (!value.is_object()) continue;
                    IncomingPeer incoming;
                    incoming.key = value.at("key").get<std::string>();
                    incoming.name = trim_copy(value.at("name").get<std::string>());
                    incoming.host = trim_copy(value.value("host", std::string()));
                    incoming.input_channels = value.value("input_channels", 2u);
                    incoming.output_channels = value.value("output_channels", 2u);
                    incoming.connected = false;
                    if (incoming.key.rfind("cm5mesh-", 0) != 0 ||
                        incoming.key.rfind(ownPrefix, 0) == 0 ||
                        !valid_name(incoming.name) ||
                        (!incoming.host.empty() && !valid_host(incoming.host)) ||
                        incoming.input_channels == 0 || incoming.input_channels > CM5_MAX_CLIENT_CHANNELS ||
                        incoming.output_channels == 0 || incoming.output_channels > CM5_MAX_CLIENT_CHANNELS)
                        continue;
                    if (std::any_of(loadedIncoming.begin(), loadedIncoming.end(),
                        [&](const IncomingPeer& item) { return item.key == incoming.key; }))
                        continue;
                    loadedIncoming.push_back(std::move(incoming));
                } catch (...) {
                    // Ignore one malformed remembered incoming peer.
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_peersLock);
            m_peers = std::move(loaded);
            m_incomingPeers = std::move(loadedIncoming);
            m_nextPeerId.store(nextId, std::memory_order_relaxed);
        }
        std::string ignored;
        save_settings(ignored);
        return true;
    } catch (const std::exception& error) {
        if (m_instanceId.empty()) m_instanceId = generate_instance_id();
        std::cerr << "[Mesh] Cannot read cm5audio_mesh.json: " << error.what() << std::endl;
        return false;
    }
}

bool MeshNetworkManager::save_settings(std::string& error) const {
    nlohmann::json document;
    document["version"] = 1;
    document["instance_id"] = m_instanceId;
    document["peers"] = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(m_peersLock);
        for (const auto& peer : m_peers) {
            if (!peer) continue;
            document["peers"].push_back({
                {"id", peer->id}, {"name", peer->name}, {"host", peer->host},
                {"port", peer->port},
                {"send_channels", peer->send_channels},
                {"receive_channels", peer->receive_channels}
            });
        }
        document["incoming_peers"] = nlohmann::json::array();
        for (const auto& peer : m_incomingPeers) {
            document["incoming_peers"].push_back({
                {"key", peer.key}, {"name", peer.name}, {"host", peer.host},
                {"input_channels", peer.input_channels},
                {"output_channels", peer.output_channels}
            });
        }
    }

    const std::string path = "./cm5audio_mesh.json";
    const std::string temporary = path + ".tmp." + std::to_string(::getpid());
    try {
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("cannot create temporary mesh settings file");
            file << document.dump(2) << '\n';
            file.flush();
            if (!file) throw std::runtime_error("cannot write mesh settings file");
        }
        if (::rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error("cannot replace cm5audio_mesh.json");
        return true;
    } catch (const std::exception& exception) {
        ::unlink(temporary.c_str());
        error = exception.what();
        return false;
    }
}

std::shared_ptr<MeshNetworkManager::Peer> MeshNetworkManager::find_peer(uint32_t id) const {
    std::lock_guard<std::mutex> lock(m_peersLock);
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
        [id](const auto& peer) { return peer && peer->id == id; });
    return found == m_peers.end() ? nullptr : *found;
}

bool MeshNetworkManager::add_peer(const std::string& rawName,
                                  const std::string& rawHost,
                                  unsigned port,
                                  unsigned sendChannels,
                                  unsigned receiveChannels,
                                  uint32_t& id, std::string& error) {
    const std::string name = trim_copy(rawName);
    const std::string host = trim_copy(rawHost);
    if (!valid_name(name)) { error = "Peer name must contain 1 to 80 characters"; return false; }
    if (!valid_host(host)) { error = "Enter a valid IP address or hostname"; return false; }
    if (port == 0 || port > 65535) { error = "WebSocket port must be between 1 and 65535"; return false; }
    if (sendChannels == 0 || sendChannels > CM5_MAX_CLIENT_CHANNELS ||
        receiveChannels == 0 || receiveChannels > CM5_MAX_CLIENT_CHANNELS) {
        error = "Channel counts must be between 1 and " + std::to_string(CM5_MAX_CLIENT_CHANNELS);
        return false;
    }

    auto peer = std::make_shared<Peer>();
    peer->id = m_nextPeerId.fetch_add(1, std::memory_order_relaxed);
    if (peer->id == 0 || peer->id >= 0x7fffffffu) {
        error = "Mesh peer ID space exhausted";
        return false;
    }
    peer->name = name;
    peer->host = host;
    peer->port = port;
    peer->send_channels = sendChannels;
    peer->receive_channels = receiveChannels;

    {
        std::lock_guard<std::mutex> lock(m_peersLock);
        const auto duplicate = std::find_if(m_peers.begin(), m_peers.end(),
            [&](const auto& item) {
                return item && item->host == host && item->port == port;
            });
        if (duplicate != m_peers.end()) {
            error = "A peer with this address and port is already configured";
            return false;
        }
        m_peers.push_back(peer);
    }

    if (!save_settings(error)) {
        std::lock_guard<std::mutex> lock(m_peersLock);
        m_peers.erase(std::remove(m_peers.begin(), m_peers.end(), peer), m_peers.end());
        return false;
    }

    id = peer->id;
    // Allocate stable client endpoint IDs immediately, even when the remote
    // host is offline. The temporary session is removed but its identity and
    // endpoint mapping intentionally remain registered in ClientManager.
    const uint32_t sessionId = mesh_session_id(peer->id);
    auto placeholder = m_clients.create_session(sessionId, peer->host);
    if (placeholder) {
        placeholder->input_channels.store(peer->receive_channels, std::memory_order_relaxed);
        placeholder->output_channels.store(peer->send_channels, std::memory_order_relaxed);
    }
    const bool identityReady = placeholder && m_clients.claim_identity(
        placeholder, mesh_key(m_instanceId, peer->id), "🔴 " + peer->name);
    if (placeholder) m_clients.remove_session(sessionId);
    if (!identityReady) {
        {
            std::lock_guard<std::mutex> lock(m_peersLock);
            m_peers.erase(std::remove(m_peers.begin(), m_peers.end(), peer), m_peers.end());
        }
        std::string ignored;
        save_settings(ignored);
        error = "CM5Audio could not reserve patchbay endpoints for this peer (node limit reached or identity already in use)";
        return false;
    }

    start_peer_worker(peer);
    return true;
}

bool MeshNetworkManager::remove_peer(uint32_t id, std::string& error) {
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lock(m_peersLock);
        const auto found = std::find_if(m_peers.begin(), m_peers.end(),
            [id](const auto& item) { return item && item->id == id; });
        if (found == m_peers.end()) { error = "Mesh peer not found"; return false; }
        peer = *found;
        m_peers.erase(found);
    }

    peer->stop_requested.store(true, std::memory_order_release);
    peer->state_changed.notify_all();
    // The worker owns the WebSocket lifecycle and performs stop() itself.
    // Avoid racing two concurrent IXWebSocket::stop() calls.
    if (peer->worker.joinable()) peer->worker.join();

    // Deliberately retain the disconnected graph endpoints and their stable IDs.
    // Removing a peer from the reconnect list must not delete saved routes.
    const uint32_t sessionId = mesh_session_id(peer->id);
    m_clients.remove_session(sessionId);
    auto placeholder = m_clients.create_session(sessionId, peer->host);
    if (placeholder) {
        placeholder->input_channels.store(peer->receive_channels, std::memory_order_relaxed);
        placeholder->output_channels.store(peer->send_channels, std::memory_order_relaxed);
    }
    if (placeholder && m_clients.claim_identity(
            placeholder, mesh_key(m_instanceId, peer->id), "🔴 " + peer->name)) {
        m_clients.remove_session(sessionId);
    } else if (placeholder) {
        m_clients.remove_session(sessionId);
    }

    if (!save_settings(error)) return false;
    return true;
}

void MeshNetworkManager::start_peer_worker(const std::shared_ptr<Peer>& peer) {
    if (!m_running.load(std::memory_order_acquire) || !peer || peer->worker.joinable()) return;
    peer->stop_requested.store(false, std::memory_order_release);
    peer->worker = std::thread(&MeshNetworkManager::peer_worker, this, peer);
}

void MeshNetworkManager::set_peer_disconnected(const std::shared_ptr<Peer>& peer,
                                               const std::string& reason) {
    if (!peer) return;
    peer->connected.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(peer->state_lock);
        if (!reason.empty()) peer->last_error = reason;
    }
    const uint32_t sessionId = mesh_session_id(peer->id);
    m_clients.remove_session(sessionId);

    // Keep disconnected endpoints discoverable and visually marked red. A
    // short-lived identity-only session updates ClientManager's known-client
    // name table without introducing a second graph or DSP endpoint type.
    auto placeholder = m_clients.create_session(sessionId, peer->host);
    if (placeholder) {
        placeholder->input_channels.store(peer->receive_channels, std::memory_order_relaxed);
        placeholder->output_channels.store(peer->send_channels, std::memory_order_relaxed);
    }
    if (placeholder && m_clients.claim_identity(
            placeholder, mesh_key(m_instanceId, peer->id), "🔴 " + peer->name)) {
        m_clients.remove_session(sessionId);
    } else if (placeholder) {
        m_clients.remove_session(sessionId);
    }
}

void MeshNetworkManager::restore_incoming_identity(const IncomingPeer& peer) {
    const uint32_t sessionId = m_nextIncomingPlaceholderId.fetch_add(1, std::memory_order_relaxed);
    if (sessionId < 0x7fff0000u || sessionId >= 0x80000000u) return;
    auto placeholder = m_clients.create_session(sessionId, peer.host);
    if (!placeholder) return;
    placeholder->input_channels.store(peer.input_channels, std::memory_order_relaxed);
    placeholder->output_channels.store(peer.output_channels, std::memory_order_relaxed);
    const bool restored = m_clients.claim_identity(placeholder, peer.key, "🔴 " + peer.name);
    m_clients.remove_session(sessionId);
    if (!restored)
        std::cerr << "[Mesh] Could not restore incoming patchbay identity for "
                  << peer.name << " (node limit reached or identity already in use)" << std::endl;
}

void MeshNetworkManager::monitor_loop() {
    const std::string ownPrefix = "cm5mesh-" + m_instanceId + "-";
    while (m_running.load(std::memory_order_acquire)) {
        std::unordered_map<std::string, std::shared_ptr<WebClientSession>> active;
        for (const auto& session : m_clients.get_active_sessions()) {
            if (!session || !session->media_ready.load(std::memory_order_acquire)) continue;
            const std::string key = session->get_client_key();
            if (key.rfind("cm5mesh-", 0) != 0 || key.rfind(ownPrefix, 0) == 0) continue;
            active[key] = session;
        }

        std::vector<IncomingPeer> changed;
        {
            std::lock_guard<std::mutex> lock(m_peersLock);
            for (const auto& entry : active) {
                const auto& session = entry.second;
                const std::string name = strip_mesh_status(session->get_client_name());
                const unsigned inputChannels = session->input_channels.load(std::memory_order_relaxed);
                const unsigned outputChannels = session->output_channels.load(std::memory_order_relaxed);
                auto found = std::find_if(m_incomingPeers.begin(), m_incomingPeers.end(),
                    [&](const IncomingPeer& item) { return item.key == entry.first; });
                if (found == m_incomingPeers.end()) {
                    IncomingPeer peer;
                    peer.key = entry.first;
                    peer.name = valid_name(name) ? name : entry.first;
                    peer.host = session->remote_ip;
                    peer.input_channels = inputChannels;
                    peer.output_channels = outputChannels;
                    peer.connected = true;
                    m_incomingPeers.push_back(peer);
                    changed.push_back(peer);
                } else {
                    const bool update = !found->connected || found->name != name ||
                        found->host != session->remote_ip ||
                        found->input_channels != inputChannels ||
                        found->output_channels != outputChannels;
                    found->connected = true;
                    if (valid_name(name)) found->name = name;
                    found->host = session->remote_ip;
                    found->input_channels = inputChannels;
                    found->output_channels = outputChannels;
                    if (update) changed.push_back(*found);
                }
            }

            for (auto& peer : m_incomingPeers) {
                if (!peer.connected || active.find(peer.key) != active.end()) continue;
                peer.connected = false;
                changed.push_back(peer);
            }
        }

        for (const auto& peer : changed) {
            const auto found = active.find(peer.key);
            if (found != active.end()) {
                found->second->set_identity(peer.key, "🟢 " + peer.name);
            } else {
                restore_incoming_identity(peer);
            }
        }
        if (!changed.empty()) {
            std::string error;
            if (!save_settings(error) && !error.empty())
                std::cerr << "[Mesh] Cannot persist incoming peer state: " << error << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void MeshNetworkManager::peer_worker(const std::shared_ptr<Peer>& peer) {
    unsigned retrySeconds = 1;
    const uint32_t sessionId = mesh_session_id(peer->id);
    const std::string key = mesh_key(m_instanceId, peer->id);
    const std::string scheme = "ws://";
    std::string host = peer->host;
    if (host.find(':') != std::string::npos && host.front() != '[')
        host = "[" + host + "]";
    const std::string url = scheme + host + ":" + std::to_string(peer->port) + "/";

    while (m_running.load(std::memory_order_acquire) &&
           !peer->stop_requested.load(std::memory_order_acquire)) {
        auto socket = std::make_shared<ix::WebSocket>();
        {
            std::lock_guard<std::mutex> lock(peer->state_lock);
            peer->socket = socket;
            peer->connection_closed = false;
            peer->identity_accepted = false;
            peer->session.reset();
        }

        socket->setUrl(url);
        socket->setPingInterval(15);
        socket->setHandshakeTimeout(5);

        std::weak_ptr<ix::WebSocket> weakSocket = socket;
        socket->setOnMessageCallback([this, peer, weakSocket, sessionId, key](
            const ix::WebSocketMessagePtr& message) {
            if (!message) return;
            auto socket = weakSocket.lock();
            if (!socket) return;

            if (message->type == ix::WebSocketMessageType::Open) {
                try {
                    auto session = m_clients.create_session(sessionId, peer->host);
                    session->input_channels.store(peer->receive_channels, std::memory_order_relaxed);
                    session->output_channels.store(peer->send_channels, std::memory_order_relaxed);
                    if (!session->incoming_rb.init(ma_format_f32, peer->receive_channels, kMeshRingFrames) ||
                        !session->outgoing_rb.init(ma_format_f32, peer->send_channels, kMeshRingFrames)) {
                        m_clients.remove_session(sessionId);
                        throw std::runtime_error("cannot allocate peer audio ring buffers");
                    }
                    session->socket = socket;
                    if (!m_clients.claim_identity(session, key, "🟢 " + peer->name)) {
                        m_clients.remove_session(sessionId);
                        throw std::runtime_error("local mesh identity is already in use");
                    }
                    {
                        std::lock_guard<std::mutex> lock(peer->state_lock);
                        peer->session = session;
                    }
                    session->start_sender(socket);

                    // From the server's perspective, input_channels is the
                    // audio this client sends (our local capture), while
                    // output_channels is what the server sends back to us.
                    const nlohmann::json hello = {
                        {"type", "hello"}, {"client_id", key},
                        {"client_name", peer->name},
                        {"input_channels", peer->send_channels},
                        {"output_channels", peer->receive_channels}
                    };
                    socket->sendText(hello.dump());
                } catch (const std::exception& error) {
                    {
                        std::lock_guard<std::mutex> lock(peer->state_lock);
                        peer->last_error = error.what();
                        peer->connection_closed = true;
                    }
                    peer->state_changed.notify_all();
                    socket->close();
                }
                return;
            }

            if (message->type == ix::WebSocketMessageType::Message) {
                std::shared_ptr<WebClientSession> session;
                {
                    std::lock_guard<std::mutex> lock(peer->state_lock);
                    session = peer->session;
                }
                if (!session || !session->active.load(std::memory_order_acquire)) return;

                if (message->binary) {
                    if (session->media_ready.load(std::memory_order_acquire))
                        session->incoming_rb.write(message->str.data(), message->str.size());
                    return;
                }

                try {
                    const auto response = nlohmann::json::parse(message->str);
                    const std::string type = response.value("type", std::string());
                    if (type == "identity_accepted") {
                        session->media_ready.store(true, std::memory_order_release);
                        // claim_identity() requested a graph rebuild before the
                        // remote handshake completed. Publish again now that the
                        // session has real audio buffers and can bind DSP handlers.
                        m_dspEngine.mark_graph_dirty();
                        peer->connected.store(true, std::memory_order_release);
                        peer->successful_connections.fetch_add(1, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lock(peer->state_lock);
                            peer->identity_accepted = true;
                            peer->last_error.clear();
                        }
                        peer->state_changed.notify_all();
                    } else if (type == "identity_conflict") {
                        {
                            std::lock_guard<std::mutex> lock(peer->state_lock);
                            peer->last_error = "Remote CM5Audio rejected this peer identity";
                            peer->connection_closed = true;
                        }
                        peer->state_changed.notify_all();
                        socket->close();
                    }
                } catch (...) {
                    // Ignore unrelated or malformed text messages. Audio remains
                    // isolated in the binary-message path.
                }
                return;
            }

            if (message->type == ix::WebSocketMessageType::Close ||
                message->type == ix::WebSocketMessageType::Error) {
                {
                    std::lock_guard<std::mutex> lock(peer->state_lock);
                    peer->connection_closed = true;
                    peer->connected.store(false, std::memory_order_release);
                    if (peer->last_error.empty())
                        peer->last_error = message->type == ix::WebSocketMessageType::Error
                            ? "WebSocket transport error" : "Remote peer disconnected";
                }
                peer->state_changed.notify_all();
            }
        });

        try {
            socket->start();
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(peer->state_lock);
            peer->last_error = error.what();
            peer->connection_closed = true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(peer->state_lock);
            peer->last_error = "Unable to start WebSocket transport";
            peer->connection_closed = true;
        }
        peer->state_changed.notify_all();
        const auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        {
            std::unique_lock<std::mutex> lock(peer->state_lock);
            while (!peer->connection_closed &&
                   !peer->stop_requested.load(std::memory_order_acquire) &&
                   m_running.load(std::memory_order_acquire)) {
                if (!peer->identity_accepted &&
                    std::chrono::steady_clock::now() >= handshakeDeadline) {
                    peer->last_error = "Connection or identity handshake timed out";
                    peer->connection_closed = true;
                    break;
                }
                peer->state_changed.wait_for(lock, std::chrono::milliseconds(200));
            }
        }

        bool wasAccepted = false;
        std::string disconnectReason;
        std::shared_ptr<WebClientSession> session;
        {
            std::lock_guard<std::mutex> lock(peer->state_lock);
            wasAccepted = peer->identity_accepted;
            disconnectReason = peer->last_error;
        }
        socket->stop();
        {
            std::lock_guard<std::mutex> lock(peer->state_lock);
            session = peer->session;
            peer->session.reset();
            peer->socket.reset();
        }
        if (session) m_clients.remove_session(sessionId);
        set_peer_disconnected(peer, disconnectReason.empty()
            ? "Disconnected; retrying" : disconnectReason);

        if (peer->stop_requested.load(std::memory_order_acquire) ||
            !m_running.load(std::memory_order_acquire))
            break;

        // Start the retry schedule fresh after any established connection.
        if (wasAccepted) retrySeconds = 1;
        // Exponential retry avoids hammering an unavailable host while still
        // restoring the peer automatically. The wait is interruptible by stop().
        {
            std::unique_lock<std::mutex> lock(peer->state_lock);
            peer->state_changed.wait_for(lock, std::chrono::seconds(retrySeconds), [&] {
                return peer->stop_requested.load(std::memory_order_acquire) ||
                       !m_running.load(std::memory_order_acquire);
            });
        }
        retrySeconds = std::min(retrySeconds * 2u, 30u);
    }

    peer->connected.store(false, std::memory_order_release);
}

bool MeshNetworkManager::start() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleLock);
    if (m_running.load(std::memory_order_acquire)) return true;
    if (!load_settings())
        std::cerr << "[Mesh] Mesh settings were malformed; valid peers will still be loaded where possible"
                  << std::endl;

    std::vector<std::shared_ptr<Peer>> configuredPeers;
    std::vector<IncomingPeer> incomingPeers;
    {
        std::lock_guard<std::mutex> lock(m_peersLock);
        configuredPeers = m_peers;
        incomingPeers = m_incomingPeers;
    }
    // Publish offline placeholders before binding the optional management port.
    // A busy UI port must not make saved peer nodes disappear from the patchbay.
    for (const auto& peer : configuredPeers)
        if (peer) set_peer_disconnected(peer, "Waiting for remote CM5Audio");
    for (const auto& peer : incomingPeers)
        restore_incoming_identity(peer);

    try {
        // Mesh management is intentionally plain HTTP on the trusted LAN.
        // Peer audio connections likewise use ws:// only; mesh has no TLS mode.
        m_httpServer = std::make_unique<ReliableHttpServer<httplib::Server>>();
        m_httpServer->set_read_timeout(2);
        m_httpServer->set_write_timeout(2);
        m_httpServer->set_payload_max_length(64 * 1024);
        const auto setCors = [](httplib::Response& response) {
            response.set_header("Access-Control-Allow-Origin", "*");
            response.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            response.set_header("Access-Control-Allow-Headers", "Content-Type");
        };
        m_httpServer->Options(R"(.*)", [setCors](const httplib::Request&, httplib::Response& response) {
            setCors(response);
            response.status = 204;
        });

        m_httpServer->Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(MeshNetworkManager::html_page(), "text/html; charset=utf-8");
        });
        m_httpServer->Get("/api/peers", [this, setCors](const httplib::Request&, httplib::Response& response) {
            setCors(response);
            nlohmann::json result = nlohmann::json::array();
            std::vector<std::shared_ptr<Peer>> peers;
            {
                std::lock_guard<std::mutex> lock(m_peersLock);
                peers = m_peers;
            }
            const std::string ownPeerPrefix = "cm5mesh-" + m_instanceId + "-";
            std::unordered_set<std::string> configuredKeys;
            for (const auto& peer : peers) {
                if (!peer) continue;
                const std::string key = mesh_key(m_instanceId, peer->id);
                configuredKeys.insert(key);
                std::string lastError;
                {
                    std::lock_guard<std::mutex> lock(peer->state_lock);
                    lastError = peer->last_error;
                }
                result.push_back({
                    {"id", peer->id}, {"name", peer->name}, {"host", peer->host},
                    {"port", peer->port}, {"direction", "outgoing"},
                    {"removable", true},
                    {"send_channels", peer->send_channels},
                    {"receive_channels", peer->receive_channels},
                    {"connected", peer->connected.load(std::memory_order_acquire)},
                    {"successful_connections", peer->successful_connections.load(std::memory_order_relaxed)},
                    {"last_error", lastError},
                    {"source_endpoint", "client/" + key + "/capture"},
                    {"sink_endpoint", "client/" + key + "/playback"}
                });
            }

            std::vector<IncomingPeer> incomingPeers;
            {
                std::lock_guard<std::mutex> lock(m_peersLock);
                incomingPeers = m_incomingPeers;
            }
            for (const auto& peer : incomingPeers) {
                if (peer.key.empty() || peer.key.rfind(ownPeerPrefix, 0) == 0 ||
                    configuredKeys.count(peer.key)) continue;
                result.push_back({
                    {"id", "incoming-" + peer.key}, {"name", peer.name}, {"host", peer.host},
                    {"port", 0}, {"direction", "incoming"},
                    {"removable", false}, {"send_channels", peer.output_channels},
                    {"receive_channels", peer.input_channels}, {"connected", peer.connected},
                    {"successful_connections", peer.connected ? 1 : 0},
                    {"last_error", peer.connected ? std::string() : "Incoming CM5Audio peer is disconnected"},
                    {"source_endpoint", "client/" + peer.key + "/capture"},
                    {"sink_endpoint", "client/" + peer.key + "/playback"}
                });
            }
            response.set_content(result.dump(2), "application/json");
        });
        m_httpServer->Get("/api/health", [this, setCors](const httplib::Request&, httplib::Response& response) {
            setCors(response);
            response.set_content(nlohmann::json({{"status", m_running.load() ? "ok" : "stopping"}}).dump(),
                                 "application/json");
        });
        m_httpServer->Post("/api/peers", [this, setCors](const httplib::Request& request, httplib::Response& response) {
            setCors(response);
            try {
                const auto body = nlohmann::json::parse(request.body);
                if (!body.is_object()) throw std::invalid_argument("Request body must be a JSON object");
                uint32_t id = 0;
                std::string error;
                const bool ok = add_peer(
                    body.value("name", std::string()),
                    body.value("host", std::string()),
                    body.value("port", 8183u),
                    body.value("send_channels", 2u),
                    body.value("receive_channels", 2u), id, error);
                if (!ok) {
                    response.status = 400;
                    response.set_content(error_json(error).dump(), "application/json");
                    return;
                }
                response.set_content(nlohmann::json({{"status", "ok"}, {"id", id}}).dump(), "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content(error_json(error.what()).dump(), "application/json");
            }
        });
        m_httpServer->Delete(R"(/api/peers/(\d+))", [this, setCors](const httplib::Request& request, httplib::Response& response) {
            setCors(response);
            try {
                if (request.matches.size() < 2) throw std::invalid_argument("Invalid peer ID");
                const unsigned long raw = std::stoul(request.matches[1]);
                if (raw == 0 || raw > std::numeric_limits<uint32_t>::max())
                    throw std::invalid_argument("Invalid peer ID");
                std::string error;
                if (!remove_peer(static_cast<uint32_t>(raw), error)) {
                    response.status = 404;
                    response.set_content(error_json(error).dump(), "application/json");
                    return;
                }
                response.set_content(nlohmann::json({{"status", "ok"}}).dump(), "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content(error_json(error.what()).dump(), "application/json");
            }
        });

        const char* bindEnv = std::getenv("CM5AUDIO_BIND_ADDRESS");
        const std::string bindAddress = bindEnv && *bindEnv ? bindEnv : "0.0.0.0";
        if (m_controlPort == 0) m_controlPort = m_httpServer->bind_to_any_port(bindAddress);
        else if (!m_httpServer->bind_to_port(bindAddress, m_controlPort))
            throw std::runtime_error("mesh control port bind failed");
        if (m_controlPort <= 0) throw std::runtime_error("mesh control port bind failed");

        m_running.store(true, std::memory_order_release);
        m_httpThread = std::thread([this] {
            if (!m_httpServer->listen_after_bind())
                m_running.store(false, std::memory_order_release);
        });
        m_httpServer->wait_until_ready();
        m_monitorThread = std::thread(&MeshNetworkManager::monitor_loop, this);
        for (const auto& peer : configuredPeers)
            if (peer) start_peer_worker(peer);

        std::cout << "[Mesh] Peer manager listening on http://" << bindAddress
                  << ":" << m_controlPort << std::endl;
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[Mesh] Startup failed: " << error.what() << std::endl;
        m_running.store(false, std::memory_order_release);
        for (const auto& peer : configuredPeers) {
            if (!peer) continue;
            peer->stop_requested.store(true, std::memory_order_release);
            peer->state_changed.notify_all();
        }
        for (const auto& peer : configuredPeers)
            if (peer && peer->worker.joinable()) peer->worker.join();
        if (m_monitorThread.joinable()) m_monitorThread.join();
        if (m_httpServer) m_httpServer->stop();
        if (m_httpThread.joinable()) m_httpThread.join();
        m_httpServer.reset();
        return false;
    }
}

void MeshNetworkManager::stop() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleLock);
    m_running.store(false, std::memory_order_release);
    if (m_httpServer) m_httpServer->stop();
    if (m_httpThread.joinable()) m_httpThread.join();

    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(m_peersLock);
        peers = m_peers;
    }
    for (const auto& peer : peers) {
        if (!peer) continue;
        peer->stop_requested.store(true, std::memory_order_release);
        peer->state_changed.notify_all();
    }
    for (const auto& peer : peers) {
        if (peer && peer->worker.joinable()) peer->worker.join();
    }
    if (m_monitorThread.joinable()) m_monitorThread.join();
    m_httpServer.reset();
}
