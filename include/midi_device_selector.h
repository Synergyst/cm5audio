#ifndef CM5AUDIO_MIDI_DEVICE_SELECTOR_H
#define CM5AUDIO_MIDI_DEVICE_SELECTOR_H

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/filter.h>
#include <pipewire/link.h>
#include <pipewire/proxy.h>

#include <spa/utils/dict.h>
#include <spa/utils/hook.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cm5audio {
namespace midi {

constexpr const char* MANUAL_OUTPUT = "manual";
constexpr char OUTPUT_SEPARATOR = '\x1f';

struct MidiOutput {
    std::string label;
    std::string nodeName;
    std::string portName;
    uint32_t objectId = PW_ID_ANY;
    std::string value;
};

struct NodeInfo {
    std::string name;
    std::string description;
};

struct PendingPort {
    uint32_t nodeId = 0;
    uint32_t objectId = PW_ID_ANY;
    std::string name;
    std::string alias;
};

struct RegistryScan {
    pw_main_loop* loop = nullptr;
    int syncSequence = -1;

    std::map<uint32_t, NodeInfo> nodes;
    std::vector<PendingPort> ports;
};

inline bool parseObjectId(const char* text, uint32_t& result)
{
    if (!text || !*text)
        return false;

    errno = 0;
    char* end = nullptr;

    const unsigned long parsed = std::strtoul(text, &end, 10);

    if (errno || end == text || !end || *end != '\0' ||
        parsed > UINT32_MAX) {
        return false;
    }

    result = static_cast<uint32_t>(parsed);
    return true;
}

inline bool isMidiNode(const spa_dict* props)
{
    if (!props)
        return false;

    const char* mediaType =
        spa_dict_lookup(props, PW_KEY_MEDIA_TYPE);

    const char* mediaClass =
        spa_dict_lookup(props, "media.class");

    if (mediaType && std::strcmp(mediaType, "Midi") == 0)
        return true;

    return mediaClass &&
           std::strstr(mediaClass, "Midi") != nullptr;
}

inline void onRegistryGlobal(void* userdata,
                             uint32_t id,
                             uint32_t permissions,
                             const char* type,
                             uint32_t version,
                             const spa_dict* props)
{
    (void)permissions;
    (void)version;

    auto* scan = static_cast<RegistryScan*>(userdata);

    if (!scan || !type || !props)
        return;

    try {
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            if (!isMidiNode(props))
                return;

            const char* name =
                spa_dict_lookup(props, PW_KEY_NODE_NAME);

            const char* description =
                spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

            if (!name || !*name)
                return;

            NodeInfo node;
            node.name = name;
            node.description =
                description && *description
                    ? description
                    : name;

            scan->nodes[id] = std::move(node);
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const char* nodeIdText =
                spa_dict_lookup(props, PW_KEY_NODE_ID);

            const char* direction =
                spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);

            const char* portName =
                spa_dict_lookup(props, PW_KEY_PORT_NAME);

            const char* portAlias =
                spa_dict_lookup(props, PW_KEY_PORT_ALIAS);

            if (!nodeIdText || !direction || !portName)
                return;

            if (std::strcmp(direction, "out") != 0)
                return;

            uint32_t nodeId = 0;

            if (!parseObjectId(nodeIdText, nodeId))
                return;

            PendingPort port;
            port.nodeId = nodeId;
            port.objectId = id;
            port.name = portName;
            port.alias =
                portAlias && *portAlias
                    ? portAlias
                    : portName;

            scan->ports.push_back(std::move(port));
        }
    }
    catch (...) {
        // Never allow a C++ exception to escape a PipeWire callback.
    }
}

inline void onCoreDone(void* userdata,
                       uint32_t id,
                       int sequence)
{
    auto* scan = static_cast<RegistryScan*>(userdata);

    if (!scan || !scan->loop)
        return;

    if (id == PW_ID_CORE && sequence == scan->syncSequence)
        pw_main_loop_quit(scan->loop);
}

inline bool openRegistryScan(pw_main_loop*& loop,
                             pw_context*& context,
                             pw_core*& core,
                             pw_registry*& registry)
{
    loop = pw_main_loop_new(nullptr);

    if (!loop) {
        std::fprintf(stderr,
            "[CM5Audio MIDI] Registry scan: "
            "pw_main_loop_new failed\n");
        return false;
    }

    context = pw_context_new(
        pw_main_loop_get_loop(loop),
        nullptr,
        0);

    if (!context) {
        std::fprintf(stderr,
            "[CM5Audio MIDI] Registry scan: "
            "pw_context_new failed\n");
        return false;
    }

    core = pw_context_connect(context, nullptr, 0);

    if (!core) {
        std::fprintf(stderr,
            "[CM5Audio MIDI] Registry scan: "
            "pw_context_connect failed: %s\n",
            std::strerror(errno));
        return false;
    }

    registry = pw_core_get_registry(
        core,
        PW_VERSION_REGISTRY,
        0);

    if (!registry) {
        std::fprintf(stderr,
            "[CM5Audio MIDI] Registry scan: "
            "pw_core_get_registry failed\n");
        return false;
    }

    return true;
}

inline void closeRegistryScan(pw_main_loop*& loop,
                              pw_context*& context,
                              pw_core*& core,
                              pw_registry*& registry)
{
    if (registry) {
        pw_proxy_destroy(
            reinterpret_cast<pw_proxy*>(registry));
        registry = nullptr;
    }

    if (core) {
        pw_core_disconnect(core);
        core = nullptr;
    }

    if (context) {
        pw_context_destroy(context);
        context = nullptr;
    }

    if (loop) {
        pw_main_loop_destroy(loop);
        loop = nullptr;
    }
}

inline bool runRegistryScan(pw_main_loop* loop,
                            pw_core* core,
                            pw_registry* registry,
                            RegistryScan& scan,
                            spa_hook& registryListener,
                            spa_hook& coreListener)
{
    pw_registry_events registryEvents{};
    registryEvents.version = PW_VERSION_REGISTRY;
    registryEvents.global = onRegistryGlobal;

    pw_core_events coreEvents{};
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done = onCoreDone;

    pw_registry_add_listener(
        registry,
        &registryListener,
        &registryEvents,
        &scan);

    pw_core_add_listener(
        core,
        &coreListener,
        &coreEvents,
        &scan);

    scan.loop = loop;
    scan.syncSequence = pw_core_sync(core, PW_ID_CORE, 0);

    if (scan.syncSequence < 0) {
        std::fprintf(stderr,
            "[CM5Audio MIDI] Registry scan: "
            "pw_core_sync failed: %d\n",
            scan.syncSequence);

        spa_hook_remove(&registryListener);
        spa_hook_remove(&coreListener);
        return false;
    }

    pw_main_loop_run(loop);

    spa_hook_remove(&registryListener);
    spa_hook_remove(&coreListener);

    return true;
}

// ------------------------------------------------------------
// MIDI output discovery
// ------------------------------------------------------------

inline std::vector<MidiOutput> discoverOutputs()
{
    std::vector<MidiOutput> outputs;

    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;

    if (!openRegistryScan(loop, context, core, registry)) {
        closeRegistryScan(loop, context, core, registry);
        return outputs;
    }

    RegistryScan scan;
    spa_hook registryListener{};
    spa_hook coreListener{};

    if (runRegistryScan(
            loop,
            core,
            registry,
            scan,
            registryListener,
            coreListener)) {

        for (const PendingPort& port : scan.ports) {
            const auto nodeIt = scan.nodes.find(port.nodeId);

            if (nodeIt == scan.nodes.end())
                continue;

            const NodeInfo& node = nodeIt->second;

            if (node.name.empty() || port.name.empty())
                continue;

            MidiOutput output;
            output.objectId = port.objectId;

            output.nodeName = node.name;
            output.portName = port.name;
            output.label = node.description;

            if (port.alias != node.description)
                output.label += " — " + port.alias;

            output.value =
                output.nodeName +
                OUTPUT_SEPARATOR +
                output.portName;

            outputs.push_back(std::move(output));
        }
    }

    std::sort(
        outputs.begin(),
        outputs.end(),
        [](const MidiOutput& a, const MidiOutput& b) {
            return a.label < b.label;
        });

    std::fprintf(stderr,
        "[CM5Audio MIDI] Discovery complete: "
        "%zu MIDI nodes, %zu output-port candidates, "
        "%zu selectable MIDI outputs\n",
        scan.nodes.size(),
        scan.ports.size(),
        outputs.size());

    for (const MidiOutput& output : outputs) {
        std::fprintf(stderr,
            "[CM5Audio MIDI] Discovered output: "
            "node='%s' port='%s' label='%s'\n",
            output.nodeName.c_str(),
            output.portName.c_str(),
            output.label.c_str());
    }

    closeRegistryScan(loop, context, core, registry);

    return outputs;
}

inline std::vector<std::pair<std::string, nlohmann::json>>
makeOutputChoices()
{
    std::vector<std::pair<std::string, nlohmann::json>> choices;

    choices.emplace_back(
        "Manual / QPWGraph",
        MANUAL_OUTPUT);

    const std::vector<MidiOutput> outputs = discoverOutputs();

    for (const MidiOutput& output : outputs) {
        choices.emplace_back(
            output.label,
            output.value);
    }

    return choices;
}

inline bool decodeOutputSelection(const std::string& value,
                                  std::string& nodeName,
                                  std::string& portName)
{
    nodeName.clear();
    portName.clear();

    if (value.empty() || value == MANUAL_OUTPUT)
        return false;

    const size_t separator =
        value.find(OUTPUT_SEPARATOR);

    if (separator == std::string::npos ||
        separator == 0 ||
        separator + 1 >= value.size()) {
        return false;
    }

    nodeName = value.substr(0, separator);
    portName = value.substr(separator + 1);

    return !nodeName.empty() && !portName.empty();
}

// ------------------------------------------------------------
// Destination input-port discovery
//
// The global PipeWire Port object ID is used for link.input.port.
// This avoids assuming that the filter's requested port.name is
// identical to its registered PipeWire port name.
// ------------------------------------------------------------

struct InputPortCandidate {
    uint32_t objectId = PW_ID_ANY;
    std::string name;
    std::string alias;
    std::string objectPath;
    std::string format;
};

struct InputPortScan {
    pw_main_loop* loop = nullptr;
    int syncSequence = -1;

    uint32_t wantedNodeId = PW_ID_ANY;
    std::string nodeName;
    std::vector<InputPortCandidate> ports;
};

inline void onInputPortGlobal(void* userdata,
                              uint32_t id,
                              uint32_t permissions,
                              const char* type,
                              uint32_t version,
                              const spa_dict* props)
{
    (void)permissions;
    (void)version;

    auto* scan = static_cast<InputPortScan*>(userdata);
    if (!scan || !type || !props)
        return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        if (id == scan->wantedNodeId) {
            const char* nodeName =
                spa_dict_lookup(props, PW_KEY_NODE_NAME);

            if (nodeName && *nodeName)
                scan->nodeName = nodeName;
        }
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
        return;

    try {
        const char* nodeIdText =
            spa_dict_lookup(props, PW_KEY_NODE_ID);

        const char* direction =
            spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);

        const char* portName =
            spa_dict_lookup(props, PW_KEY_PORT_NAME);

        const char* portAlias =
            spa_dict_lookup(props, PW_KEY_PORT_ALIAS);

        const char* objectPath =
            spa_dict_lookup(props, "object.path");

        const char* format =
            spa_dict_lookup(props, PW_KEY_FORMAT_DSP);

        if (!nodeIdText || !direction || !portName ||
            !objectPath || !*objectPath)
            return;

        if (std::strcmp(direction, "in") != 0)
            return;

        uint32_t nodeId = PW_ID_ANY;

        if (!parseObjectId(nodeIdText, nodeId))
            return;

        if (nodeId != scan->wantedNodeId)
            return;

        InputPortCandidate candidate;

        // id is the global PipeWire object ID of this Port.
        candidate.objectId = id;
        candidate.name = portName;

        candidate.alias =
            portAlias && *portAlias
                ? portAlias
                : portName;

        candidate.objectPath = objectPath;

        candidate.format =
            format && *format
                ? format
                : "";

        scan->ports.push_back(std::move(candidate));
    }
    catch (...) {
        // Never allow a C++ exception to escape a PipeWire callback.
    }
}

inline void onInputPortCoreDone(void* userdata,
                                uint32_t id,
                                int sequence)
{
    auto* scan = static_cast<InputPortScan*>(userdata);

    if (!scan || !scan->loop)
        return;

    if (id == PW_ID_CORE && sequence == scan->syncSequence)
        pw_main_loop_quit(scan->loop);
}
inline bool discoverInputPort(uint32_t nodeId,
                              InputPortCandidate& selected,
                              std::string& selectedNodeName)
{
    selected = {};
    selectedNodeName.clear();

    pw_main_loop* loop = pw_main_loop_new(nullptr);

    if (!loop) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Input-port discovery: pw_main_loop_new failed\n");
        return false;
    }

    pw_context* context = pw_context_new(
        pw_main_loop_get_loop(loop),
        nullptr,
        0);

    if (!context) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Input-port discovery: pw_context_new failed\n");
        pw_main_loop_destroy(loop);
        return false;
    }

    pw_core* core = pw_context_connect(context, nullptr, 0);

    if (!core) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Input-port discovery: pw_context_connect failed: %s\n",
            std::strerror(errno));

        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return false;
    }

    pw_registry* registry = pw_core_get_registry(
        core,
        PW_VERSION_REGISTRY,
        0);

    if (!registry) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Input-port discovery: pw_core_get_registry failed\n");

        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return false;
    }

    InputPortScan scan;
    scan.loop = loop;
    scan.wantedNodeId = nodeId;

    spa_hook registryListener{};
    spa_hook coreListener{};

    pw_registry_events registryEvents{};
    registryEvents.version = PW_VERSION_REGISTRY;
    registryEvents.global = onInputPortGlobal;

    pw_core_events coreEvents{};
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done = onInputPortCoreDone;

    pw_registry_add_listener(
        registry,
        &registryListener,
        &registryEvents,
        &scan);

    pw_core_add_listener(
        core,
        &coreListener,
        &coreEvents,
        &scan);

    scan.syncSequence =
        pw_core_sync(core, PW_ID_CORE, 0);

    bool scanSucceeded = false;

    if (scan.syncSequence >= 0) {
        pw_main_loop_run(loop);
        scanSucceeded = true;
    } else {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Input-port discovery: pw_core_sync failed: %d\n",
            scan.syncSequence);
    }

    spa_hook_remove(&registryListener);
    spa_hook_remove(&coreListener);

    // Prefer the raw UMP input if multiple input ports exist.
    if (scanSucceeded && !scan.ports.empty()) {
        auto selectedIt = std::find_if(
            scan.ports.begin(),
            scan.ports.end(),
            [](const InputPortCandidate& port) {
                return port.format == "32 bit raw UMP";
            });

        if (selectedIt == scan.ports.end())
            selectedIt = scan.ports.begin();

        selected = *selectedIt;
    }
    if (scanSucceeded)
        selectedNodeName = scan.nodeName;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Destination node %u: node.name='%s'\n",
        nodeId,
        scan.nodeName.c_str());

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Destination node %u: found %zu input ports\n",
        nodeId,
        scan.ports.size());

    for (const InputPortCandidate& port : scan.ports) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Destination port: object.id=%u name='%s' "
            "alias='%s' format='%s'%s\n",
            port.objectId,
            port.name.c_str(),
            port.alias.c_str(),
            port.format.c_str(),
            port.objectId == selected.objectId ? " [SELECTED]" : "");
    }

    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    return scanSucceeded &&
           !selectedNodeName.empty() &&
           selected.objectId != PW_ID_ANY;
}

// ------------------------------------------------------------
// Link state diagnostics
// ------------------------------------------------------------

struct LinkWatch {
    std::string description;

    spa_hook linkListener{};
    spa_hook proxyListener{};

    bool linkListenerAdded = false;
    bool proxyListenerAdded = false;
};

inline std::map<pw_proxy*, std::unique_ptr<LinkWatch>>& linkWatchers()
{
    static std::map<pw_proxy*, std::unique_ptr<LinkWatch>> watchers;
    return watchers;
}

inline std::mutex& linkWatchersMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline const char* linkStateName(enum pw_link_state state)
{
    switch (state) {
    case PW_LINK_STATE_ERROR:
        return "ERROR";
    case PW_LINK_STATE_UNLINKED:
        return "UNLINKED";
    case PW_LINK_STATE_INIT:
        return "INIT";
    case PW_LINK_STATE_NEGOTIATING:
        return "NEGOTIATING";
    case PW_LINK_STATE_ALLOCATING:
        return "ALLOCATING";
    case PW_LINK_STATE_PAUSED:
        return "PAUSED";
    case PW_LINK_STATE_ACTIVE:
        return "ACTIVE";
    default:
        return "UNKNOWN";
    }
}

inline void onLinkInfo(void* userdata,
                       const pw_link_info* info)
{
    auto* watch = static_cast<LinkWatch*>(userdata);

    if (!watch || !info)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] %s: "
        "link id=%u state=%s (%d), "
        "output node=%u port=%u, "
        "input node=%u port=%u",
        watch->description.c_str(),
        info->id,
        linkStateName(info->state),
        static_cast<int>(info->state),
        info->output_node_id,
        info->output_port_id,
        info->input_node_id,
        info->input_port_id);

    if (info->error && *info->error)
        std::fprintf(stderr, ", error='%s'", info->error);

    std::fprintf(stderr, "\n");
}

inline void onLinkProxyBound(void* userdata,
                             uint32_t globalId)
{
    auto* watch = static_cast<LinkWatch*>(userdata);

    if (!watch)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] %s: "
        "link proxy bound to global object id=%u\n",
        watch->description.c_str(),
        globalId);
}

inline void onLinkProxyRemoved(void* userdata)
{
    auto* watch = static_cast<LinkWatch*>(userdata);

    if (!watch)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] %s: "
        "link proxy was removed by PipeWire\n",
        watch->description.c_str());
}

inline void onLinkProxyDestroy(void* userdata)
{
    auto* watch = static_cast<LinkWatch*>(userdata);

    if (!watch)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] %s: "
        "link proxy was destroyed\n",
        watch->description.c_str());
}

inline void onLinkProxyError(void* userdata,
                             int sequence,
                             int result,
                             const char* message)
{
    auto* watch = static_cast<LinkWatch*>(userdata);

    if (!watch)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] %s: "
        "PIPEWIRE PROXY ERROR: sequence=%d result=%d "
        "message='%s'\n",
        watch->description.c_str(),
        sequence,
        result,
        message ? message : "unknown");
}

inline void attachLinkDiagnostics(pw_proxy* proxy,
                                  const char* description)
{
    if (!proxy)
        return;

    auto watch = std::make_unique<LinkWatch>();
    watch->description =
        description ? description : "MIDI input";

    LinkWatch* rawWatch = watch.get();

    {
        std::lock_guard<std::mutex> lock(linkWatchersMutex());
        linkWatchers()[proxy] = std::move(watch);
    }

    static const pw_link_events linkEvents = [] {
        pw_link_events events{};
        events.version = PW_VERSION_LINK_EVENTS;
        events.info = onLinkInfo;
        return events;
    }();

    static const pw_proxy_events proxyEvents = [] {
        pw_proxy_events events{};
        events.version = PW_VERSION_PROXY_EVENTS;
        events.destroy = onLinkProxyDestroy;
        events.bound = onLinkProxyBound;
        events.removed = onLinkProxyRemoved;
        events.error = onLinkProxyError;
        return events;
    }();

    const int linkResult = pw_link_add_listener(
        reinterpret_cast<pw_link*>(proxy),
        &rawWatch->linkListener,
        &linkEvents,
        rawWatch);

    if (linkResult >= 0) {
        rawWatch->linkListenerAdded = true;
    } else {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] %s: "
            "pw_link_add_listener failed: %d\n",
            rawWatch->description.c_str(),
            linkResult);
    }

    pw_proxy_add_listener(
        proxy,
        &rawWatch->proxyListener,
        &proxyEvents,
        rawWatch);

    rawWatch->proxyListenerAdded = true;
}

inline void detachLinkDiagnostics(pw_proxy* proxy)
{
    if (!proxy)
        return;

    std::unique_ptr<LinkWatch> watch;

    {
        std::lock_guard<std::mutex> lock(linkWatchersMutex());

        auto& watchers = linkWatchers();
        const auto it = watchers.find(proxy);

        if (it == watchers.end())
            return;

        watch = std::move(it->second);
        watchers.erase(it);
    }

    if (watch->linkListenerAdded)
        spa_hook_remove(&watch->linkListener);

    if (watch->proxyListenerAdded)
        spa_hook_remove(&watch->proxyListener);
}

// ------------------------------------------------------------
// Automatic MIDI input linking
// ------------------------------------------------------------

struct LinkRegistryPort {
    uint32_t objectId = PW_ID_ANY;
    uint32_t nodeId = PW_ID_ANY;
    std::string direction;
    std::string name;
};

struct InputLinkRequest {
    pw_filter* filter = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;

    spa_hook registryListener{};
    spa_hook coreListener{};

    bool registryListenerAdded = false;
    bool coreListenerAdded = false;

    int syncSequence = -1;

    uint32_t inputNodeId = PW_ID_ANY;

    std::string outputNode;
    std::string outputPort;
    std::string inputPort;

    std::map<uint32_t, std::string> nodeNames;
    std::vector<LinkRegistryPort> ports;

    void (*completed)(void*, pw_proxy*) = nullptr;
    void* userdata = nullptr;
};

inline void releaseInputLinkRequest(InputLinkRequest* request)
{
    if (!request)
        return;

    if (request->registryListenerAdded) {
        spa_hook_remove(&request->registryListener);
        request->registryListenerAdded = false;
    }

    if (request->coreListenerAdded) {
        spa_hook_remove(&request->coreListener);
        request->coreListenerAdded = false;
    }

    if (request->registry) {
        pw_proxy_destroy(
            reinterpret_cast<pw_proxy*>(request->registry));
        request->registry = nullptr;
    }
}

inline void finishInputLinkRequest(InputLinkRequest* request,
                                   pw_proxy* proxy)
{
    if (!request)
        return;

    auto completed = request->completed;
    void* userdata = request->userdata;

    releaseInputLinkRequest(request);
    delete request;

    if (completed)
        completed(userdata, proxy);
}

inline void onInputLinkRegistryGlobal(void* userdata,
                                      uint32_t id,
                                      uint32_t permissions,
                                      const char* type,
                                      uint32_t version,
                                      const spa_dict* props)
{
    (void)permissions;
    (void)version;

    auto* request = static_cast<InputLinkRequest*>(userdata);

    if (!request || !type || !props)
        return;

    try {
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* nodeName =
                spa_dict_lookup(props, PW_KEY_NODE_NAME);

            if (nodeName && *nodeName)
                request->nodeNames[id] = nodeName;

            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
            return;

        const char* nodeIdText =
            spa_dict_lookup(props, PW_KEY_NODE_ID);

        const char* direction =
            spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);

        const char* portName =
            spa_dict_lookup(props, PW_KEY_PORT_NAME);

        if (!nodeIdText || !direction || !portName)
            return;

        uint32_t nodeId = PW_ID_ANY;

        if (!parseObjectId(nodeIdText, nodeId))
            return;

        LinkRegistryPort port;
        port.objectId = id;
        port.nodeId = nodeId;
        port.direction = direction;
        port.name = portName;

        request->ports.push_back(std::move(port));
    }
    catch (...) {
        // Never let an exception escape a PipeWire callback.
    }
}

inline void onInputLinkCoreDone(void* userdata,
                                uint32_t id,
                                int sequence)
{
    auto* request = static_cast<InputLinkRequest*>(userdata);

    if (!request ||
        id != PW_ID_CORE ||
        sequence != request->syncSequence) {
        return;
    }

    /*
     * Resolve the selected output node using this filter's registry.
     */
    uint32_t outputNodeId = PW_ID_ANY;

    for (const auto& node : request->nodeNames) {
        if (node.second == request->outputNode) {
            outputNodeId = node.first;
            break;
        }
    }

    /*
     * Resolve both endpoint ports using IDs from this same registry.
     */
    uint32_t outputPortId = PW_ID_ANY;
    uint32_t inputPortId = PW_ID_ANY;

    for (const LinkRegistryPort& port : request->ports) {
        if (port.nodeId == outputNodeId &&
            port.direction == "out" &&
            port.name == request->outputPort) {
            outputPortId = port.objectId;
        }

        if (port.nodeId == request->inputNodeId &&
            port.direction == "in" &&
            port.name == request->inputPort) {
            inputPortId = port.objectId;
        }
    }

    if (outputNodeId == PW_ID_ANY ||
        outputPortId == PW_ID_ANY ||
        inputPortId == PW_ID_ANY) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Same-core registry resolution failed:\n"
            "  output node='%s' node.id=%u port='%s' port.id=%u\n"
            "  input node.id=%u port='%s' port.id=%u\n"
            "  registry nodes=%zu ports=%zu\n",
            request->outputNode.c_str(),
            outputNodeId,
            request->outputPort.c_str(),
            outputPortId,
            request->inputNodeId,
            request->inputPort.c_str(),
            inputPortId,
            request->nodeNames.size(),
            request->ports.size());

        finishInputLinkRequest(request, nullptr);
        return;
    }

    const std::string outputPortIdString =
        std::to_string(outputPortId);

    const std::string inputPortIdString =
        std::to_string(inputPortId);

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Resolved endpoints on filter core:\n"
        "  output %s:%s -> global port ID %u\n"
        "  input node ID %u:%s -> global port ID %u\n",
        request->outputNode.c_str(),
        request->outputPort.c_str(),
        outputPortId,
        request->inputNodeId,
        request->inputPort.c_str(),
        inputPortId);

    /*
     * Match pw-link: supply the resolved global port IDs, without
     * either node selector, using this exact same pw_core.
     */
    pw_properties* properties = pw_properties_new(
        PW_KEY_LINK_OUTPUT_PORT, outputPortIdString.c_str(),
        PW_KEY_LINK_INPUT_PORT, inputPortIdString.c_str(),
        nullptr);

    if (!properties) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Could not allocate link-factory properties\n");

        finishInputLinkRequest(request, nullptr);
        return;
    }

    errno = 0;

    void* object = pw_core_create_object(
        request->core,
        "link-factory",
        PW_TYPE_INTERFACE_Link,
        PW_VERSION_LINK,
        &properties->dict,
        0);

    const int createErrno = errno;

    pw_properties_free(properties);

    if (!object) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "pw_core_create_object(link-factory) failed: "
            "errno=%d (%s)\n",
            createErrno,
            std::strerror(createErrno));

        finishInputLinkRequest(request, nullptr);
        return;
    }

    auto* proxy = static_cast<pw_proxy*>(object);

    const auto inputNodeIt =
        request->nodeNames.find(request->inputNodeId);

    const std::string inputNodeName =
        inputNodeIt != request->nodeNames.end()
            ? inputNodeIt->second
            : std::to_string(request->inputNodeId);

    const std::string description =
        "MIDI " + request->outputNode + ":" +
        request->outputPort + " -> " +
        inputNodeName + ":" + request->inputPort;

    attachLinkDiagnostics(proxy, description.c_str());

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Link-factory request created using filter core. "
        "proxy=%p\n",
        static_cast<void*>(proxy));

    finishInputLinkRequest(request, proxy);
}

inline InputLinkRequest* createInputLinkAsync(
    pw_filter* filter,
    const std::string& outputSelection,
    const char* inputPortName,
    void (*completed)(void*, pw_proxy*),
    void* userdata)
{
    if (!filter || !completed) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Cannot start link request: invalid filter or callback\n");
        return nullptr;
    }

    std::string outputNode;
    std::string outputPort;

    if (!decodeOutputSelection(
            outputSelection,
            outputNode,
            outputPort)) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Cannot start link request: invalid output selection\n");
        return nullptr;
    }

    if (!inputPortName || !*inputPortName) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Cannot start link request: empty input port name\n");
        return nullptr;
    }

    auto* request = new (std::nothrow) InputLinkRequest();

    if (!request) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Cannot allocate asynchronous link request\n");
        return nullptr;
    }

    request->filter = filter;
    request->core = pw_filter_get_core(filter);
    request->inputNodeId = pw_filter_get_node_id(filter);
    request->outputNode = std::move(outputNode);
    request->outputPort = std::move(outputPort);
    request->inputPort = inputPortName;
    request->completed = completed;
    request->userdata = userdata;

    if (!request->core || request->inputNodeId == PW_ID_ANY) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "Cannot start link request: filter core/node unavailable\n");
        delete request;
        return nullptr;
    }

    /*
     * IMPORTANT:
     * This registry belongs to the filter's own core. The port IDs
     * received below will be passed back to this same core.
     */
    request->registry = pw_core_get_registry(
        request->core,
        PW_VERSION_REGISTRY,
        0);

    if (!request->registry) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "pw_core_get_registry on filter core failed\n");
        delete request;
        return nullptr;
    }

    static const pw_registry_events registryEvents = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = onInputLinkRegistryGlobal;
        return events;
    }();

    static const pw_core_events coreEvents = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = onInputLinkCoreDone;
        return events;
    }();

    int result = pw_registry_add_listener(
        request->registry,
        &request->registryListener,
        &registryEvents,
        request);

    if (result < 0) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "pw_registry_add_listener failed: %d\n",
            result);

        releaseInputLinkRequest(request);
        delete request;
        return nullptr;
    }

    request->registryListenerAdded = true;

    result = pw_core_add_listener(
        request->core,
        &request->coreListener,
        &coreEvents,
        request);

    if (result < 0) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "pw_core_add_listener failed: %d\n",
            result);

        releaseInputLinkRequest(request);
        delete request;
        return nullptr;
    }

    request->coreListenerAdded = true;

    request->syncSequence =
        pw_core_sync(request->core, PW_ID_CORE, 0);

    if (request->syncSequence < 0) {
        std::fprintf(stderr,
            "[CM5Audio MIDI AutoConnect] "
            "pw_core_sync failed: %d\n",
            request->syncSequence);

        releaseInputLinkRequest(request);
        delete request;
        return nullptr;
    }

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Asynchronous same-core registry scan started: "
        "output='%s:%s', input node ID=%u, input port='%s'\n",
        request->outputNode.c_str(),
        request->outputPort.c_str(),
        request->inputNodeId,
        request->inputPort.c_str());

    return request;
}

inline void cancelInputLinkRequest(InputLinkRequest*& request)
{
    if (!request)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Cancelling pending same-core registry/link request\n");

    releaseInputLinkRequest(request);
    delete request;
    request = nullptr;
}

inline void destroyInputLink(pw_filter* filter,
                             pw_proxy*& linkProxy)
{
    if (!linkProxy)
        return;

    std::fprintf(stderr,
        "[CM5Audio MIDI AutoConnect] "
        "Destroying MIDI link proxy=%p\n",
        static_cast<void*>(linkProxy));

    detachLinkDiagnostics(linkProxy);

    if (filter) {
        pw_core* core = pw_filter_get_core(filter);

        if (core) {
            pw_core_destroy(core, linkProxy);
        } else {
            std::fprintf(stderr,
                "[CM5Audio MIDI AutoConnect] "
                "Cannot destroy link: filter core is NULL\n");
        }
    }

    linkProxy = nullptr;
}

} // namespace midi
} // namespace cm5audio

#endif
