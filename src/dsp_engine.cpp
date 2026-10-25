#include "dsp_engine.h"
#include "web_server.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <string>
#include <queue>
#include <cstring>

#define CM5_ENABLE_PROBES 0

#if CM5_ENABLE_PROBES
static std::atomic<uint32_t> g_processCount{0};
#define SAMPLED_TRACE(msg, val) \
    if (++g_processCount % 100 == 0) { std::cout << "[TRACE] " << msg << " | Value: " << val << std::endl; }
#else
#define SAMPLED_TRACE(msg, val)
#endif

DspEngine::DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics, ToneControls& tone)
    : m_deviceManager(deviceManager), m_clientManager(clientManager), m_metrics(metrics), m_tone(tone) {
    static_assert(CM5_MAX_CHANNELS <= MA_MAX_CHANNELS, "Application width exceeds miniaudio");
    // Explicit bounded graph storage: 3 * 256 * 64 * 254 floats (about 48 MiB).
    // Allocate before starting DSP. Per-node strides and processing widths remain
    // variable; adding/reopening an endpoint never reallocates audio buffers.
    const size_t samples = static_cast<size_t>(CM5_BUFFER_FRAMES) * CM5_MAX_CHANNELS;
    m_nodeBuffers.assign(MAX_NODES, std::vector<float>(samples, 0.0f));
    m_feedbackBuffers.assign(MAX_NODES, std::vector<float>(samples, 0.0f));
    m_nextFeedbackBuffers.assign(MAX_NODES, std::vector<float>(samples, 0.0f));
    m_nodeWidths.resize(MAX_NODES, 0);
    m_routeScratchBuffer.resize(CM5_BUFFER_FRAMES, 0.0f);
    m_gainProc = std::make_unique<GainProcessor>(1.0f);
    m_compProc = std::make_unique<CompressorProcessor>(0.7f, 4.0f);
}

DspEngine::~DspEngine() {
    stop();
    // DeviceManager can outlive the engine. Tear down its PipeWire callbacks
    // while this object is still valid, including exception/unusual shutdown paths.
    m_deviceManager.set_dsp_engine(nullptr);
}

void DspEngine::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    // stop() discarded graph ownership; a restart must bind fresh endpoints.
    m_graphDirty.store(true, std::memory_order_release);
    try {
        m_workerThread = std::thread(&DspEngine::processing_loop, this);
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        m_revisionChanged.notify_all();
        throw;
    }
}

void DspEngine::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_workerThread.joinable()) m_workerThread.join();
    // No block can still dereference a handler. Final endpoint destruction runs
    // on this control thread, never on the joined audio worker.
    for (auto& handler : m_nodeHandlers) handler = {};
    m_executionOrder.clear();
    for (auto& routes : m_adjList) routes.clear();
    m_currentRoutes.reset();
    m_endpointOwners = {};
    collect_retired_endpoints();
    m_builtRevision = 0;
    m_graphDirty.store(true, std::memory_order_release);
    m_revisionChanged.notify_all();
}

uint64_t DspEngine::mark_graph_dirty() {
    // State publication precedes this call. Set dirty first, then release the
    // revision. If a block consumes dirty before seeing the new revision, its
    // next block still rebuilds because the built revision does not match.
    m_graphDirty.store(true, std::memory_order_release);
    return m_requestedRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool DspEngine::wait_for_revision(uint64_t revision, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> waitLock(m_revisionWaitLock);
    while (adopted_revision() < revision) {
        if (!is_running() || std::chrono::steady_clock::now() >= deadline) return false;
        // The audio notifier intentionally never takes a control mutex. Bound
        // waits also close the predicate/notification race without locking the
        // real-time thread (a notification can arrive just before wait begins).
        m_revisionChanged.wait_until(waitLock, std::min(deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
    }
    return true;
}

void DspEngine::collect_retired_endpoints() {
    std::vector<EndpointOwners> retired;
    {
        std::lock_guard<std::mutex> lock(m_retiredEndpointsLock);
        retired.swap(m_retiredEndpointOwners);
    }
    // Drop owners outside the queue lock: device stop/session sender join can
    // block, and must not hold up an audio-side graph rebuild.
}

void DspEngine::update_route_dsp(uint32_t routeId,
                                 std::vector<std::unique_ptr<AudioProcessor>> newChain,
                                 float gain, bool active) {
    if (!supports_route_dsp(routeId)) throw std::invalid_argument("Route ID exceeds DSP capacity");
    if (newChain.size() > CM5_MAX_PROCESSORS_PER_ROUTE) throw std::invalid_argument("DSP chain exceeds capacity");
    if (!std::isfinite(gain) || gain < 0.0f || gain > 8.0f)
        throw std::invalid_argument("DSP gain must be finite and between 0 and 8");
    RouteDsp& rdsp = m_routeDsp[routeId];
    std::lock_guard<std::mutex> writer(rdsp.writer_lock);
    // Sequentially consistent publication plus reader pinning prevents a fast
    // second update from destroying a state still used by the audio thread.
    // Three slots provide a spare without waiting or allocating on that reader.
    const int current = rdsp.current_idx.load();
    const int reader = rdsp.reader_idx.load();
    int write_idx = 0;
    while (write_idx == current || write_idx == reader) ++write_idx;
    auto& target = rdsp.chains[write_idx];
    for (auto& processor : target.processors) processor.reset();
    target.length = static_cast<int>(newChain.size());
    for (int i = 0; i < target.length; ++i) target.processors[i] = std::move(newChain[i]);
    target.gain = gain;
    target.active = active;
    rdsp.current_idx.store(write_idx);
}

void DspEngine::register_pipewire_meter_bank(
    const std::string& endpointId, unsigned channels, bool isCapture)
{
    if (endpointId.empty() || channels == 0 || channels > CM5_MAX_CHANNELS)
        return;
    std::lock_guard<std::mutex> metricsLock(m_metrics.lock);
    auto& banks = isCapture ? m_metrics.capture : m_metrics.playback;
    auto bank = banks.find(endpointId);
    if (bank == banks.end()) {
        banks.emplace(endpointId, std::vector<ChannelMeter>(channels));
    } else if (bank->second.size() != channels) {
        std::vector<ChannelMeter> prepared(channels);
        bank->second.swap(prepared);
    }
}

void DspEngine::unregister_pipewire_meter_bank(
    const std::string& endpointId, bool isCapture)
{
    std::lock_guard<std::mutex> metricsLock(m_metrics.lock);
    auto& banks = isCapture ? m_metrics.capture : m_metrics.playback;
    banks.erase(endpointId);
}

void DspEngine::update_meters(float* buffer, unsigned channels, unsigned stride,
                              const std::string& endpointId, bool isCapture,
                              size_t frames) {
    if (!buffer || channels == 0 || channels > CM5_MAX_CHANNELS ||
        stride < channels || frames == 0) return;

    std::lock_guard<std::mutex> metricsLock(m_metrics.lock);
    auto& banks = isCapture ? m_metrics.capture : m_metrics.playback;
    const auto bank = banks.find(endpointId);

    if (bank == banks.end() || bank->second.size() != channels) return;
    auto& targetMeters = bank->second;

    for (unsigned ch = 0; ch < channels; ++ch) {
        double sumSq = 0.0;
        float peak = 0.0f;
        for (size_t f = 0; f < frames; ++f) {
            const float sample = buffer[f * stride + ch];
            const float absS = std::abs(sample);
            sumSq += static_cast<double>(sample) * sample;
            if (absS > peak) peak = absS;
        }
        const float rms = static_cast<float>(std::sqrt(sumSq / frames));
        const float rmsDb = rms > 0.00001f ? 20.0f * std::log10(rms) : -60.0f;
        const float peakDb = peak > 0.00001f ? 20.0f * std::log10(peak) : -60.0f;
        targetMeters[ch].raw_value.store(rms, std::memory_order_relaxed);
        targetMeters[ch].raw_peak.store(peak, std::memory_order_relaxed);
        targetMeters[ch].rms_db.store(rmsDb, std::memory_order_relaxed);
        targetMeters[ch].peak_db.store(peakDb, std::memory_order_relaxed);
        targetMeters[ch].clipped.store(peak > 1.0f, std::memory_order_relaxed);
    }
}

void DspEngine::rebuild_graph() {
    const auto routes_ptr = m_clientManager.route_snapshot();
    EndpointOwners nextOwners;
    nextOwners.hardware = m_deviceManager.get_active_devices();
    nextOwners.sessions = m_clientManager.get_active_sessions();

    const auto validNode = [](NodeId node) {
        return node >= 0 && static_cast<size_t>(node) < MAX_NODES;
    };

    // Resolve actual widths before route sorting/validation. Real endpoint widths
    // are authoritative; only offline/virtual nodes infer a minimum from routes.
    std::fill(m_nodeWidths.begin(), m_nodeWidths.end(), 0);
    std::vector<bool> fixedWidth(MAX_NODES, false);
    std::vector<int> direction(MAX_NODES, 0); // 1 source, -1 destination, 0 offline/virtual.
    direction[NODE_TONE_GENERATOR] = 1;
    m_nodeWidths[NODE_TONE_GENERATOR] = 1;
    fixedWidth[NODE_TONE_GENERATOR] = true;
    for (const auto& dev : nextOwners.hardware) {
        const std::string endpoint = "hardware/" + dev->get_original_id() +
                                     (dev->get_info().is_capture ? "/capture" : "/playback");
        const NodeId id = m_clientManager.get_node_id(endpoint);
        if (validNode(id)) {
            m_nodeWidths[id] = std::min(dev->get_info().channels, CM5_MAX_CHANNELS);
            fixedWidth[id] = true;
            direction[id] = dev->get_info().is_capture ? 1 : -1;
        }
    }
    for (const auto& session : nextOwners.sessions) {
        if (!session->media_ready.load(std::memory_order_acquire)) continue;
        const std::string base = "client/" + session->get_client_key();
        const NodeId capture = m_clientManager.get_node_id(base + "/capture");
        const NodeId playback = m_clientManager.get_node_id(base + "/playback");
        if (validNode(capture)) {
            m_nodeWidths[capture] = std::min(session->input_channels.load(), CM5_MAX_CHANNELS);
            fixedWidth[capture] = true;
            direction[capture] = 1;
        }
        if (validNode(playback)) {
            m_nodeWidths[playback] = std::min(session->output_channels.load(), CM5_MAX_CHANNELS);
            fixedWidth[playback] = true;
            direction[playback] = -1;
        }
    }
    if (routes_ptr) for (const auto& route : *routes_ptr) {
        if (!route.enabled || !validNode(route.source_node) || !validNode(route.destination_node)) continue;
        if (!fixedWidth[route.source_node] && route.source_channel < CM5_MAX_CHANNELS)
            m_nodeWidths[route.source_node] = std::max(m_nodeWidths[route.source_node], route.source_channel + 1);
        if (!fixedWidth[route.destination_node] && route.destination_channel < CM5_MAX_CHANNELS)
            m_nodeWidths[route.destination_node] = std::max(m_nodeWidths[route.destination_node], route.destination_channel + 1);
    }

    const auto validRoute = [&](const AudioRoute& route) {
        if (!route.enabled || !validNode(route.source_node) || !validNode(route.destination_node)) return false;
        const unsigned sourceWidth = m_nodeWidths[route.source_node];
        const unsigned destinationWidth = m_nodeWidths[route.destination_node];
        return direction[route.source_node] != -1 && direction[route.destination_node] != 1 &&
               sourceWidth > 0 && destinationWidth > 0 &&
               route.source_channel < sourceWidth && route.destination_channel < destinationWidth;
    };
    std::set<NodeId> activeNodes;
    std::unordered_map<NodeId, std::vector<NodeId>> adj;
    std::unordered_map<NodeId, int> inDegree;
    if (routes_ptr) for (const auto& route : *routes_ptr) {
        // Disabled/invalid routes must not affect execution order or introduce
        // a cycle that prevents otherwise enabled routes from being processed.
        if (!validRoute(route)) continue;
        activeNodes.insert(route.source_node);
        activeNodes.insert(route.destination_node);
        adj[route.source_node].push_back(route.destination_node);
        inDegree[route.destination_node]++;
    }
    for (const auto& dev : nextOwners.hardware) {
        const std::string endpoint = "hardware/" + dev->get_original_id() +
                                     (dev->get_info().is_capture ? "/capture" : "/playback");
        const NodeId id = m_clientManager.get_node_id(endpoint);
        if (validNode(id) && m_nodeWidths[id] > 0) activeNodes.insert(id);
    }
    for (const auto& session : nextOwners.sessions) {
        if (!session->media_ready.load(std::memory_order_acquire)) continue;
        const std::string base = "client/" + session->get_client_key();
        const NodeId capture = m_clientManager.get_node_id(base + "/capture");
        const NodeId playback = m_clientManager.get_node_id(base + "/playback");
        if (validNode(capture) && m_nodeWidths[capture] > 0) activeNodes.insert(capture);
        if (validNode(playback) && m_nodeWidths[playback] > 0) activeNodes.insert(playback);
    }
    activeNodes.insert(NODE_TONE_GENERATOR);
    std::vector<NodeId> sortedNodes;
    std::priority_queue<NodeId, std::vector<NodeId>, std::greater<NodeId>> q;
    for (NodeId node : activeNodes) if (inDegree[node] == 0) q.push(node);
    while (!q.empty()) {
        const NodeId u = q.top(); q.pop();
        sortedNodes.push_back(u);
        for (NodeId v : adj[u]) if (--inDegree[v] == 0) q.push(v);
    }
    // Feedback/cycle design remains out of scope. Do not advertise reliable
    // cyclic routing; enabled cycles can still omit nodes from Kahn's order.

    if (m_rebuildGraphTestHook) m_rebuildGraphTestHook();
    m_executionOrder = std::move(sortedNodes);
    std::fill(std::begin(m_nodePosition), std::end(m_nodePosition), -1);
    for (size_t i = 0; i < m_executionOrder.size(); ++i)
        m_nodePosition[m_executionOrder[i]] = static_cast<int>(i);
    for (auto& handler : m_nodeHandlers) handler = {};

    for (NodeId node : m_executionOrder) {
        if (!validNode(node) || m_nodeWidths[node] == 0 || m_nodeWidths[node] > CM5_MAX_CHANNELS) continue;
        const size_t samples = static_cast<size_t>(CM5_BUFFER_FRAMES) * m_nodeWidths[node];
        std::fill_n(m_nodeBuffers[node].data(), samples, 0.0f);
        std::fill_n(m_feedbackBuffers[node].data(), samples, 0.0f);
        std::fill_n(m_nextFeedbackBuffers[node].data(), samples, 0.0f);
        const std::string endpoint = m_clientManager.get_endpoint_from_id(node);
        if (node == NODE_TONE_GENERATOR) {
            m_nodeHandlers[node] = {NodeHandler::Type::TONE, nullptr, 1, "tone/generator", true};
        } else {
            // Original hardware IDs and client keys may contain arbitrary '/'.
            // Match complete endpoint strings rather than splitting at a slash.
            for (const auto& dev : nextOwners.hardware) {
                const bool capture = dev->get_info().is_capture;
                if (endpoint == "hardware/" + dev->get_original_id() + (capture ? "/capture" : "/playback")) {
                    const unsigned channels = std::min(dev->get_info().channels, CM5_MAX_CHANNELS);
                    m_nodeHandlers[node] = {capture ? NodeHandler::Type::HARDWARE_CAPTURE : NodeHandler::Type::HARDWARE_PLAYBACK,
                        dev.get(), channels, endpoint, channels > 0};
                    break;
                }
            }
            if (!m_nodeHandlers[node].active) for (const auto& session : nextOwners.sessions) {
                if (!session->media_ready.load(std::memory_order_acquire)) continue;
                const std::string base = "client/" + session->get_client_key();
                const bool capture = endpoint == base + "/capture";
                if (capture || endpoint == base + "/playback") {
                    m_nodeHandlers[node] = {capture ? NodeHandler::Type::CLIENT_CAPTURE : NodeHandler::Type::CLIENT_PLAYBACK,
                        session.get(), std::min(capture ? session->input_channels.load() : session->output_channels.load(), CM5_MAX_CHANNELS), endpoint, true};
                    break;
                }
            }
        }
        if (!m_nodeHandlers[node].active)
            m_nodeHandlers[node] = {NodeHandler::Type::VIRTUAL, nullptr, m_nodeWidths[node], "virtual/" + std::to_string(node), true};
    }
    // Prepare meter widths only when rebuilding a layout, never each block.
    {
        std::lock_guard<std::mutex> metricsLock(m_metrics.lock);
        for (NodeId node : m_executionOrder) {
            const auto& handler = m_nodeHandlers[node];
            const bool capture = handler.type == NodeHandler::Type::HARDWARE_CAPTURE ||
                                 handler.type == NodeHandler::Type::CLIENT_CAPTURE;
            const bool playback = handler.type == NodeHandler::Type::HARDWARE_PLAYBACK ||
                                  handler.type == NodeHandler::Type::CLIENT_PLAYBACK;
            if (!capture && !playback) continue;
            auto& bank = capture ? m_metrics.capture[handler.endpointId] : m_metrics.playback[handler.endpointId];
            if (bank.size() != handler.channels) {
                std::vector<ChannelMeter> prepared(handler.channels);
                bank.swap(prepared);
            }
        }
    }
    m_currentRoutes = routes_ptr;
    for (auto& routes : m_adjList) routes.clear();
    if (routes_ptr) for (const auto& route : *routes_ptr)
        if (validRoute(route)) m_adjList[route.source_node].push_back(&route);

    // Every snapshot owner, including endpoints that did not bind a handler,
    // survives until a control thread collects it. Never release a possible last
    // HardwareDevice/WebClientSession owner while rebuilding on the audio thread.
    if (!m_endpointOwners.empty()) {
        std::lock_guard<std::mutex> retiredLock(m_retiredEndpointsLock);
        m_retiredEndpointOwners.push_back(std::move(m_endpointOwners));
    }
    m_endpointOwners = std::move(nextOwners);
    // Dirty is consumed ONLY at block entry, never cleared here: publication
    // during this rebuild must force another pass on the following block.
}

void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started (Dynamic Unified Timing)." << std::endl;

    // Mathematical interval: (Frames / SampleRate) * 1,000,000 microseconds
    auto interval = std::chrono::microseconds(
        (static_cast<long long>(CM5_BUFFER_FRAMES) * 1000000LL) / m_engineSampleRate
    );

    auto next_wakeup = std::chrono::steady_clock::now();

    while (m_running) {
        next_wakeup += interval;

        process_audio();

        auto now = std::chrono::steady_clock::now();
        if (next_wakeup > now) {
            std::this_thread::sleep_until(next_wakeup);
        } else {
            next_wakeup = now; // Prevent spinlock if an iteration runs long
        }
    }
}

void DspEngine::process_audio() {
    // Capture the requested revision before any processing. Even if dirty was
    // consumed just before a publisher increments its revision, the generation
    // mismatch guarantees that publication is rebuilt on the next block.
    const uint64_t target = m_requestedRevision.load(std::memory_order_acquire);
    const bool dirty = m_graphDirty.exchange(false, std::memory_order_acq_rel);
    if (dirty || m_builtRevision != target) {
        rebuild_graph();
        m_builtRevision = target;
    }
    for (NodeId node : m_executionOrder) {
        if (node >= 0 && static_cast<size_t>(node) < MAX_NODES) {
            std::fill_n(m_nodeBuffers[node].data(), static_cast<size_t>(CM5_BUFFER_FRAMES) * m_nodeWidths[node], 0.0f);
            std::fill_n(m_nextFeedbackBuffers[node].data(), static_cast<size_t>(CM5_BUFFER_FRAMES) * m_nodeWidths[node], 0.0f);
        }
    }

    for (NodeId node : m_executionOrder) {
        float* buf = get_buffer(node);
        if (!buf || node < 0 || node >= (int)MAX_NODES || !m_nodeHandlers[node].active) continue;
        const NodeHandler& handler = m_nodeHandlers[node];

        if (handler.type == NodeHandler::Type::TONE) {
            if (m_tone.enabled) {
                float freq = m_tone.frequency_hz, amp = m_tone.amplitude;
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
                    float sample = std::sin(m_tonePhase * 2.0 * M_PI) * amp;
                    buf[f] += sample;
                    m_tonePhase += freq / m_engineSampleRate;
                    if (m_tonePhase > 1.0) m_tonePhase -= 1.0;
                }
            }
        } else if (handler.type == NodeHandler::Type::HARDWARE_CAPTURE) {
            auto* dev = static_cast<HardwareDevice*>(handler.ptr);
            const unsigned chs = handler.channels;
            if (chs > 0 && chs <= CM5_MAX_CHANNELS && m_nodeWidths[node] == chs && dev->io_buffer.size() >= CM5_BUFFER_FRAMES * chs) {
                const size_t bytes = CM5_BUFFER_FRAMES * chs * sizeof(float);
                // A short read must not replay samples left by the previous block.
                std::fill_n(dev->io_buffer.data(), CM5_BUFFER_FRAMES * chs, 0.0f);
                dev->get_input_buffer().read(dev->io_buffer.data(), bytes);
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f)
                    for (unsigned ch = 0; ch < chs; ++ch)
                        buf[f * chs + ch] += dev->io_buffer[f * chs + ch];
                update_meters(buf, chs, chs, handler.endpointId, true);
            }
        } else if (handler.type == NodeHandler::Type::CLIENT_CAPTURE) {
            auto* session = static_cast<WebClientSession*>(handler.ptr);
            const unsigned chs = handler.channels;
            if (chs > 0 && chs <= CM5_MAX_CHANNELS && m_nodeWidths[node] == chs && session->io_buffer.size() >= CM5_BUFFER_FRAMES * chs) {
                std::fill_n(session->io_buffer.data(), CM5_BUFFER_FRAMES * chs, 0.0f);
                session->incoming_rb.read(session->io_buffer.data(), CM5_BUFFER_FRAMES * chs * sizeof(float));
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f)
                    for (unsigned ch = 0; ch < chs; ++ch)
                        buf[f * chs + ch] += session->io_buffer[f * chs + ch];
                update_meters(buf, chs, chs, handler.endpointId, true);
            }
        }

        if (node >= 0 && node < (int)MAX_NODES) {
            for (const auto* route : m_adjList[node]) {
                float* dstBuf = get_buffer(route->destination_node);
                if (dstBuf && route->source_channel < m_nodeWidths[node] &&
                    route->destination_channel < m_nodeWidths[route->destination_node]) {
                    bool isFeedback = false;
                    if (m_nodePosition[node] != -1 && m_nodePosition[route->destination_node] != -1) {
                        if (m_nodePosition[node] >= m_nodePosition[route->destination_node]) isFeedback = true;
                    }
                    float* srcData = isFeedback ? get_feedback_buffer(node) : buf;
                    for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f)
                        m_routeScratchBuffer[f] = srcData[f * m_nodeWidths[node] + route->source_channel];
                    float currentGain = route->gain;
                    if (supports_route_dsp(route->id)) {
                        RouteDsp& rdsp = m_routeDsp[route->id];
                        // Pin even a bypassed state: the control writer must
                        // never race gain/active reads or destroy its processors.
                        int idx;
                        do {
                            idx = rdsp.current_idx.load();
                            rdsp.reader_idx.store(idx);
                        } while (idx != rdsp.current_idx.load());
                        const auto& state = rdsp.chains[idx];
                        if (state.active) {
                            for (int i = 0; i < state.length; ++i)
                                if (state.processors[i]) state.processors[i]->process(m_routeScratchBuffer.data(), CM5_BUFFER_FRAMES, 1);
                            currentGain *= state.gain;
                        }
                        rdsp.reader_idx.store(-1);
                    }
                    for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f)
                        dstBuf[f * m_nodeWidths[route->destination_node] + route->destination_channel] += m_routeScratchBuffer[f] * currentGain;
                }
            }
        }

        if (handler.type == NodeHandler::Type::HARDWARE_PLAYBACK) {
            auto* dev = static_cast<HardwareDevice*>(handler.ptr);
            m_gainProc->process(buf, CM5_BUFFER_FRAMES, handler.channels);
            m_compProc->process(buf, CM5_BUFFER_FRAMES, handler.channels);
            const unsigned chs = handler.channels;
            if (chs > 0 && chs <= CM5_MAX_CHANNELS && m_nodeWidths[node] == chs && dev->io_buffer.size() >= CM5_BUFFER_FRAMES * chs) {
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f)
                    for (unsigned ch = 0; ch < chs; ++ch)
                        dev->io_buffer[f * chs + ch] = buf[f * chs + ch];
                update_meters(buf, chs, handler.channels, handler.endpointId, false);
                dev->get_output_buffer().write(dev->io_buffer.data(), CM5_BUFFER_FRAMES * chs * sizeof(float));
            }
        } else if (handler.type == NodeHandler::Type::CLIENT_PLAYBACK) {
            auto* session = static_cast<WebClientSession*>(handler.ptr);
            const unsigned chs = handler.channels;
            if (chs > 0 && chs <= CM5_MAX_CHANNELS && m_nodeWidths[node] == chs && session->io_buffer.size() >= CM5_BUFFER_FRAMES * chs) {
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f)
                    for (unsigned ch = 0; ch < chs; ++ch)
                        session->io_buffer[f * chs + ch] = buf[f * chs + ch];
                session->outgoing_rb.write(session->io_buffer.data(), CM5_BUFFER_FRAMES * chs * sizeof(float));
                update_meters(buf, chs, handler.channels, handler.endpointId, false);
            }
        }
        float* nextFb = get_next_feedback_buffer(node);
        if (nextFb) std::memcpy(nextFb, buf, CM5_BUFFER_FRAMES * handler.channels * sizeof(float));
    }
    for (NodeId node : m_executionOrder) {
        if (node >= 0 && static_cast<size_t>(node) < MAX_NODES)
            std::swap(m_feedbackBuffers[node], m_nextFeedbackBuffers[node]);
    }
    // A waiter can now safely act on this revision: no old block is in flight.
    m_adoptedRevision.store(target, std::memory_order_release);
    m_revisionChanged.notify_all();
}
