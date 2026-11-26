#include "device_manager.h"
#include "dsp_engine.h"
#include "httplib.h"
#include "mesh_network.h"
#include "web_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool require(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "[mesh_network_test] FAIL: " << message << std::endl;
    return false;
}

unsigned unused_loopback_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return 0;
    }
    const unsigned port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

} // namespace

int main() {
    char temporaryDirectory[] = "/tmp/cm5audio-mesh-test-XXXXXX";
    if (!::mkdtemp(temporaryDirectory)) {
        std::cerr << "[mesh_network_test] FAIL: mkdtemp failed" << std::endl;
        return 1;
    }

    char originalDirectory[4096]{};
    if (!::getcwd(originalDirectory, sizeof(originalDirectory)) ||
        ::chdir(temporaryDirectory) != 0) {
        std::cerr << "[mesh_network_test] FAIL: cannot enter temporary directory" << std::endl;
        return 1;
    }

    const char* previousBind = std::getenv("CM5AUDIO_BIND_ADDRESS");
    const std::string savedBind = previousBind ? previousBind : "";
    const bool hadBind = previousBind != nullptr;
    ::setenv("CM5AUDIO_BIND_ADDRESS", "127.0.0.1", 1);

    bool ok = true;
    {
        DeviceManager devices(false, false);
        AudioMetrics metrics;
        ToneControls tone;
        ClientManager clients;
        clients.set_device_manager(&devices);
        DspEngine engine(devices, clients, metrics, tone);
        devices.set_dsp_engine(&engine);
        clients.set_dsp_engine(&engine);

        MeshNetworkManager mesh(clients, engine, 0);
        ok &= require(mesh.start(), "mesh manager starts on an ephemeral loopback port");
        if (ok) {
            httplib::Client http("127.0.0.1", mesh.control_port());
            auto health = http.Get("/api/health");
            ok &= require(health && health->status == 200,
                          "mesh management health endpoint responds");

            const unsigned unavailablePort = unused_loopback_port();
            ok &= require(unavailablePort != 0, "allocated a test destination port");
            if (unavailablePort != 0) {
                nlohmann::json request = {
                    {"name", "Unreachable Test Peer"},
                    {"host", "127.0.0.1"},
                    {"port", unavailablePort},
                    {"send_channels", 2},
                    {"receive_channels", 2}
                };
                auto created = http.Post("/api/peers", request.dump(), "application/json");
                ok &= require(created && created->status == 200,
                              "an unreachable peer can be configured without blocking the API");

                auto listed = http.Get("/api/peers");
                ok &= require(listed && listed->status == 200,
                              "configured peer remains discoverable");
                std::string key;
                if (listed && listed->status == 200) {
                    const auto peers = nlohmann::json::parse(listed->body);
                    ok &= require(peers.is_array() && peers.size() == 1,
                                  "exactly one outgoing peer is listed");
                    if (peers.is_array() && !peers.empty()) {
                        ok &= require(!peers[0].value("connected", true),
                                      "unreachable peer is reported disconnected");
                    }
                }

                const auto known = clients.known_client_names();
                for (const auto& entry : known) {
                    if (entry.first.rfind("cm5mesh-", 0) == 0) {
                        key = entry.first;
                        break;
                    }
                }
                ok &= require(!key.empty(),
                              "unreachable peer still registers a persistent client identity");
                if (!key.empty()) {
                    const std::string source = "client/" + key + "/capture";
                    const std::string sink = "client/" + key + "/playback";
                    const NodeId sourceNode = clients.get_node_id(source);
                    const NodeId sinkNode = clients.get_node_id(sink);
                    ok &= require(sourceNode != NODE_INVALID && sinkNode != NODE_INVALID,
                                  "both source and sink endpoints exist while disconnected");

                    auto removed = http.Delete("/api/peers/1");
                    ok &= require(removed && removed->status == 200,
                                  "peer reconnection can be disabled without deleting endpoints");
                    ok &= require(clients.get_node_id(source) == sourceNode &&
                                      clients.get_node_id(sink) == sinkNode,
                                  "source and sink endpoint identities survive peer removal");
                }
            }
            mesh.stop();
        }
        clients.set_dsp_engine(nullptr);
        devices.set_dsp_engine(nullptr);
        clients.set_device_manager(nullptr);
    }

    if (hadBind) ::setenv("CM5AUDIO_BIND_ADDRESS", savedBind.c_str(), 1);
    else ::unsetenv("CM5AUDIO_BIND_ADDRESS");
    ::chdir(originalDirectory);
    std::error_code ignored;
    std::filesystem::remove_all(temporaryDirectory, ignored);

    if (!ok) return 1;
    std::cout << "[mesh_network_test] PASS" << std::endl;
    return 0;
}
