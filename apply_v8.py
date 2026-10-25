#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime
import os, sys

ROOT = Path.cwd()
header_path = ROOT / "include/device_manager.h"
source_path = ROOT / "src/device_manager.cpp"
if not header_path.is_file() or not source_path.is_file():
    sys.exit("Run this from the cm5audio repository root (needs include/device_manager.h and src/device_manager.cpp).")

h0 = header_path.read_text()
c0 = source_path.read_text()
h, c = h0, c0

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one match for {label}, found {count}; no files have been changed.")
    return text.replace(old, new, 1)

# Header: keep all PipeWire proxy bookkeeping in DeviceManager, and make graph
# reconciliation wait until node info has been received.
h = replace_once(h,
'''struct pw_stream;
struct spa_loop;''',
'''struct pw_stream;
struct spa_loop;
struct pw_node;
struct pw_registry;
struct pw_node_info;''', "PipeWire forward declarations")
h = replace_once(h,
'''    std::vector<uint32_t> audio_positions; // SPA channel-position enum in stream channel order.
};''',
'''    std::vector<uint32_t> audio_positions; // SPA channel-position enum in stream channel order.
    bool audio_layout_resolved = false; // Set only after pw_node_info.props is received.
};''', "PipeWireNode layout-ready flag")
h = replace_once(h,
'''    friend void registry_global_remove(void *data, uint32_t id);
    struct MonitorLoopRequest {''',
'''    friend void registry_global_remove(void *data, uint32_t id);
    friend void pipewire_node_info_changed(void *data, const struct pw_node_info *info);
    struct MonitorLoopRequest {''', "node info callback friend")
h = replace_once(h,
'''    struct MonitorLoopRequest {
        DeviceManager* manager = nullptr;
        bool start = false;
        uint32_t node_id = 0;
        std::string name;
        bool is_capture = true;
        unsigned channels = 0;
        std::vector<uint32_t> positions;
    };
    static int invoke_monitor_operation(struct spa_loop*, bool, uint32_t,
                                        const void*, std::size_t, void*);''',
'''    struct MonitorLoopRequest {
        DeviceManager* manager = nullptr;
        bool start = false;
        uint32_t node_id = 0;
        std::string name;
        bool is_capture = true;
        unsigned channels = 0;
        std::vector<uint32_t> positions;
    };
    struct PipeWireNodeInfoWatch {
        DeviceManager* manager = nullptr;
        uint32_t node_id = 0;
        struct pw_node* proxy = nullptr;
        struct spa_hook listener{};
    };
    static int invoke_monitor_operation(struct spa_loop*, bool, uint32_t,
                                        const void*, std::size_t, void*);
    static int invoke_pipewire_cleanup(struct spa_loop*, bool, uint32_t,
                                       const void*, std::size_t, void*);''', "PipeWireNodeInfoWatch declaration")
h = replace_once(h,
'''    struct pw_context* m_pw_context = nullptr;
    struct spa_hook m_registry_listener;''',
'''    struct pw_context* m_pw_context = nullptr;
    struct pw_registry* m_pw_registry = nullptr;
    struct spa_hook m_registry_listener;''', "stored registry proxy")
h = replace_once(h,
'''    std::unordered_map<uint32_t, PipeWireLink> m_pw_links_by_id;

    // pw_loop_invoke requires external callers to be serialized.''',
'''    std::unordered_map<uint32_t, PipeWireLink> m_pw_links_by_id;
    // Node proxy listeners are created, updated, and destroyed on the PipeWire loop.
    std::unordered_map<uint32_t, std::unique_ptr<PipeWireNodeInfoWatch>> m_pw_node_info_watchers;

    // pw_loop_invoke requires external callers to be serialized.''', "node watcher map")

# Source includes and registry ownership.
c = replace_once(c,
'''#include <pipewire/pipewire.h>
#include <pipewire/stream.h>''',
'''#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/node.h>
#include <pipewire/proxy.h>''', "PipeWire node/proxy includes")
c = replace_once(c,
'''                    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
                    if (registry) {
                        spa_zero(m_registry_listener);
                        pw_registry_add_listener(registry, &m_registry_listener, &registry_events, this);
                    }''',
'''                    m_pw_registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
                    if (m_pw_registry) {
                        spa_zero(m_registry_listener);
                        pw_registry_add_listener(m_pw_registry, &m_registry_listener, &registry_events, this);
                    }''', "store PipeWire registry proxy")

# Destructor: remove node listeners and registry listener in the PipeWire loop
# before quitting/destroying that loop.
c = replace_once(c,
'''        if (m_pw_loop) {
            pw_main_loop_quit(m_pw_loop);
            if (m_pw_thread.joinable()) m_pw_thread.join();
            pw_main_loop_destroy(m_pw_loop);
        }''',
'''        if (m_pw_loop) {
            if (m_pw_thread.joinable()) {
                int cleanupResult = -1;
                {
                    std::lock_guard<std::mutex> invokeLock(m_monitorInvokeLock);
                    cleanupResult = pw_loop_invoke(
                        pw_main_loop_get_loop(m_pw_loop), &DeviceManager::invoke_pipewire_cleanup,
                        0, nullptr, 0, true, this);
                }
                if (cleanupResult < 0) {
                    std::cerr << "[DeviceManager] Could not clean up PipeWire node proxies on the loop: "
                              << cleanupResult << std::endl;
                }
                pw_main_loop_quit(m_pw_loop);
                m_pw_thread.join();
            } else {
                invoke_pipewire_cleanup(nullptr, false, 0, nullptr, 0, this);
            }
            pw_main_loop_destroy(m_pw_loop);
        }''', "loop-thread proxy cleanup in destructor")

# Add the cleanup callback immediately after the current monitor-invoke callback.
c = replace_once(c,
'''int DeviceManager::invoke_monitor_operation(
    struct spa_loop*, bool, uint32_t, const void*, std::size_t, void* user_data) {
    auto* request = static_cast<MonitorLoopRequest*>(user_data);
    if (!request || !request->manager) return -1;

    if (request->start) {
        request->manager->start_node_monitor_in_loop(
            request->node_id, request->name, request->is_capture,
            request->channels, request->positions);
    } else {
        request->manager->stop_node_monitor_in_loop(request->node_id);
    }
    return 0;
}
''',
'''int DeviceManager::invoke_monitor_operation(
    struct spa_loop*, bool, uint32_t, const void*, std::size_t, void* user_data) {
    auto* request = static_cast<MonitorLoopRequest*>(user_data);
    if (!request || !request->manager) return -1;

    if (request->start) {
        request->manager->start_node_monitor_in_loop(
            request->node_id, request->name, request->is_capture,
            request->channels, request->positions);
    } else {
        request->manager->stop_node_monitor_in_loop(request->node_id);
    }
    return 0;
}

int DeviceManager::invoke_pipewire_cleanup(
    struct spa_loop*, bool, uint32_t, const void*, std::size_t, void* user_data) {
    auto* manager = static_cast<DeviceManager*>(user_data);
    if (!manager) return -1;

    for (auto& entry : manager->m_pw_node_info_watchers) {
        auto& watch = entry.second;
        spa_hook_remove(&watch->listener);
        if (watch->proxy) {
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(watch->proxy));
            watch->proxy = nullptr;
        }
    }
    manager->m_pw_node_info_watchers.clear();

    if (manager->m_pw_registry) {
        spa_hook_remove(&manager->m_registry_listener);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(manager->m_pw_registry));
        manager->m_pw_registry = nullptr;
    }
    return 0;
}
''', "PipeWire cleanup invoke callback")

# Install the node-info callback/event table before registry_global can refer to it.
node_callback = r'''void pipewire_node_info_changed(void* data, const struct pw_node_info* info) {
    auto* watch = static_cast<DeviceManager::PipeWireNodeInfoWatch*>(data);
    if (!watch || !watch->manager || !info) return;

    DeviceManager* manager = watch->manager;
    const uint32_t nodeId = watch->node_id;
    const PipeWireAudioLayout layout = parse_pipewire_audio_layout(info->props);
    std::string nodeName;
    bool isCapture = true;
    bool wasResolved = false;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(manager->m_pw_graph_lock);
        auto it = manager->m_pw_graph.nodes.find(nodeId);
        if (it == manager->m_pw_graph.nodes.end()) return;

        PipeWireNode& node = it->second;
        wasResolved = node.audio_layout_resolved;
        changed = !wasResolved || node.audio_channels != layout.channels ||
                  node.audio_positions != layout.positions;
        node.audio_channels = layout.channels;
        node.audio_positions = layout.positions;
        node.audio_layout_resolved = true;
        nodeName = node.name;
        isCapture = !node.is_input;
    }

    // pw_node_info.props contains the actual node properties. The registry's
    // initial global dictionary may not include audio.channels/audio.position.
    if (!changed) return;
    if (wasResolved) manager->stop_node_monitor_in_loop(nodeId);
    if (manager->m_dspEngine.load(std::memory_order_acquire) &&
        nodeName.find("cm5-meter") == std::string::npos) {
        manager->start_node_monitor_in_loop(
            nodeId, nodeName, isCapture, layout.channels, layout.positions);
    }
}

static const struct pw_node_events node_info_events = {
    PW_VERSION_NODE_EVENTS,
    .info = pipewire_node_info_changed,
};

'''
c = replace_once(c,
'''void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {''',
node_callback + '''void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {''',
"node info callback/event table")

# Replace the premature stereo monitor creation with a node proxy listener.
old_monitor_registry = '''        // Monitor every audio node, including both sources and sinks. The
        // cm5-meter streams are excluded to prevent recursive self-monitoring.
        const std::string nodeName = name ? name : "";
        const bool isAudio = mediaClass.find("Audio") != std::string::npos;
        if (isAudio &&
            manager->m_dspEngine.load(std::memory_order_acquire) &&
            nodeName.find("cm5-meter") == std::string::npos) {
            manager->start_node_monitor_in_loop(
                id, nodeName, !is_input, audioLayout.channels, audioLayout.positions);
        }'''
new_monitor_registry = '''        // Bind the actual node and wait for pw_node_info.props before creating
        // its meter. Registry global props do not reliably include audio layout.
        const std::string nodeName = name ? name : "";
        const bool isAudio = mediaClass.find("Audio") != std::string::npos;
        if (isAudio && nodeName.find("cm5-meter") == std::string::npos) {
            auto fallback_to_registry_layout = [&]() {
                auto nodeIt = manager->m_pw_graph.nodes.find(id);
                if (nodeIt != manager->m_pw_graph.nodes.end())
                    nodeIt->second.audio_layout_resolved = true;
                if (manager->m_dspEngine.load(std::memory_order_acquire)) {
                    manager->start_node_monitor_in_loop(
                        id, nodeName, !is_input, audioLayout.channels, audioLayout.positions);
                }
            };

            if (!manager->m_pw_registry) {
                fallback_to_registry_layout();
            } else if (manager->m_pw_node_info_watchers.find(id) ==
                       manager->m_pw_node_info_watchers.end()) {
                auto watch = std::make_unique<DeviceManager::PipeWireNodeInfoWatch>();
                watch->manager = manager;
                watch->node_id = id;
                const uint32_t bindVersion = std::min<uint32_t>(version, PW_VERSION_NODE);
                watch->proxy = static_cast<struct pw_node*>(pw_registry_bind(
                    manager->m_pw_registry, id, PW_TYPE_INTERFACE_Node, bindVersion, 0));

                if (!watch->proxy) {
                    std::cerr << "[PipeWire Monitor] Could not bind node info for "
                              << nodeName << " (" << id << "); using fallback layout" << std::endl;
                    fallback_to_registry_layout();
                } else {
                    auto* watchPtr = watch.get();
                    manager->m_pw_node_info_watchers.emplace(id, std::move(watch));
                    spa_zero(watchPtr->listener);
                    const int listenerResult = pw_node_add_listener(
                        watchPtr->proxy, &watchPtr->listener, &node_info_events, watchPtr);
                    if (listenerResult < 0) {
                        std::cerr << "[PipeWire Monitor] Could not listen for node info for "
                                  << nodeName << " (" << id << "): " << listenerResult
                                  << "; using fallback layout" << std::endl;
                        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(watchPtr->proxy));
                        watchPtr->proxy = nullptr;
                        manager->m_pw_node_info_watchers.erase(id);
                        fallback_to_registry_layout();
                    }
                }
            }
        }'''
c = replace_once(c, old_monitor_registry, new_monitor_registry, "defer monitor creation until node info")

# Node-global removal callbacks run on the PipeWire loop. Existing revisions of
# this source differ in whether graph cleanup is immediately preceded by a lock,
# an ignored-node check, or a monitor stop. Anchor at the function prologue and
# insert proxy cleanup there, before any graph lock or early return.
remove_prefix = """void registry_global_remove(void *data, uint32_t id) {
    auto* manager = static_cast<DeviceManager*>(data);
"""
watch_cleanup = """
    auto watchIt = manager->m_pw_node_info_watchers.find(id);
    if (watchIt != manager->m_pw_node_info_watchers.end()) {
        auto& watch = watchIt->second;
        spa_hook_remove(&watch->listener);
        if (watch->proxy) {
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(watch->proxy));
            watch->proxy = nullptr;
        }
        manager->m_pw_node_info_watchers.erase(watchIt);
    }
"""
if c.count(remove_prefix) != 1:
    raise RuntimeError(
        "Expected exactly one registry_global_remove prologue; found "
        f"{c.count(remove_prefix)}; no files have been changed.")
if "auto watchIt = manager->m_pw_node_info_watchers.find(id);" in c:
    raise RuntimeError(
        "Node watcher cleanup already appears in source; refusing to duplicate it; "
        "no files have been changed.")
c = c.replace(remove_prefix, remove_prefix + watch_cleanup, 1)

# Reconciliation only starts monitors after a node info event resolved its layout.
c = replace_once(c,
'''        if (node.media_class.find("Audio") == std::string::npos ||
            node.name.find("cm5-meter") != std::string::npos) continue;''',
'''        if (node.media_class.find("Audio") == std::string::npos ||
            node.name.find("cm5-meter") != std::string::npos ||
            !node.audio_layout_resolved) continue;''', "skip unresolved layouts during engine reconciliation")

# Atomic backup + writes only after every anchor above has matched.
stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
for path, original, updated in ((header_path,h0,h),(source_path,c0,c)):
    backup = path.with_name(path.name + ".before-node-info-v8-" + stamp)
    backup.write_text(original)
    tmp = path.with_suffix(path.suffix + ".v8tmp")
    tmp.write_text(updated)
    os.replace(tmp, path)
print("Applied PipeWire node-info channel-layout fix v8.")
print(f"Backups: include/device_manager.h.before-node-info-v8-{stamp}")
print(f"         src/device_manager.cpp.before-node-info-v8-{stamp}")
print("Next: make -j7")
