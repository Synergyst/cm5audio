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
#include <cstdlib>
#include <cstring>
#include <map>
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
    std::string value;
};

struct NodeInfo {
    std::string name;
    std::string description;
};

struct PendingPort {
    uint32_t nodeId = 0;
    std::string name;
    std::string alias;
};

struct RegistryScan {
    pw_main_loop* loop = nullptr;
    int syncSequence = -1;

    std::map<uint32_t, NodeInfo> nodes;
    std::vector<PendingPort> ports;
};

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

            // We only want MIDI source/output ports.
            if (std::strcmp(direction, "out") != 0)
                return;

            errno = 0;
            char* end = nullptr;

            const unsigned long parsed =
                std::strtoul(nodeIdText, &end, 10);

            if (errno || end == nodeIdText || !end || *end != '\0' ||
                parsed > UINT32_MAX) {
                return;
            }

            PendingPort port;
            port.nodeId = static_cast<uint32_t>(parsed);
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

inline std::vector<MidiOutput> discoverOutputs()
{
    std::vector<MidiOutput> outputs;

    RegistryScan scan;

    pw_main_loop* loop = pw_main_loop_new(nullptr);
    if (!loop)
        return outputs;

    scan.loop = loop;

    pw_context* context = pw_context_new(
        pw_main_loop_get_loop(loop),
        nullptr,
        0);

    if (!context) {
        pw_main_loop_destroy(loop);
        return outputs;
    }

    pw_core* core = pw_context_connect(
        context,
        nullptr,
        0);

    if (!core) {
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return outputs;
    }

    pw_registry* registry = pw_core_get_registry(
        core,
        PW_VERSION_REGISTRY,
        0);

    if (!registry) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return outputs;
    }

    spa_hook registryListener{};
    spa_hook coreListener{};

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

    scan.syncSequence = pw_core_sync(core, PW_ID_CORE, 0);

    if (scan.syncSequence >= 0)
        pw_main_loop_run(loop);

    // All globals delivered before the sync response are now collected.
    for (const PendingPort& port : scan.ports) {
        const auto nodeIt = scan.nodes.find(port.nodeId);

        if (nodeIt == scan.nodes.end())
            continue;

        const NodeInfo& node = nodeIt->second;

        if (node.name.empty() || port.name.empty())
            continue;

        MidiOutput output;
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

    std::sort(
        outputs.begin(),
        outputs.end(),
        [](const MidiOutput& a, const MidiOutput& b) {
            return a.label < b.label;
        });

    spa_hook_remove(&registryListener);
    spa_hook_remove(&coreListener);

    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));

    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

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

inline pw_proxy* createInputLink(pw_filter* filter,
                                 const std::string& outputSelection,
                                 const char* inputPortName)
{
    if (!filter || !inputPortName || !*inputPortName)
        return nullptr;

    std::string outputNode;
    std::string outputPort;

    if (!decodeOutputSelection(
            outputSelection,
            outputNode,
            outputPort)) {
        return nullptr;
    }

    pw_core* core = pw_filter_get_core(filter);

    if (!core)
        return nullptr;

    const uint32_t inputNodeId =
        pw_filter_get_node_id(filter);

    if (inputNodeId == PW_ID_ANY)
        return nullptr;

    const std::string inputNode =
        std::to_string(inputNodeId);

    pw_properties* properties = pw_properties_new(
        PW_KEY_LINK_OUTPUT_NODE, outputNode.c_str(),
        PW_KEY_LINK_OUTPUT_PORT, outputPort.c_str(),
        PW_KEY_LINK_INPUT_NODE, inputNode.c_str(),
        PW_KEY_LINK_INPUT_PORT, inputPortName,
        nullptr);

    if (!properties)
        return nullptr;

    void* object = pw_core_create_object(
        core,
        "link-factory",
        PW_TYPE_INTERFACE_Link,
        PW_VERSION_LINK,
        &properties->dict,
        0);

    pw_properties_free(properties);

    return static_cast<pw_proxy*>(object);
}

inline void destroyInputLink(pw_filter* filter,
                             pw_proxy*& linkProxy)
{
    if (!filter || !linkProxy)
        return;

    pw_core* core = pw_filter_get_core(filter);

    if (core)
        pw_core_destroy(core, linkProxy);

    linkProxy = nullptr;
}

} // namespace midi
} // namespace cm5audio

#endif
