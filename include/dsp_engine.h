#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#include "device_manager.h"
#include "web_server.h"
#include "constants.h"
#include "processor_registry.h"
#include "plugins/gain.h"
#include "plugins/compressor.h"
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <set>
#include <functional>
#include <utility>

class ClientManager;

struct NodeHandler {
    enum class Type { HARDWARE_CAPTURE, HARDWARE_PLAYBACK, CLIENT_CAPTURE, CLIENT_PLAYBACK, TONE, VIRTUAL };
    Type type;
    void* ptr;
    unsigned channels;
    std::string endpointId;
    bool active = false;
};

class DspEngine {
public:
    DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics, ToneControls& tone);
    ~DspEngine();

    void start();
    void stop();
    bool is_running() const { return m_running.load(std::memory_order_acquire); }

    // Publish state before requesting adoption. A revision is acknowledged only
    // after a complete audio block has finished using the adopted state.
    uint64_t mark_graph_dirty();
    uint64_t request_adoption() { return mark_graph_dirty(); }
    uint64_t requested_revision() const { return m_requestedRevision.load(std::memory_order_acquire); }
    uint64_t adopted_revision() const { return m_adoptedRevision.load(std::memory_order_acquire); }
    bool wait_for_revision(uint64_t revision, std::chrono::milliseconds timeout);

    // Control-thread only. Final device/session destruction must never run on
    // the audio worker; stop() also drains all retained endpoint ownership.
    void collect_retired_endpoints();

    GainProcessor* get_gain_processor() { return m_gainProc.get(); }
    CompressorProcessor* get_compressor() { return m_compProc.get(); }
    bool supports_route_dsp(uint32_t routeId) const { return routeId > 0 && routeId < MAX_ROUTES; }

    // Construction, publication and reclamation of processors are control-side.
    // Chain, gain and bypass state are one indivisible audio-side publication.
    void update_route_dsp(uint32_t routeId, std::vector<std::unique_ptr<AudioProcessor>> newChain,
                          float gain, bool active);

    // Publicly exposed so DeviceManager native stream monitors can feed meter data
    void update_meters(float* buffer, unsigned channels, unsigned stride, const std::string& endpointId, bool isCapture);

private:
    friend struct DspEngineTestAccess;

    void processing_loop();
    void process_audio();
    void rebuild_graph();

    DeviceManager& m_deviceManager;
    ClientManager& m_clientManager;
    AudioMetrics& m_metrics;
    ToneControls& m_tone;
    std::thread m_workerThread;
    std::atomic<bool> m_running{false};

    static constexpr size_t MAX_NODES = 256;
    static constexpr size_t MAX_ROUTES = 1024;

    // Fixed-capacity backing storage is allocated before DSP starts (~48 MiB
    // for all three banks). Each node uses only its negotiated variable stride;
    // no endpoint change reallocates these buffers while processing.
    std::vector<std::vector<float>> m_nodeBuffers;
    std::vector<std::vector<float>> m_feedbackBuffers;
    std::vector<std::vector<float>> m_nextFeedbackBuffers;
    std::vector<unsigned> m_nodeWidths;
    std::vector<NodeId> m_executionOrder;
    int m_nodePosition[MAX_NODES];
    NodeHandler m_nodeHandlers[MAX_NODES];
    std::atomic<bool> m_graphDirty{true};
    std::atomic<uint64_t> m_requestedRevision{1};
    std::atomic<uint64_t> m_adoptedRevision{0};
    uint64_t m_builtRevision = 0; // Audio-thread only (or after join).
    std::mutex m_revisionWaitLock;
    std::condition_variable m_revisionChanged;

    struct EndpointOwners {
        std::vector<std::shared_ptr<HardwareDevice>> hardware;
        std::vector<std::shared_ptr<WebClientSession>> sessions;
        bool empty() const { return hardware.empty() && sessions.empty(); }
    };
    EndpointOwners m_endpointOwners;
    std::mutex m_retiredEndpointsLock;
    std::vector<EndpointOwners> m_retiredEndpointOwners;

    // Installed only by the friend test accessor while the worker is stopped.
    // Runs after taking graph snapshots, before publishing the rebuilt graph.
    std::function<void()> m_rebuildGraphTestHook;

    std::shared_ptr<const std::vector<AudioRoute>> m_currentRoutes;
    std::vector<const AudioRoute*> m_adjList[MAX_NODES];

    struct RouteDsp {
        struct Chain {
            std::unique_ptr<AudioProcessor> processors[CM5_MAX_PROCESSORS_PER_ROUTE];
            int length = 0;
            float gain = 1.0f;
            bool active = false;
        };
        // Three slots guarantee a writable slot even while the audio reader holds an
        // older publication. Only the control thread creates/destroys processors.
        Chain chains[3];
        std::atomic<int> current_idx{0};
        std::atomic<int> reader_idx{-1};
        std::mutex writer_lock;
    };
    RouteDsp m_routeDsp[MAX_ROUTES];
    std::vector<float> m_routeScratchBuffer;

    std::unique_ptr<GainProcessor> m_gainProc;
    std::unique_ptr<CompressorProcessor> m_compProc;

    ma_uint32 m_engineSampleRate = 48000;
    double m_tonePhase = 0.0;

    inline float* get_buffer(NodeId id) {
        if (id < 0 || static_cast<size_t>(id) >= MAX_NODES) return nullptr;
        return m_nodeBuffers[static_cast<size_t>(id)].empty() ? nullptr : m_nodeBuffers[static_cast<size_t>(id)].data();
    }
    inline float* get_feedback_buffer(NodeId id) {
        if (id < 0 || static_cast<size_t>(id) >= MAX_NODES) return nullptr;
        return m_feedbackBuffers[static_cast<size_t>(id)].empty() ? nullptr : m_feedbackBuffers[static_cast<size_t>(id)].data();
    }
    inline float* get_next_feedback_buffer(NodeId id) {
        if (id < 0 || static_cast<size_t>(id) >= MAX_NODES) return nullptr;
        return m_nextFeedbackBuffers[static_cast<size_t>(id)].empty() ? nullptr : m_nextFeedbackBuffers[static_cast<size_t>(id)].data();
    }
};

#endif
