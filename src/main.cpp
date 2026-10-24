#include "web_server.h"
#include "device_manager.h"
#include "dsp_engine.h"
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace {
volatile sig_atomic_t shutdownRequested = 0;
volatile sig_atomic_t signalWriteFd = -1;

void signal_handler(int) {
    const int savedErrno = errno;
    shutdownRequested = 1;
    const unsigned char byte = 1;
    if (signalWriteFd >= 0) {
        const auto ignored = ::write(static_cast<int>(signalWriteFd), &byte, 1);
        (void)ignored; // Nonblocking self-pipe; the flag also covers a full pipe.
    }
    errno = savedErrno;
}

class ShutdownNotification {
public:
    ShutdownNotification() {
        if (::pipe2(m_pipe, O_NONBLOCK | O_CLOEXEC) != 0)
            throw std::runtime_error("Cannot create shutdown notification pipe");
        signalWriteFd = m_pipe[1];
        struct sigaction action {};
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0)
            throw std::runtime_error("Cannot install shutdown signal handlers");
        // Peer disconnect must be a transport error, not process termination.
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ::sigaction(SIGPIPE, &ignore, nullptr);
    }
    ~ShutdownNotification() {
        signalWriteFd = -1;
        ::close(m_pipe[0]);
        ::close(m_pipe[1]);
    }
    void wait() const {
        struct pollfd descriptor {m_pipe[0], POLLIN, 0};
        ::poll(&descriptor, 1, 100);
    }
private:
    int m_pipe[2]{-1, -1};
};

int port_from_env(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    size_t used = 0;
    const std::string text(value);
    const int port = std::stoi(text, &used);
    if (used != text.size() || port < 0 || port > 65535)
        throw std::invalid_argument(std::string("Invalid port: ") + name);
    return port;
}

void print_usage(const char* programName) {
    std::cout << "Usage: " << programName << " [options]\n\n"
              << "Options:\n"
              << "  -h, --help      Display this help message and exit\n"
              << "  --plain-text    Run the web server in plain text mode\n"
              << "  --no-audio      Bypass audio device manager and context initialization\n"
              << "  --enable-pipewire-patchbay  Enable the PipeWire patchbay viewport\n\n"
              << "Environment Variables:\n"
              << "  CM5AUDIO_HTTP_PORT  HTTP server port (default: 8182)\n"
              << "  CM5AUDIO_WS_PORT    WebSocket server port (default: 8183)\n";
}
}

int main(int argc, char** argv) {
    try {
        bool plainText = false, noAudio = false, enablePwPatchbay = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument(argv[i]);
            if (argument == "--help" || argument == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            else if (argument == "--plain-text") plainText = true;
            else if (argument == "--no-audio") noAudio = true;
            else if (argument == "--enable-pipewire-patchbay") enablePwPatchbay = true;
            else throw std::invalid_argument("Unknown argument: " + argument);
        }

        // Install handlers before any backend constructor can create threads.
        ShutdownNotification shutdown;
        const int httpPort = port_from_env("CM5AUDIO_HTTP_PORT", 8182);
        const int wsPort = port_from_env("CM5AUDIO_WS_PORT", 8183);
        std::cout << "Starting cm5audio..." << std::endl;
        DeviceManager devices(!noAudio, enablePwPatchbay);
        if (shutdownRequested) return 0;
        if (!noAudio) devices.enumerate_devices();
        if (shutdownRequested) return 0;
        AudioMetrics metrics;
        AudioControls controls;
        ToneControls tone;
        ClientManager clients;
        clients.set_device_manager(&devices);
        auto engine = std::make_unique<DspEngine>(devices, clients, metrics, tone);
        clients.set_dsp_engine(engine.get());
        clients.load_settings(controls, tone);
        if (shutdownRequested) { clients.set_dsp_engine(nullptr); return 0; }
        engine->start();
        WebServer server(metrics, controls, tone, clients, devices, *engine, httpPort, wsPort, plainText, enablePwPatchbay);
        if (!server.start()) { clients.set_dsp_engine(nullptr); return 1; }
        while (!shutdownRequested && server.is_running()) shutdown.wait();
        std::cout << "[Shutdown] Quiescing control, DSP, network and sessions" << std::endl;
        server.stop();
        clients.set_dsp_engine(nullptr);
        engine.reset(); // Release graph owners before hardware/context teardown.
        devices.stop_all();
        std::cout << "[Shutdown] Application workers stopped; releasing audio context" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "cm5audio: " << error.what() << std::endl;
        return 1;
    }
}
