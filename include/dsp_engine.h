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
#include <cstddef>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <set>
#include <functional>
#include <utility>
#include <cmath>
#include <nlohmann/json.hpp>

class ClientManager;

struct NodeHandler {
    enum class Type { HARDWARE_CAPTURE, HARDWARE_PLAYBACK, CLIENT_CAPTURE, CLIENT_PLAYBACK,
                       PLUGIN_INPUT, PLUGIN_OUTPUT, PLUGIN_PROCESS, TONE, VIRTUAL };
    Type type;
    void* ptr;
    unsigned channels;
    std::string endpointId;
    bool active = false;
    const PluginNodeInstance* plugin = nullptr;
    size_t port_index = 0;
};

class DspEngine {
public:
    DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics, ToneControls& tone);
    ~DspEngine();

    void start();
    void stop();
    bool is_running() const { return m_running.load(std::memory_order_acquire); }

    uint64_t mark_graph_dirty();
    uint64_t request_adoption() { return mark_graph_dirty(); }
    uint64_t requested_revision() const { return m_requestedRevision.load(std::memory_order_acquire); }
    uint64_t adopted_revision() const { return m_adoptedRevision.load(std::memory_order_acquire); }
    bool wait_for_revision(uint64_t revision, std::chrono::milliseconds timeout);
    void collect_retired_endpoints();

    GainProcessor* get_gain_processor() { return m_gainProc.get(); }
    CompressorProcessor* get_compressor() { return m_compProc.get(); }
    bool supports_route_dsp(uint32_t routeId) const { return routeId > 0 && routeId < MAX_ROUTES; }

    void update_route_dsp(uint32_t routeId, std::vector<std::unique_ptr<AudioProcessor>> newChain,
                          float gain, bool active);

    void register_pipewire_meter_bank(const std::string& endpointId,
                                      unsigned channels, bool isCapture);
    void unregister_pipewire_meter_bank(const std::string& endpointId, bool isCapture);
    void update_meters(float* buffer, unsigned channels, unsigned stride,
                       const std::string& endpointId, bool isCapture,
                       size_t frames = CM5_BUFFER_FRAMES);

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
    uint64_t m_builtRevision = 0;
    std::mutex m_revisionWaitLock;
    std::condition_variable m_revisionChanged;

    struct EndpointOwners {
        std::vector<std::shared_ptr<HardwareDevice>> hardware;
        std::vector<std::shared_ptr<WebClientSession>> sessions;
        std::shared_ptr<const std::vector<std::shared_ptr<const PluginNodeInstance>>> plugins;
        std::shared_ptr<const std::vector<MidiRoute>> midiRoutes;
        bool empty() const {
            return hardware.empty() && sessions.empty() && (!plugins || plugins->empty()) &&
                   (!midiRoutes || midiRoutes->empty());
        }
    };
    EndpointOwners m_endpointOwners;
    std::mutex m_retiredEndpointsLock;
    std::vector<EndpointOwners> m_retiredEndpointOwners;

    std::function<void()> m_rebuildGraphTestHook;

    std::shared_ptr<const std::vector<AudioRoute>> m_currentRoutes;
    std::shared_ptr<const std::vector<MidiRoute>> m_currentMidiRoutes;
    std::vector<const AudioRoute*> m_adjList[MAX_NODES];

    struct MidiDispatch {
        NodeId process_node = NODE_INVALID;
        const PluginPortInstance* source_port = nullptr;
        std::vector<std::shared_ptr<MidiEventQueue>> destinations;
    };
    std::vector<MidiDispatch> m_midiDispatches;

    struct RouteDsp {
        struct Chain {
            std::unique_ptr<AudioProcessor> processors[CM5_MAX_PROCESSORS_PER_ROUTE];
            int length = 0;
            float gain = 1.0f;
            bool active = false;
        };
        Chain chains[3];
        std::atomic<int> current_idx{0};
        std::atomic<int> reader_idx{-1};
        std::mutex writer_lock;
    };

public:
    // Read-only route-chain discovery. The writer lock pins the published
    // processor instances against a concurrent chain replacement. Parameter
    // reads themselves are atomic and do not touch the audio callback.
    nlohmann::json route_dsp_parameter_targets(uint32_t routeId) {
        nlohmann::json result = nlohmann::json::array();
        if (!supports_route_dsp(routeId)) return result;
        RouteDsp& route = m_routeDsp[routeId];
        std::lock_guard<std::mutex> writer(route.writer_lock);
        const int index = route.current_idx.load(std::memory_order_acquire);
        const auto& chain = route.chains[index];
        for (int stage = 0; stage < chain.length; ++stage) {
            const AudioProcessor* processor = chain.processors[stage].get();
            if (!processor) continue;
            const char* type = processor->parameter_control_type();
            if (!type || !*type) continue;
            const ProcessorDescriptor* descriptor = ProcessorRegistry::instance().find(type);
            if (!descriptor) continue;
            for (const auto& parameter : descriptor->parameters) {
                if ((parameter.type != "number" && parameter.type != "integer") ||
                    !parameter.min || !parameter.max ||
                    !processor->supports_parameter_control(parameter.id))
                    continue;
                float current = 0.0f;
                if (!processor->get_parameter_control(parameter.id, current)) continue;
                result.push_back({
                    {"stage_index", stage}, {"plugin_type", type},
                    {"plugin_name", descriptor->name},
                    {"parameter_id", parameter.id}, {"name", parameter.name},
                    {"min", *parameter.min}, {"max", *parameter.max},
                    {"step", parameter.step.value_or(0.0)},
                    {"unit", parameter.unit}, {"integer", parameter.type == "integer"},
                    {"current", current}
                });
            }
        }
        return result;
    }

    // MIDI updates target only parameters explicitly declared realtime-safe by
    // their processor. They update atomics in-place and never rebuild the route.
    bool set_route_dsp_parameter(uint32_t routeId, unsigned stageIndex,
                                 const std::string& parameterId, float value,
                                 const std::string& expectedPluginType = {}) {
        if (!supports_route_dsp(routeId) || !std::isfinite(value)) return false;
        RouteDsp& route = m_routeDsp[routeId];
        std::lock_guard<std::mutex> writer(route.writer_lock);
        const int index = route.current_idx.load(std::memory_order_acquire);
        auto& chain = route.chains[index];
        if (stageIndex >= static_cast<unsigned>(chain.length)) return false;
        AudioProcessor* processor = chain.processors[stageIndex].get();
        if (!processor) return false;
        const char* actualType = processor->parameter_control_type();
        if (!expectedPluginType.empty() &&
            (!actualType || expectedPluginType != actualType))
            return false;
        return processor->set_parameter_control(parameterId, value);
    }

private:
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
