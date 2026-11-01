#include "midi_cc_controller.h"

#include "web_server.h"
#include "dsp_engine.h"
#include "processor_registry.h"
#include "midi_device_selector.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/control/control.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

constexpr const char* MANUAL_MIDI_SOURCE = "manual";

const char* controllerPage() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CM5Audio MIDI CC Controller</title>
<style>
:root{color-scheme:dark;font:15px system-ui,sans-serif;background:#12151b;color:#e8edf4}
body{margin:0;padding:24px;max-width:1180px;margin-inline:auto}
h1{font-size:1.55rem;margin:0 0 8px}h2{font-size:1.1rem;margin:0 0 12px}
p{color:#aab5c4;margin:6px 0 18px}.panel{background:#1b222c;border:1px solid #303b49;border-radius:10px;padding:16px;margin:16px 0}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}.status{color:#a9d5a9}.muted{color:#9aa6b5}
select,button{background:#263241;color:#e8edf4;border:1px solid #465568;border-radius:6px;padding:8px 10px;font:inherit}
button{cursor:pointer}button:hover{background:#34465b}button.primary{background:#245c87;border-color:#397cae}
button.danger{background:#632f35;border-color:#8c454d}
table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:9px 8px;border-bottom:1px solid #303b49;vertical-align:middle}
th{color:#b9c5d3;font-weight:600}td small{display:block;color:#9aa6b5}
.parameter-list{max-height:440px;overflow:auto}.right{text-align:right}.empty{color:#9aa6b5;padding:14px 0}
.live-value{font-variant-numeric:tabular-nums;font-weight:650;color:#d8f0d4;white-space:nowrap}
.event-ok{color:#a9d5a9}.event-neutral{color:#c3cbd5}.event-error{color:#f0a4a4}.event-cell{font-variant-numeric:tabular-nums;min-width:170px}
input[type=checkbox]{width:17px;height:17px;vertical-align:middle}
input[type=number]{background:#263241;color:#e8edf4;border:1px solid #465568;border-radius:6px;padding:7px 8px;font:inherit}
@media(max-width:700px){body{padding:14px}table{font-size:13px}th,td{padding:7px 4px}}
</style>
</head>
<body>
<h1>CM5Audio MIDI CC Controller</h1>
<p>Learn MIDI Control Change messages and assign them to live processor parameters. ABS maps the full 0–127 position range; REL applies encoder movement to the current value. For the MPK mini Plus, raw 1 = increment and raw 127 = decrement. REL multiplier scales each movement relative to the target parameter range. Mappings are saved separately from the main audio settings.</p>
<section class="panel">
<h2>MIDI Input</h2>
<div class="row">
<label for="midi-source">Input source</label>
<select id="midi-source"></select>
<button class="primary" id="apply-source">Apply input</button>
<span id="midi-status" class="status">Starting…</span>
</div>
<p class="muted">Choose an available PipeWire MIDI output, or select Manual / QPWGraph and connect a MIDI source to the controller's midi_in port yourself.</p>
<div id="last-cc" class="muted">Waiting for MIDI CC…</div>
<div id="cc-diagnostic" class="muted">No incoming MIDI CC data yet.</div>
</section>
<section class="panel">
<h2>Learn a Parameter</h2>
<p class="muted">Press Learn, then move a knob, fader, or other MIDI CC control. The first CC received is assigned to that parameter.</p>
<div class="parameter-list">
<table><thead><tr><th>Processor / parameter</th><th>Target range</th><th>Current value</th><th>Control</th><th class="right">Action</th></tr></thead>
<tbody id="parameters"><tr><td colspan="5" class="empty">Loading parameters…</td></tr></tbody></table>
</div>
</section>
<section class="panel">
<h2>Mappings</h2>
<div id="learning" class="muted">No learn operation pending.</div>
<table><thead><tr><th>Controller</th><th>Destination</th><th>Channel</th><th>Mode</th><th>Relative format</th><th>REL multiplier</th><th>ABS curve</th><th>Invert</th><th>Live value</th><th>Last input / result</th><th></th></tr></thead>
<tbody id="mappings"><tr><td colspan="11" class="empty">No mappings yet.</td></tr></tbody></table>
</section>
<script>
const $=id=>document.getElementById(id);
const esc=value=>String(value??'').replace(/[&<>"']/g,ch=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
async function api(url,options={}){const response=await fetch(url,{cache:'no-store',...options,headers:{'Content-Type':'application/json',...(options.headers||{})}});const body=await response.json();if(!response.ok)throw new Error(body.error||body.message||`HTTP ${response.status}`);return body;}
function channelOptions(selected){let html=`<option value="0"${selected===0?' selected':''}>All channels</option>`;for(let n=1;n<=16;n++)html+=`<option value="${n}"${selected===n?' selected':''}>Channel ${n}</option>`;return html;}
let sourceDirty=false;
let renderedParameters='';
let renderedMappings='';
$('midi-source').addEventListener('change',()=>{sourceDirty=true;});
function formatValue(value,unit=''){
  if(value===null||value===undefined||!Number.isFinite(Number(value)))return '—';
  const text=Number(value).toLocaleString(undefined,{maximumFractionDigits:5});
  return unit?`${text} ${unit}`:text;
}
function render(state){
  const source=$('midi-source');const active=state.source||'manual';
  if(source.dataset.loaded!=='1'){source.innerHTML=(state.sources||[]).map(item=>`<option value="${esc(item.value)}">${esc(item.label)}</option>`).join('');source.dataset.loaded='1';}
  // Never overwrite a locally selected value until the user explicitly applies it.
  // Also avoid assigning .value during focus; either behavior can reset an open menu.
  if(!sourceDirty && document.activeElement!==source && source.value!==active)source.value=active;
  const midiStatus=state.connected?'MIDI input connected':(state.filter_state>=0?`PipeWire state ${state.filter_state}`:'MIDI input unavailable');
  if($('midi-status').textContent!==midiStatus)$('midi-status').textContent=midiStatus;
  const lastCc=state.last_cc
    ?`Last CC: CC ${state.last_cc.cc} · raw ${state.last_cc.value} (0x${Number(state.last_cc.value).toString(16).padStart(2,'0').toUpperCase()}) · channel ${state.last_cc.channel} · ${state.last_cc.matched_mappings||0} mapping(s) matched`
    :'Waiting for MIDI CC…';
  if($('last-cc').textContent!==lastCc)$('last-cc').textContent=lastCc;
  const diagnostic=state.last_cc
    ?`Received ${Number(state.cc_events||0).toLocaleString()} CC event(s). Raw value is shown before any relative decoding. A REL mapping should show a non-zero decoded Δ when its encoder moves.`
    :'No incoming MIDI CC data yet. Rotate a knob and confirm this counter changes.';
  if($('cc-diagnostic').textContent!==diagnostic)$('cc-diagnostic').textContent=diagnostic;

  const parameters=state.parameters||[];
  const parameterHtml=parameters.length?parameters.map(item=>{
    const kind=item.target_kind||'plugin';
    const suffix=kind==='route'?` · route ${item.route_id}, stage ${Number(item.stage_index)+1}`:` · #${item.plugin_id}`;
    const targetAttrs=kind==='route'
      ? `data-target-kind="route" data-route="${item.route_id}" data-stage="${item.stage_index}"`
      : `data-target-kind="plugin" data-plugin="${item.plugin_id}"`;
    const range=item.boolean?'Off / On (threshold 64)':`${esc(item.min)} to ${esc(item.max)}${item.unit?' '+esc(item.unit):''}`;
    const control=item.boolean?'Switch':`${esc(item.unit||'Numeric')} · 7-bit ABS / REL`;
    return `<tr><td><strong>${esc(item.plugin_name)}</strong><small>${esc(item.name)} · ${esc(item.parameter_id)}${suffix}</small></td><td>${range}</td><td><span class="live-value" data-parameter-value="${esc(item.key)}">—</span></td><td>${control}</td><td class="right"><button class="primary" data-learn="1" ${targetAttrs} data-type="${esc(item.plugin_type)}" data-param="${esc(item.parameter_id)}">Learn CC</button></td></tr>`;
  }).join(''):'<tr><td colspan="5" class="empty">No live-controllable parameters are currently available. Add a supported processor or a route DSP chain.</td></tr>';
  if(parameterHtml!==renderedParameters){$('parameters').innerHTML=parameterHtml;renderedParameters=parameterHtml;}
  // Update values independently so live changes never rebuild interactive controls.
  for(const item of parameters){
    const key=item.key;
    const cell=document.querySelector(`[data-parameter-value="${CSS.escape(key)}"]`);
    if(cell)cell.textContent=item.boolean?(item.current>=0.5?'On':'Off'):formatValue(item.current,item.unit);
  }

  const learning=state.learning?`Listening for the next CC for ${state.learning.plugin_name} → ${state.learning.name}. Move the desired controller now.`:'No learn operation pending.';
  if($('learning').textContent!==learning)$('learning').textContent=learning;
  const mappings=state.mappings||[];
  const mappingHtml=mappings.length?mappings.map(item=>`<tr>
    <td><strong>CC ${item.cc}</strong><small>${item.channel===0?'Any channel':`Channel ${item.channel}`} · ${item.mode==='relative'?'REL':'ABS'}</small></td>
    <td>${esc(item.plugin_name)}<small>${esc(item.parameter_name)}${item.boolean?' · switch':''}</small></td>
    <td><select data-channel="${item.id}">${channelOptions(item.channel)}</select></td>
    <td><select data-mode="${item.id}"><option value="absolute"${item.mode==='absolute'?' selected':''}>ABS</option><option value="relative"${item.mode==='relative'?' selected':''}>REL</option></select></td>
    <td><select data-encoding="${item.id}">
      <option value="inc-dec"${item.relative_encoding==='inc-dec'?' selected':''}>MPK mini Plus: 1 = INC / 127 = DEC</option>
      <option value="two's-complement"${item.relative_encoding==="two's-complement"?' selected':''}>Signed 7-bit: 1–63 INC / 65–127 DEC</option>
      <option value="sign-magnitude"${item.relative_encoding==='sign-magnitude'?' selected':''}>Sign / magnitude</option>
      <option value="binary-offset"${item.relative_encoding==='binary-offset'?' selected':''}>Binary offset (64 = neutral)</option>
    </select><small>Check decoded Δ below while turning the knob.</small></td>
    <td><input type="number" min="0.001" max="1000" step="0.05" data-relmult="${item.id}" value="${esc(item.relative_multiplier)}" style="width:88px"${item.mode==='relative'&&!item.boolean?'':' disabled'}> ×
      <small>1 tick = ${formatValue(item.relative_step,item.unit)}</small></td>
    <td><select data-curve="${item.id}">
      <option value="linear"${item.curve==='linear'?' selected':''}>Linear</option>
      <option value="log"${item.curve==='log'?' selected':''}>Logarithmic</option>
      <option value="audio"${item.curve==='audio'?' selected':''}>Power taper</option>
      <option value="bipolar"${item.curve==='bipolar'?' selected':''}>Bipolar</option>
    </select></td>
    <td><input type="checkbox" data-invert="${item.id}"${item.invert?' checked':''}></td>
    <td class="live-value"><span data-live-value="${item.id}">—</span><small>Range ${formatValue(item.min,item.unit)} – ${formatValue(item.max,item.unit)}</small></td>
    <td class="event-cell"><span data-last-event="${item.id}" class="event-neutral">Waiting for this CC</span></td>
    <td><button class="danger" data-delete="${item.id}">Remove</button></td></tr>`).join(''):'<tr><td colspan="11" class="empty">No CC mappings have been learned yet.</td></tr>';
  if(mappingHtml!==renderedMappings){$('mappings').innerHTML=mappingHtml;renderedMappings=mappingHtml;}
  // Live-value spans are updated without replacing the mapping row DOM. This keeps
  // selects, checkboxes, and number inputs stable while a knob is being turned.
  for(const item of mappings){
    const live=document.querySelector(`[data-live-value="${item.id}"]`);
    if(live)live.textContent=item.boolean?(item.current_value>=0.5?'On':'Off'):formatValue(item.current_value,item.unit);
    const eventCell=document.querySelector(`[data-last-event="${item.id}"]`);
    if(eventCell){
      let message='Waiting for this CC';
      let className='event-neutral';
      if(item.has_event){
        const raw=Number(item.last_raw);
        if(item.mode==='relative'){
          const delta=Number(item.last_delta||0);
          if(delta===0){message=`raw ${raw} → Δ 0 (neutral)`;className='event-neutral';}
          else if(item.last_apply_ok){
            const effectiveStep=Math.abs(delta)*Number(item.relative_step||0);
            message=`raw ${raw} → Δ ${delta>0?'+':''}${delta} · step ${formatValue(effectiveStep,item.unit)} · set ${formatValue(item.last_applied_value,item.unit)}`;
            className='event-ok';
          }
          else {message=`raw ${raw} → Δ ${delta>0?'+':''}${delta} · APPLY FAILED`;className='event-error';}
        }else if(item.last_apply_ok){
          message=`raw ${raw} · set ${formatValue(item.last_applied_value,item.unit)}`;className='event-ok';
        }else{message=`raw ${raw} · APPLY FAILED`;className='event-error';}
      }
      if(eventCell.textContent!==message)eventCell.textContent=message;
      eventCell.className=`event-cell ${className}`;
    }
  }
}
async function refresh(){try{render(await api('/api/state'));}catch(error){$('midi-status').textContent=error.message;}}
$('apply-source').addEventListener('click',async()=>{try{const result=await api('/api/source',{method:'POST',body:JSON.stringify({source:$('midi-source').value})});sourceDirty=false;$('midi-status').textContent=result.input_started?'Input source applied':'Saved, but PipeWire input did not start';await refresh();}catch(error){alert(error.message);}});
$('parameters').addEventListener('click',async event=>{const button=event.target.closest('[data-learn]');if(!button)return;try{const body={target_kind:button.dataset.targetKind,plugin_type:button.dataset.type,parameter_id:button.dataset.param};if(body.target_kind==='route'){body.route_id=Number(button.dataset.route);body.stage_index=Number(button.dataset.stage);}else body.plugin_id=Number(button.dataset.plugin);await api('/api/learn',{method:'POST',body:JSON.stringify(body)});await refresh();}catch(error){alert(error.message);}});
$('mappings').addEventListener('change',async event=>{
  const control=event.target.closest('[data-channel],[data-invert],[data-mode],[data-encoding],[data-relmult],[data-curve]');
  if(!control)return;
  const row=event.target.closest('tr');
  const id=Number(control.dataset.channel||control.dataset.invert||control.dataset.mode||control.dataset.encoding||control.dataset.relmult||control.dataset.curve);
  const body={
    channel:Number(row.querySelector('[data-channel]').value),
    invert:row.querySelector('[data-invert]').checked,
    mode:row.querySelector('[data-mode]').value,
    relative_encoding:row.querySelector('[data-encoding]').value,
    relative_multiplier:Number(row.querySelector('[data-relmult]').value),
    curve:row.querySelector('[data-curve]').value
  };
  try{await api(`/api/mappings/${id}/update`,{method:'POST',body:JSON.stringify(body)});}
  catch(error){alert(error.message);await refresh();}
});
$('mappings').addEventListener('click',async event=>{const button=event.target.closest('[data-delete]');if(!button)return;try{await api(`/api/mappings/${Number(button.dataset.delete)}`,{method:'DELETE'});await refresh();}catch(error){alert(error.message);}});
refresh();setInterval(refresh,500);
</script>
</body>
</html>)HTML";
}

struct ParameterTarget {
    bool routeTarget = false;
    uint32_t pluginId = 0;
    uint32_t routeId = 0;
    unsigned stageIndex = 0;
    std::string parameterId;
    std::string expectedPluginType;
};

struct ParameterInfo {
    ParameterTarget target;
    std::string pluginName;
    std::string pluginType;
    std::string name;
    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.0;
    bool integer = false;
    bool boolean = false;
    std::string unit;
    double currentValue = 0.0;
};

struct Mapping {
    uint32_t id = 0;
    ParameterTarget target;
    unsigned cc = 0;
    unsigned channel = 0; // 0 = omni, 1..16 = MIDI channel.
    bool invert = false;
    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.0; // Descriptor UI step; not used to quantize CC resolution.
    bool integer = false;
    bool boolean = false;
    std::string mode = "absolute"; // absolute or relative
    std::string curve = "linear"; // linear, log, audio, bipolar
    std::string relativeEncoding = "inc-dec";
    double relativeStep = 1.0 / 127.0; // Base step: full parameter range / 127 (integer params use whole steps).
    double relativeMultiplier = 1.0;   // User-adjustable sensitivity multiplier.
    double relativeRemainder = 0.0;    // Retains sub-unit movement for integer parameters.
    double currentValue = 0.0; // Runtime value used by relative encoders.
    bool hasEvent = false;
    unsigned lastRawValue = 0;
    int lastDelta = 0;
    double lastAppliedValue = 0.0;
    bool lastApplySucceeded = false;
    uint64_t eventCount = 0;
};

static bool sameTarget(const ParameterTarget& a, const ParameterTarget& b)
{
    return a.routeTarget == b.routeTarget &&
           (a.routeTarget
                ? (a.routeId == b.routeId && a.stageIndex == b.stageIndex)
                : a.pluginId == b.pluginId) &&
           a.parameterId == b.parameterId &&
           (a.expectedPluginType.empty() || b.expectedPluginType.empty() ||
            a.expectedPluginType == b.expectedPluginType);
}

static std::optional<ParameterInfo> findParameter(ClientManager& clients, DspEngine& dsp,
                                                   const ParameterTarget& target)
{
    if (target.parameterId.empty()) return std::nullopt;

    if (target.routeTarget) {
        if (!dsp.supports_route_dsp(target.routeId)) return std::nullopt;
        const auto targets = dsp.route_dsp_parameter_targets(target.routeId);
        for (const auto& item : targets) {
            if (item.value("stage_index", UINT_MAX) != target.stageIndex ||
                item.value("parameter_id", std::string()) != target.parameterId ||
                (!target.expectedPluginType.empty() &&
                 item.value("plugin_type", std::string()) != target.expectedPluginType))
                continue;
            ParameterInfo info;
            info.target = target;
            const auto routes = clients.get_routes();
            const auto route = std::find_if(routes.begin(), routes.end(), [&](const AudioRoute& r) {
                return r.id == target.routeId;
            });
            std::string routeLabel = "Route " + std::to_string(target.routeId);
            if (route != routes.end()) {
                const std::string source = clients.get_endpoint_from_id(route->source_node);
                const std::string destination = clients.get_endpoint_from_id(route->destination_node);
                if (!source.empty() || !destination.empty())
                    routeLabel += ": " + source + " → " + destination;
            }
            info.pluginName = routeLabel + " / Stage " + std::to_string(target.stageIndex + 1) +
                              " / " + item.value("plugin_name", std::string("Processor"));
            info.pluginType = item.value("plugin_type", std::string());
            info.name = item.value("name", target.parameterId);
            info.minimum = item.value("min", 0.0);
            info.maximum = item.value("max", 1.0);
            info.step = item.value("step", 0.0);
            info.integer = item.value("integer", false);
            info.unit = item.value("unit", std::string());
            info.currentValue = item.value("current", (info.minimum + info.maximum) * 0.5);
            return info;
        }
        return std::nullopt;
    }

    const auto nodes = clients.get_plugin_nodes();
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
        return item && item->id == target.pluginId;
    });
    if (found == nodes.end()) return std::nullopt;
    const auto& plugin = **found;
    if (!target.expectedPluginType.empty() && plugin.type != target.expectedPluginType)
        return std::nullopt;

    if (plugin.type == "audio_file_player" && plugin.audio_player_state) {
        const auto& state = plugin.audio_player_state;
        ParameterInfo info;
        info.target = target;
        info.pluginName = plugin.name;
        info.pluginType = plugin.type;
        info.minimum = 0.0;
        info.maximum = 1.0;
        info.step = 1.0;
        if (target.parameterId == "gain") {
            info.name = "Playback Gain"; info.minimum = 0.0; info.maximum = 8.0;
            info.step = 0.01; info.unit = "linear";
            info.currentValue = state->gain.load(std::memory_order_relaxed);
        } else if (target.parameterId == "loop") {
            info.name = "Loop Playback"; info.boolean = true;
            info.currentValue = state->loop.load(std::memory_order_relaxed) ? 1.0 : 0.0;
        } else if (target.parameterId == "playing") {
            info.name = "Play / Pause"; info.boolean = true;
            info.currentValue = state->playing.load(std::memory_order_relaxed) ? 1.0 : 0.0;
        } else if (target.parameterId == "position") {
            info.name = "Playback Position"; info.minimum = 0.0; info.maximum = 100.0;
            info.step = 0.1; info.unit = "%";
            info.currentValue = state->frames
                ? 100.0 * static_cast<double>(state->position_frames.load(std::memory_order_relaxed)) /
                    static_cast<double>(state->frames) : 0.0;
        } else return std::nullopt;
        return info;
    }

    const auto* descriptor = ProcessorRegistry::instance().find(plugin.type);
    if (!descriptor || !plugin.processor) return std::nullopt;
    const auto parameter = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
        [&](const ProcessorParameter& item) { return item.id == target.parameterId; });
    if (parameter == descriptor->parameters.end() ||
        (parameter->type != "number" && parameter->type != "integer") ||
        !parameter->min || !parameter->max ||
        !plugin.processor->supports_parameter_control(parameter->id))
        return std::nullopt;
    float current = 0.0f;
    if (!plugin.processor->get_parameter_control(parameter->id, current)) return std::nullopt;

    ParameterInfo info;
    info.target = target;
    info.pluginName = plugin.name;
    info.pluginType = plugin.type;
    info.name = parameter->name;
    info.minimum = *parameter->min;
    info.maximum = *parameter->max;
    info.step = parameter->step.value_or(0.0);
    info.integer = parameter->type == "integer";
    info.unit = parameter->unit;
    info.currentValue = current;
    return info;
}

static bool applyParameter(ClientManager& clients, DspEngine& dsp,
                           const Mapping& mapping, double value)
{
    const float v = static_cast<float>(value);
    if (mapping.target.routeTarget) {
        return dsp.set_route_dsp_parameter(mapping.target.routeId,
            mapping.target.stageIndex, mapping.target.parameterId, v,
            mapping.target.expectedPluginType);
    }

    const auto nodes = clients.get_plugin_nodes();
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
        return item && item->id == mapping.target.pluginId;
    });
    if (found == nodes.end()) return false;
    const auto& plugin = **found;
    if (!mapping.target.expectedPluginType.empty() &&
        plugin.type != mapping.target.expectedPluginType)
        return false;

    if (plugin.type == "audio_file_player" && plugin.audio_player_state) {
        const auto& state = plugin.audio_player_state;
        if (mapping.target.parameterId == "gain") {
            state->gain.store(v, std::memory_order_relaxed); return true;
        }
        if (mapping.target.parameterId == "loop") {
            state->loop.store(value >= 0.5, std::memory_order_relaxed); return true;
        }
        if (mapping.target.parameterId == "playing") {
            const bool play = value >= 0.5;
            if (play && state->position_frames.load(std::memory_order_relaxed) >= state->frames)
                state->seek_request_frames.store(0, std::memory_order_release);
            state->playing.store(play, std::memory_order_release); return true;
        }
        if (mapping.target.parameterId == "position") {
            const double percent = std::clamp(value, 0.0, 100.0);
            const uint64_t frame = static_cast<uint64_t>(std::llround(
                percent * static_cast<double>(state->frames) / 100.0));
            state->seek_request_frames.store(std::min(frame, state->frames), std::memory_order_release);
            return true;
        }
        return false;
    }

    return plugin.processor &&
        plugin.processor->set_parameter_control(mapping.target.parameterId, v);
}

static double quantizeValue(const Mapping& mapping, double value)
{
    value = std::clamp(value, mapping.minimum, mapping.maximum);
    if (mapping.integer) value = std::round(value);
    return std::clamp(value, mapping.minimum, mapping.maximum);
}

static double mapAbsoluteValue(const Mapping& mapping, unsigned ccValue)
{
    if (mapping.boolean) {
        bool enabled = ccValue >= 64u;
        if (mapping.invert) enabled = !enabled;
        return enabled ? 1.0 : 0.0;
    }

    const double rawPosition = static_cast<double>(std::min(ccValue, 127u));
    const double range = mapping.maximum - mapping.minimum;
    double value = mapping.minimum;

    if (mapping.curve == "bipolar" && mapping.minimum < 0.0 && mapping.maximum > 0.0) {
        // CC 64 is an exact neutral point, including when direction is inverted.
        if (rawPosition < 64.0) {
            const double amount = (64.0 - rawPosition) / 64.0;
            value = (mapping.invert ? mapping.maximum : mapping.minimum) * amount;
        } else if (rawPosition > 64.0) {
            const double amount = (rawPosition - 64.0) / 63.0;
            value = (mapping.invert ? mapping.minimum : mapping.maximum) * amount;
        }
    } else {
        const double position = mapping.invert ? 127.0 - rawPosition : rawPosition;
        const double x = position / 127.0;
        value = mapping.minimum + x * range;
        if (mapping.curve == "log" && mapping.minimum > 0.0 && mapping.maximum > 0.0) {
            value = mapping.minimum * std::pow(mapping.maximum / mapping.minimum, x);
        } else if (mapping.curve == "audio") {
            value = mapping.minimum + x * x * range;
        }
    }
    return quantizeValue(mapping, value);
}

static double relativeBaseStep(double minimum, double maximum, bool integer, bool boolean)
{
    if (boolean) return 1.0;
    const double range = std::max(0.0, maximum - minimum);
    if (integer) return std::max(1.0, std::ceil(range / 127.0));
    const double step = range / 127.0;
    return step > 0.0 && std::isfinite(step) ? step : 0.01;
}

static int decodeRelativeDelta(unsigned value, const std::string& encoding)
{
    value &= 0x7fu;
    if (encoding == "two's-complement") {
        // Common 7-bit relative encoder convention: 1..63 increment,
        // 65..127 decrement, with both 0 and 64 treated as neutral.
        // In particular, 64 must NOT decode as -64.
        if (value == 0u || value == 64u) return 0;
        return value < 64u ? static_cast<int>(value) : static_cast<int>(value) - 128;
    }
    if (encoding == "binary-offset") return static_cast<int>(value) - 64;
    if (encoding == "inc-dec") {
        // Akai MPK mini Plus REL observations: raw 1 means one increment,
        // raw 127 means one decrement. Other values are not movement events.
        if (value == 1u) return 1;
        if (value == 127u) return -1;
        return 0;
    }
    // Sign/magnitude (also called signed bit): bit 6 is direction, lower
    // six bits are magnitude. 0 and 64 are both neutral encodings.
    const int magnitude = static_cast<int>(value & 0x3fu);
    if (magnitude == 0) return 0;
    return (value & 0x40u) ? -magnitude : magnitude;
}

static double mapRelativeValue(Mapping& mapping, unsigned ccValue)
{
    const int delta = decodeRelativeDelta(ccValue, mapping.relativeEncoding);
    if (delta == 0) return mapping.currentValue;
    if (mapping.boolean) {
        const bool current = mapping.currentValue >= 0.5;
        return current ? 0.0 : 1.0;
    }
    const double direction = mapping.invert ? -1.0 : 1.0;
    const double requested = mapping.currentValue + mapping.relativeRemainder +
        direction * static_cast<double>(delta) * mapping.relativeStep * mapping.relativeMultiplier;
    const double bounded = std::clamp(requested, mapping.minimum, mapping.maximum);
    const double quantized = quantizeValue(mapping, bounded);
    // Integer parameters cannot represent fractional values. Carry the unused
    // fraction across encoder ticks instead of throwing it away every event.
    mapping.relativeRemainder = mapping.integer ? bounded - quantized : 0.0;
    return quantized;
}

static std::string defaultCurve(const ParameterInfo& info)
{
    if (info.minimum > 0.0 && (info.unit == "Hz" || info.unit == "ms" ||
        info.name.find("Frequency") != std::string::npos ||
        info.name.find("Freq") != std::string::npos))
        return "log";
    if (info.pluginType == "gain" || info.name == "Playback Gain")
        return "audio";
    return "linear";
}

static size_t umpMessageWords(uint8_t messageType)
{
    switch (messageType & 0x0fu) {
    case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xa: return 2;
    case 0xb: case 0xc: return 3;
    case 0x5: case 0xd: case 0xe: case 0xf: return 4;
    default: return 0;
    }
}

} // namespace

class MidiCcController::Impl {
public:
    Impl(ClientManager& clients, DspEngine& dsp, unsigned httpPort)
        : clients_(clients), dsp_(dsp), httpPort_(httpPort), http_(std::make_unique<httplib::Server>())
    {
        loadConfiguration();
        discoverSources();
        configureHttp();
    }

    ~Impl() { stop(); }

    bool start()
    {
        if (started_.load(std::memory_order_acquire)) return true;
        if (!http_->bind_to_port("0.0.0.0", static_cast<int>(httpPort_))) {
            std::fprintf(stderr, "[MIDI CC] Cannot bind controller HTTP port %u\n", httpPort_);
            return false;
        }

        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { workerLoop(); });
        restartPipewireInput();
        httpThread_ = std::thread([this] { http_->listen_after_bind(); });
        started_.store(true, std::memory_order_release);
        std::fprintf(stderr, "[MIDI CC] Controller page listening on http://0.0.0.0:%u\n",
                     httpPort_);
        return true;
    }

    void stop()
    {
        if (!started_.exchange(false, std::memory_order_acq_rel) &&
            !running_.load(std::memory_order_acquire) && !worker_.joinable() &&
            !httpThread_.joinable() && !pipeThread_.joinable())
            return;

        running_.store(false, std::memory_order_release);
        if (http_) http_->stop();
        if (httpThread_.joinable()) httpThread_.join();
        {
            std::lock_guard<std::mutex> lock(pipeMutex_);
            stopPipewireInputLocked();
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    struct LearnTarget {
        ParameterTarget target;
        std::string pluginName;
        std::string parameterName;
    };

    ClientManager& clients_;
    DspEngine& dsp_;
    unsigned httpPort_;
    std::unique_ptr<httplib::Server> http_;
    std::thread httpThread_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};

    mutable std::mutex configMutex_;
    std::vector<Mapping> mappings_;
    std::optional<LearnTarget> learning_;
    uint32_t nextMappingId_ = 1;
    std::string selectedSource_{MANUAL_MIDI_SOURCE};
    std::vector<std::pair<std::string, std::string>> sources_;
    std::string configPath_;

    MidiEventQueue midiQueue_;
    std::mutex pipeMutex_;
    pw_main_loop* pipeLoop_ = nullptr;
    pw_filter* pipeFilter_ = nullptr;
    void* pipeInputPort_ = nullptr;
    pw_proxy* linkProxy_ = nullptr;
    cm5audio::midi::InputLinkRequest* linkRequest_ = nullptr;
    std::thread pipeThread_;
    std::atomic<int> filterState_{-1};
    std::atomic<uint64_t> ccCount_{0};
    std::atomic<unsigned> lastCc_{0};
    std::atomic<unsigned> lastValue_{0};
    std::atomic<unsigned> lastChannel_{0};
    std::atomic<unsigned> lastMatchedMappings_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;

    void setError(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = error;
    }

    std::string getError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

    void discoverSources()
    {
        sources_.clear();
        try {
            for (const auto& choice : cm5audio::midi::makeOutputChoices()) {
                if (!choice.second.is_string()) continue;
                sources_.emplace_back(choice.first, choice.second.get<std::string>());
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[MIDI CC] MIDI output discovery failed: %s\n", error.what());
        }
        if (std::none_of(sources_.begin(), sources_.end(), [](const auto& item) {
                return item.second == MANUAL_MIDI_SOURCE;
            })) {
            sources_.insert(sources_.begin(), {"Manual / QPWGraph", MANUAL_MIDI_SOURCE});
        }
        const bool currentExists = std::any_of(sources_.begin(), sources_.end(),
            [&](const auto& item) { return item.second == selectedSource_; });
        if (!currentExists) {
            const auto firstDevice = std::find_if(sources_.begin(), sources_.end(),
                [](const auto& item) { return item.second != MANUAL_MIDI_SOURCE; });
            selectedSource_ = firstDevice == sources_.end()
                ? MANUAL_MIDI_SOURCE : firstDevice->second;
        }
    }

    std::filesystem::path settingsPath() const
    {
        const char* configured = std::getenv("CM5AUDIO_MIDI_CC_SETTINGS");
        return configured && *configured
            ? std::filesystem::path(configured)
            : std::filesystem::path("./cm5audio_midi_cc.json");
    }

    void loadConfiguration()
    {
        configPath_ = settingsPath().string();
        std::ifstream file(configPath_, std::ios::binary);
        if (!file) return;
        try {
            const Json root = Json::parse(file);
            if (!root.is_object()) return;
            const std::string savedSource = root.value("source", std::string(MANUAL_MIDI_SOURCE));
            if (!savedSource.empty()) selectedSource_ = savedSource;
            if (!root.contains("mappings") || !root["mappings"].is_array()) return;

            for (const auto& item : root["mappings"]) {
                try {
                    if (!item.is_object()) continue;
                    const uint64_t rawId = item.value("id", uint64_t{0});
                    const unsigned cc = item.value("cc", 128u);
                    const unsigned channel = item.value("channel", 17u);
                    const std::string parameter = item.value("parameter_id", std::string());
                    if (rawId == 0 || rawId > UINT32_MAX || cc > 127 || channel > 16 || parameter.empty())
                        continue;

                    ParameterTarget target;
                    target.routeTarget = item.value("target_kind", std::string("plugin")) == "route";
                    target.parameterId = parameter;
                    target.expectedPluginType = item.value("plugin_type", std::string());
                    if (target.routeTarget) {
                        const uint64_t rawRoute = item.value("route_id", uint64_t{0});
                        const unsigned stage = item.value("stage_index", UINT_MAX);
                        if (rawRoute == 0 || rawRoute > UINT32_MAX || stage >= CM5_MAX_PROCESSORS_PER_ROUTE)
                            continue;
                        target.routeId = static_cast<uint32_t>(rawRoute);
                        target.stageIndex = stage;
                    } else {
                        const uint64_t rawPlugin = item.value("plugin_id", uint64_t{0});
                        if (rawPlugin == 0 || rawPlugin > UINT32_MAX) continue;
                        target.pluginId = static_cast<uint32_t>(rawPlugin);
                    }

                    const auto info = findParameter(clients_, dsp_, target);
                    if (!info) continue;
                    Mapping mapping;
                    mapping.id = static_cast<uint32_t>(rawId);
                    mapping.target = target;
                    mapping.cc = cc;
                    mapping.channel = channel;
                    mapping.invert = item.value("invert", false);
                    mapping.minimum = info->minimum;
                    mapping.maximum = info->maximum;
                    mapping.step = info->step;
                    mapping.integer = info->integer;
                    mapping.boolean = info->boolean;
                    mapping.mode = item.value("mode", std::string("absolute"));
                    if (mapping.mode != "absolute" && mapping.mode != "relative") mapping.mode = "absolute";
                    mapping.curve = item.value("curve", defaultCurve(*info));
                    if (mapping.curve != "linear" && mapping.curve != "log" &&
                        mapping.curve != "audio" && mapping.curve != "bipolar") mapping.curve = "linear";
                    mapping.relativeEncoding = item.value("relative_encoding", std::string("inc-dec"));
                    if (mapping.relativeEncoding != "sign-magnitude" &&
                        mapping.relativeEncoding != "two's-complement" &&
                        mapping.relativeEncoding != "binary-offset" &&
                        mapping.relativeEncoding != "inc-dec")
                        mapping.relativeEncoding = "inc-dec";
                    mapping.relativeStep = relativeBaseStep(info->minimum, info->maximum,
                                                            info->integer, info->boolean);
                    // Version-2 configs stored an absolute per-tick step. Convert it
                    // into a multiplier so their existing sensitivity is preserved.
                    if (item.contains("relative_multiplier") && item["relative_multiplier"].is_number()) {
                        mapping.relativeMultiplier = item["relative_multiplier"].get<double>();
                    } else if (item.contains("relative_step") && item["relative_step"].is_number()) {
                        const double legacyStep = item["relative_step"].get<double>();
                        mapping.relativeMultiplier = legacyStep / mapping.relativeStep;
                    }
                    if (!std::isfinite(mapping.relativeMultiplier) ||
                        mapping.relativeMultiplier < 0.001 || mapping.relativeMultiplier > 1000.0)
                        mapping.relativeMultiplier = 1.0;
                    mapping.currentValue = quantizeValue(mapping, info->currentValue);
                    mappings_.push_back(std::move(mapping));
                    nextMappingId_ = std::max(nextMappingId_, static_cast<uint32_t>(rawId + 1));
                } catch (...) {
                    // Keep valid mappings even if an individual saved target is gone.
                }
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[MIDI CC] Could not parse %s: %s\n",
                         configPath_.c_str(), error.what());
        }
    }

    bool saveConfigurationLocked()
    {
        Json root = {
            {"version", 3},
            {"source", selectedSource_},
            {"mappings", Json::array()}
        };
        for (const auto& mapping : mappings_) {
            Json item = {
                {"id", mapping.id},
                {"target_kind", mapping.target.routeTarget ? "route" : "plugin"},
                {"parameter_id", mapping.target.parameterId},
                {"plugin_type", mapping.target.expectedPluginType},
                {"cc", mapping.cc},
                {"channel", mapping.channel},
                {"invert", mapping.invert},
                {"mode", mapping.mode},
                {"curve", mapping.curve},
                {"relative_encoding", mapping.relativeEncoding},
                {"relative_multiplier", mapping.relativeMultiplier}
            };
            if (mapping.target.routeTarget) {
                item["route_id"] = mapping.target.routeId;
                item["stage_index"] = mapping.target.stageIndex;
            } else {
                item["plugin_id"] = mapping.target.pluginId;
            }
            root["mappings"].push_back(std::move(item));
        }

        const std::string temporary = configPath_ + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                setError("Cannot write MIDI CC settings file: " + temporary);
                return false;
            }
            file << root.dump(2) << '\n';
            file.flush();
            if (!file) {
                setError("Could not flush MIDI CC settings file");
                return false;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, configPath_, error);
        if (error) {
            std::filesystem::remove(temporary);
            setError("Could not replace MIDI CC settings file: " + error.message());
            return false;
        }
        return true;
    }

    void configureHttp()
    {
        http_->Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_content(controllerPage(), "text/html; charset=utf-8");
        });

        http_->Get("/api/state", [this](const httplib::Request&, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_content(stateJson().dump(2), "application/json");
        });

        http_->Post("/api/learn", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const Json body = Json::parse(request.body);
                if (!body.is_object() || !body.contains("target_kind") ||
                    !body["target_kind"].is_string() || !body.contains("parameter_id") ||
                    !body["parameter_id"].is_string())
                    throw std::invalid_argument("target_kind and parameter_id are required");
                ParameterTarget target;
                target.routeTarget = body["target_kind"].get<std::string>() == "route";
                target.parameterId = body["parameter_id"].get<std::string>();
                if (body.contains("plugin_type") && body["plugin_type"].is_string())
                    target.expectedPluginType = body["plugin_type"].get<std::string>();
                if (target.routeTarget) {
                    if (!body.contains("route_id") || !body["route_id"].is_number_integer() ||
                        !body.contains("stage_index") || !body["stage_index"].is_number_integer())
                        throw std::invalid_argument("route_id and stage_index are required for route targets");
                    const int64_t rawRoute = body["route_id"].get<int64_t>();
                    const int64_t rawStage = body["stage_index"].get<int64_t>();
                    if (rawRoute <= 0 || rawRoute > UINT32_MAX || rawStage < 0 ||
                        rawStage >= CM5_MAX_PROCESSORS_PER_ROUTE)
                        throw std::invalid_argument("Invalid route or chain stage");
                    target.routeId = static_cast<uint32_t>(rawRoute);
                    target.stageIndex = static_cast<unsigned>(rawStage);
                } else {
                    if (!body.contains("plugin_id") || !body["plugin_id"].is_number_integer())
                        throw std::invalid_argument("plugin_id is required for plugin targets");
                    const int64_t rawPlugin = body["plugin_id"].get<int64_t>();
                    if (rawPlugin <= 0 || rawPlugin > UINT32_MAX)
                        throw std::invalid_argument("Invalid plugin ID");
                    target.pluginId = static_cast<uint32_t>(rawPlugin);
                }
                const auto info = findParameter(clients_, dsp_, target);
                if (!info) throw std::invalid_argument("That parameter is not currently available for live MIDI control");
                {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    learning_ = LearnTarget{target, info->pluginName, info->name};
                }
                response.set_content(Json{{"status", "ok"}, {"learning", true}}.dump(), "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content(Json{{"status", "error"}, {"error", error.what()}}.dump(), "application/json");
            }
        });

        http_->Post("/api/learn/cancel", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard<std::mutex> lock(configMutex_);
            learning_.reset();
            response.set_content(Json{{"status", "ok"}, {"learning", false}}.dump(), "application/json");
        });

        http_->Post("/api/source", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const Json body = Json::parse(request.body);
                if (!body.is_object() || !body.contains("source") || !body["source"].is_string())
                    throw std::invalid_argument("source is required");
                const std::string source = body["source"].get<std::string>();
                if (std::none_of(sources_.begin(), sources_.end(),
                    [&](const auto& item) { return item.second == source; }))
                    throw std::invalid_argument("Selected MIDI source is not available");
                {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    selectedSource_ = source;
                    if (!saveConfigurationLocked()) throw std::runtime_error(getError());
                }
                const bool started = restartPipewireInput();
                response.set_content(Json{{"status", "ok"}, {"input_started", started},
                    {"source", source}, {"error", getError()}}.dump(), "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content(Json{{"status", "error"}, {"error", error.what()}}.dump(), "application/json");
            }
        });

        http_->Post(R"(/api/mappings/(\d+)/update)", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const uint32_t id = static_cast<uint32_t>(std::stoul(request.matches[1].str()));
                const Json body = Json::parse(request.body);
                if (!body.is_object()) throw std::invalid_argument("JSON object required");
                const int channel = body.value("channel", -1);
                const bool invert = body.value("invert", false);
                if (channel < 0 || channel > 16)
                    throw std::invalid_argument("channel must be 0 (all) or 1 through 16");
                const std::string mode = body.value("mode", std::string("absolute"));
                const std::string encoding = body.value("relative_encoding", std::string("inc-dec"));
                const std::string curve = body.value("curve", std::string("linear"));
                const double relativeMultiplier = body.value("relative_multiplier", 1.0);
                if (mode != "absolute" && mode != "relative")
                    throw std::invalid_argument("mode must be absolute or relative");
                if (encoding != "sign-magnitude" && encoding != "two's-complement" &&
                    encoding != "binary-offset" && encoding != "inc-dec")
                    throw std::invalid_argument("Unknown relative CC encoding");
                if (curve != "linear" && curve != "log" && curve != "audio" && curve != "bipolar")
                    throw std::invalid_argument("Unknown absolute scaling curve");
                if (!std::isfinite(relativeMultiplier) || relativeMultiplier < 0.001 || relativeMultiplier > 1000.0)
                    throw std::invalid_argument("relative_multiplier must be between 0.001 and 1000");
                if (!updateMapping(id, static_cast<unsigned>(channel), invert,
                                   mode, encoding, curve, relativeMultiplier))
                    throw std::invalid_argument("Mapping not found or settings could not be saved");
                response.set_content(Json{{"status", "ok"}}.dump(), "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content(Json{{"status", "error"}, {"error", error.what()}}.dump(), "application/json");
            }
        });

        http_->Delete(R"(/api/mappings/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const uint32_t id = static_cast<uint32_t>(std::stoul(request.matches[1].str()));
                std::lock_guard<std::mutex> lock(configMutex_);
                const auto found = std::find_if(mappings_.begin(), mappings_.end(),
                    [id](const Mapping& mapping) { return mapping.id == id; });
                if (found == mappings_.end()) throw std::invalid_argument("Mapping not found");
                mappings_.erase(found);
                if (!saveConfigurationLocked()) throw std::runtime_error(getError());
                response.set_content(Json{{"status", "ok"}, {"deleted", id}}.dump(), "application/json");
            } catch (const std::exception& error) {
                response.status = 404;
                response.set_content(Json{{"status", "error"}, {"error", error.what()}}.dump(), "application/json");
            }
        });
    }

    Json stateJson()
    {
        Json state = Json::object();
        Json sourceList = Json::array();
        Json mappings = Json::array();
        Json parameters = Json::array();
        Json learning = nullptr;
        std::string selected;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            selected = selectedSource_;
            for (const auto& source : sources_)
                sourceList.push_back({{"label", source.first}, {"value", source.second}});
            for (const auto& mapping : mappings_) {
                const auto info = findParameter(clients_, dsp_, mapping.target);
                if (!info) continue;
                Json item = {
                    {"id", mapping.id}, {"target_kind", mapping.target.routeTarget ? "route" : "plugin"},
                    {"parameter_id", mapping.target.parameterId},
                    {"plugin_type", mapping.target.expectedPluginType}, {"cc", mapping.cc},
                    {"channel", mapping.channel}, {"invert", mapping.invert},
                    {"boolean", info->boolean}, {"plugin_name", info->pluginName},
                    {"parameter_name", info->name}, {"mode", mapping.mode},
                    {"curve", mapping.curve}, {"relative_encoding", mapping.relativeEncoding},
                    {"relative_multiplier", mapping.relativeMultiplier},
                    {"relative_step", mapping.relativeStep * mapping.relativeMultiplier},
                    {"min", info->minimum}, {"max", info->maximum},
                    {"unit", info->unit}, {"current_value", info->currentValue},
                    {"has_event", mapping.hasEvent}, {"last_raw", mapping.lastRawValue},
                    {"last_delta", mapping.lastDelta},
                    {"last_applied_value", mapping.lastAppliedValue},
                    {"last_apply_ok", mapping.lastApplySucceeded},
                    {"event_count", mapping.eventCount}
                };
                if (mapping.target.routeTarget) {
                    item["route_id"] = mapping.target.routeId;
                    item["stage_index"] = mapping.target.stageIndex;
                } else item["plugin_id"] = mapping.target.pluginId;
                mappings.push_back(std::move(item));
            }
            if (learning_) {
                learning = {
                    {"target_kind", learning_->target.routeTarget ? "route" : "plugin"},
                    {"plugin_name", learning_->pluginName}, {"name", learning_->parameterName}
                };
            }
        }

        auto appendParameter = [&](const ParameterInfo& info) {
            const std::string key = info.target.routeTarget
                ? ("r" + std::to_string(info.target.routeId) + "_" +
                   std::to_string(info.target.stageIndex) + "_" + info.target.parameterId)
                : ("p" + std::to_string(info.target.pluginId) + "_" + info.target.parameterId);
            Json item = {
                {"key", key},
                {"target_kind", info.target.routeTarget ? "route" : "plugin"},
                {"plugin_name", info.pluginName}, {"plugin_type", info.pluginType},
                {"parameter_id", info.target.parameterId}, {"name", info.name},
                {"min", info.minimum}, {"max", info.maximum}, {"step", info.step},
                {"unit", info.unit}, {"integer", info.integer},
                {"boolean", info.boolean}, {"current", info.currentValue}
            };
            if (info.target.routeTarget) {
                item["route_id"] = info.target.routeId;
                item["stage_index"] = info.target.stageIndex;
            } else item["plugin_id"] = info.target.pluginId;
            parameters.push_back(std::move(item));
        };

        for (const auto& plugin : clients_.get_plugin_nodes()) {
            if (!plugin) continue;
            if (plugin->type == "audio_file_player" && plugin->audio_player_state) {
                for (const std::string id : {"gain", "loop", "playing", "position"}) {
                    ParameterTarget target; target.pluginId = plugin->id; target.parameterId = id;
                    const auto info = findParameter(clients_, dsp_, target);
                    if (info) appendParameter(*info);
                }
                continue;
            }
            const auto* descriptor = ProcessorRegistry::instance().find(plugin->type);
            if (!descriptor || !plugin->processor) continue;
            for (const auto& parameter : descriptor->parameters) {
                ParameterTarget target; target.pluginId = plugin->id; target.parameterId = parameter.id;
                const auto info = findParameter(clients_, dsp_, target);
                if (info) appendParameter(*info);
            }
        }

        // Route DSP chains are separate from graph plugin nodes. Expose each
        // live-safe parameter of each stage as an independent MIDI destination.
        for (const auto& route : clients_.get_routes()) {
            const auto stages = dsp_.route_dsp_parameter_targets(route.id);
            std::string routeLabel = "Route " + std::to_string(route.id);
            const std::string source = clients_.get_endpoint_from_id(route.source_node);
            const std::string destination = clients_.get_endpoint_from_id(route.destination_node);
            if (!source.empty() || !destination.empty())
                routeLabel += ": " + source + " → " + destination;
            for (const auto& stage : stages) {
                ParameterInfo info;
                info.target.routeTarget = true;
                info.target.routeId = route.id;
                info.target.stageIndex = stage.value("stage_index", 0u);
                info.target.parameterId = stage.value("parameter_id", std::string());
                info.pluginType = stage.value("plugin_type", std::string());
                info.target.expectedPluginType = info.pluginType;
                info.pluginName = routeLabel + " / Stage " +
                    std::to_string(info.target.stageIndex + 1) + " / " +
                    stage.value("plugin_name", std::string("Processor"));
                info.name = stage.value("name", info.target.parameterId);
                info.minimum = stage.value("min", 0.0);
                info.maximum = stage.value("max", 1.0);
                info.step = stage.value("step", 0.0);
                info.integer = stage.value("integer", false);
                info.unit = stage.value("unit", std::string());
                info.currentValue = stage.value("current", (info.minimum + info.maximum) * 0.5);
                if (!info.target.parameterId.empty()) appendParameter(info);
            }
        }

        state["source"] = selected;
        state["sources"] = std::move(sourceList);
        state["mappings"] = std::move(mappings);
        state["parameters"] = std::move(parameters);
        state["learning"] = std::move(learning);
        state["connected"] = filterState_.load(std::memory_order_relaxed) == PW_FILTER_STATE_STREAMING;
        state["filter_state"] = filterState_.load(std::memory_order_relaxed);
        state["cc_events"] = ccCount_.load(std::memory_order_relaxed);
        state["last_cc"] = ccCount_.load(std::memory_order_relaxed)
            ? Json{{"cc", lastCc_.load()}, {"value", lastValue_.load()},
                   {"channel", lastChannel_.load()},
                   {"matched_mappings", lastMatchedMappings_.load(std::memory_order_relaxed)}}
            : Json(nullptr);
        state["error"] = getError();
        return state;
    }

    bool restartPipewireInput()
    {
        std::lock_guard<std::mutex> lock(pipeMutex_);
        stopPipewireInputLocked();
        std::string source;
        {
            std::lock_guard<std::mutex> configLock(configMutex_);
            source = selectedSource_;
        }
        try {
            pipeLoop_ = pw_main_loop_new(nullptr);
            if (!pipeLoop_) throw std::runtime_error("pw_main_loop_new failed");

            pw_properties* nodeProperties = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Midi",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Production",
                PW_KEY_APP_NAME, "CM5Audio",
                PW_KEY_NODE_NAME, "cm5audio-midi-cc-controller",
                PW_KEY_NODE_DESCRIPTION, "CM5Audio MIDI CC Controller",
                PW_KEY_NODE_AUTOCONNECT, "false",
                nullptr);
            if (!nodeProperties) throw std::runtime_error("PipeWire node properties allocation failed");

            static const pw_filter_events events = [] {
                pw_filter_events value{};
                value.version = PW_VERSION_FILTER_EVENTS;
                value.process = &Impl::onPipewireProcess;
                value.state_changed = &Impl::onPipewireStateChanged;
                return value;
            }();

            pipeFilter_ = pw_filter_new_simple(
                pw_main_loop_get_loop(pipeLoop_),
                "CM5Audio MIDI CC Controller",
                nodeProperties, &events, this);
            if (!pipeFilter_) throw std::runtime_error("pw_filter_new_simple failed");

            pw_properties* portProperties = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit raw UMP",
                PW_KEY_PORT_NAME, "midi_in",
                nullptr);
            if (!portProperties) throw std::runtime_error("PipeWire MIDI port properties allocation failed");

            pipeInputPort_ = pw_filter_add_port(
                pipeFilter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                sizeof(InputPortData), portProperties, nullptr, 0);
            if (!pipeInputPort_) throw std::runtime_error("pw_filter_add_port failed");

            const int result = pw_filter_connect(pipeFilter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
            if (result < 0) throw std::runtime_error("pw_filter_connect failed: " + std::to_string(result));

            selectedOutputForPipe_ = source;
            filterState_.store(PW_FILTER_STATE_CONNECTING, std::memory_order_relaxed);
            pipeThread_ = std::thread([this] { pw_main_loop_run(pipeLoop_); });
            setError({});
            return true;
        } catch (const std::exception& error) {
            stopPipewireInputLocked();
            filterState_.store(-1, std::memory_order_relaxed);
            setError(error.what());
            std::fprintf(stderr, "[MIDI CC] PipeWire input failed: %s\n", error.what());
            return false;
        }
    }

    struct InputPortData { Impl* owner = nullptr; };

    void stopPipewireInputLocked()
    {
        if (pipeLoop_) pw_main_loop_quit(pipeLoop_);
        if (pipeThread_.joinable()) pipeThread_.join();
        cm5audio::midi::cancelInputLinkRequest(linkRequest_);
        if (pipeFilter_ && linkProxy_)
            cm5audio::midi::destroyInputLink(pipeFilter_, linkProxy_);
        if (pipeFilter_) {
            pw_filter_destroy(pipeFilter_);
            pipeFilter_ = nullptr;
        }
        pipeInputPort_ = nullptr;
        linkProxy_ = nullptr;
        if (pipeLoop_) {
            pw_main_loop_destroy(pipeLoop_);
            pipeLoop_ = nullptr;
        }
        filterState_.store(-1, std::memory_order_relaxed);
    }

    static void onInputLinkCreated(void* userdata, pw_proxy* proxy)
    {
        auto* self = static_cast<Impl*>(userdata);
        if (!self) return;
        self->linkRequest_ = nullptr;
        self->linkProxy_ = proxy;
        if (!proxy)
            std::fprintf(stderr, "[MIDI CC] Automatic MIDI link creation failed for '%s'\n",
                         self->selectedOutputForPipe_.c_str());
    }

    static void onPipewireStateChanged(void* userdata, enum pw_filter_state oldState,
                                       enum pw_filter_state state, const char* error)
    {
        (void)oldState;
        auto* self = static_cast<Impl*>(userdata);
        if (!self) return;
        self->filterState_.store(static_cast<int>(state), std::memory_order_relaxed);
        std::fprintf(stderr, "[MIDI CC] PipeWire filter state %d -> %d\n",
                     static_cast<int>(oldState), static_cast<int>(state));
        if ((state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) &&
            self->selectedOutputForPipe_ != MANUAL_MIDI_SOURCE &&
            !self->linkProxy_ && !self->linkRequest_) {
            self->linkRequest_ = cm5audio::midi::createInputLinkAsync(
                self->pipeFilter_, self->selectedOutputForPipe_, "midi_in",
                &Impl::onInputLinkCreated, self);
            if (!self->linkRequest_)
                std::fprintf(stderr, "[MIDI CC] Could not start automatic MIDI link request\n");
        }
        if (state == PW_FILTER_STATE_ERROR) {
            self->setError(error ? error : "PipeWire MIDI input error");
            std::fprintf(stderr, "[MIDI CC] PipeWire error: %s\n", error ? error : "unknown");
        }
    }

    static void onPipewireProcess(void* userdata, spa_io_position* position)
    {
        (void)position;
        auto* self = static_cast<Impl*>(userdata);
        if (!self || !self->pipeInputPort_) return;
        pw_buffer* buffer = pw_filter_dequeue_buffer(self->pipeInputPort_);
        if (!buffer) return;
        self->processInputBuffer(buffer);
        pw_filter_queue_buffer(self->pipeInputPort_, buffer);
    }

    void processInputBuffer(pw_buffer* buffer) noexcept
    {
        if (!buffer || !buffer->buffer || buffer->buffer->n_datas < 1) return;
        spa_data* data = &buffer->buffer->datas[0];
        if (!data->data || !data->chunk) return;
        const size_t offset = data->chunk->offset;
        const size_t size = data->chunk->size;
        if (offset > data->maxsize || size > data->maxsize - offset ||
            size < sizeof(spa_pod_sequence)) return;

        const auto* base = static_cast<const uint8_t*>(data->data) + offset;
        const auto* sequence = reinterpret_cast<const spa_pod_sequence*>(base);
        if (sequence->pod.type != SPA_TYPE_Sequence ||
            SPA_POD_SIZE(&sequence->pod) > size) return;

        spa_pod_control* control = nullptr;
        SPA_POD_SEQUENCE_FOREACH(sequence, control) {
            if (control->value.type != SPA_TYPE_Bytes) continue;
            const size_t payloadSize = SPA_POD_BODY_SIZE(&control->value);
            const auto* payload = static_cast<const uint8_t*>(SPA_POD_BODY(&control->value));
            if (!payload || payloadSize == 0) continue;

            if (control->type == SPA_CONTROL_Midi) {
                MidiEvent event;
                event.protocol = 1;
                event.size = static_cast<uint8_t>(
                    std::min(payloadSize, static_cast<size_t>(CM5_MIDI_MAX_EVENT_BYTES)));
                std::memcpy(event.data, payload, event.size);
                midiQueue_.push(event);
            } else if (control->type == SPA_CONTROL_UMP &&
                       payloadSize % sizeof(uint32_t) == 0 && payloadSize <= 1024) {
                std::array<uint32_t, 256> words{};
                std::memcpy(words.data(), payload, payloadSize);
                size_t index = 0;
                const size_t count = payloadSize / sizeof(uint32_t);
                while (index < count) {
                    const size_t packetWords = umpMessageWords(
                        static_cast<uint8_t>(words[index] >> 28u));
                    if (packetWords == 0 || packetWords > count - index ||
                        packetWords * sizeof(uint32_t) > CM5_MIDI_MAX_EVENT_BYTES) break;
                    MidiEvent event;
                    event.protocol = 2;
                    event.size = static_cast<uint8_t>(packetWords * sizeof(uint32_t));
                    std::memcpy(event.data, words.data() + index, event.size);
                    midiQueue_.push(event);
                    index += packetWords;
                }
            }
        }
    }

    void workerLoop()
    {
        while (running_.load(std::memory_order_acquire)) {
            MidiEvent event;
            bool received = false;
            while (midiQueue_.pop(event)) {
                received = true;
                processMidiEvent(event);
            }
            if (!received)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void processMidiEvent(const MidiEvent& event)
    {
        unsigned channel = 0;
        unsigned cc = 0;
        unsigned value = 0;
        bool isControlChange = false;

        if (event.protocol == 1 && event.size >= 3) {
            const uint8_t status = event.data[0];
            if ((status & 0xf0u) == 0xb0u) {
                channel = (status & 0x0fu) + 1u;
                cc = event.data[1] & 0x7fu;
                value = event.data[2] & 0x7fu;
                isControlChange = true;
            }
        } else if (event.protocol == 2 && event.size >= 4) {
            uint32_t firstWord = 0;
            std::memcpy(&firstWord, event.data, sizeof(firstWord));
            const unsigned messageType = firstWord >> 28u;
            const unsigned status = (firstWord >> 20u) & 0x0fu;
            channel = ((firstWord >> 16u) & 0x0fu) + 1u;
            if (status == 0x0bu && messageType == 0x2u) {
                cc = (firstWord >> 8u) & 0x7fu;
                value = firstWord & 0x7fu;
                isControlChange = true;
            } else if (status == 0x0bu && messageType == 0x4u && event.size >= 8) {
                uint32_t dataWord = 0;
                std::memcpy(&dataWord, event.data + sizeof(uint32_t), sizeof(dataWord));
                cc = (firstWord >> 8u) & 0x7fu;
                value = static_cast<unsigned>(
                    (static_cast<uint64_t>(dataWord) * 127ull + 0x7fffffffull) / 0xffffffffull);
                isControlChange = true;
            }
        }
        if (!isControlChange) return;

        ccCount_.fetch_add(1, std::memory_order_relaxed);
        lastCc_.store(cc, std::memory_order_relaxed);
        lastValue_.store(value, std::memory_order_relaxed);
        lastChannel_.store(channel, std::memory_order_relaxed);

        std::vector<Mapping> mappings;
        uint32_t justLearnedId = 0;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            if (learning_) {
                const LearnTarget target = *learning_;
                learning_.reset();
                const auto info = findParameter(clients_, dsp_, target.target);
                if (info) {
                    auto existing = std::find_if(mappings_.begin(), mappings_.end(),
                        [&](const Mapping& mapping) { return sameTarget(mapping.target, target.target); });
                    Mapping learned;
                    learned.id = existing == mappings_.end() ? nextMappingId_++ : existing->id;
                    learned.target = target.target;
                    learned.target.expectedPluginType = info->pluginType;
                    learned.cc = cc;
                    learned.channel = channel;
                    learned.minimum = info->minimum;
                    learned.maximum = info->maximum;
                    learned.step = info->step;
                    learned.integer = info->integer;
                    learned.boolean = info->boolean;
                    learned.mode = "absolute";
                    learned.curve = defaultCurve(*info);
                    // MPK mini Plus REL mode observed here sends exactly 1 for
                    // increment and 127 for decrement. Other encodings remain selectable.
                    learned.relativeEncoding = "inc-dec";
                    learned.relativeStep = relativeBaseStep(info->minimum, info->maximum,
                                                             info->integer, info->boolean);
                    learned.relativeMultiplier = 1.0;
                    learned.currentValue = quantizeValue(learned, info->currentValue);
                    justLearnedId = learned.id;
                    if (existing == mappings_.end()) mappings_.push_back(learned);
                    else *existing = learned;
                    saveConfigurationLocked();
                    std::fprintf(stderr, "[MIDI CC] Learned CC %u channel %u -> %s / %s\n",
                        cc, channel, target.pluginName.c_str(), target.parameterName.c_str());
                }
            }
            mappings = mappings_;
        }

        unsigned matchedMappings = 0;
        for (const auto& mapping : mappings) {
            if (mapping.id == justLearnedId || mapping.cc != cc ||
                (mapping.channel != 0 && mapping.channel != channel))
                continue;
            ++matchedMappings;

            Mapping activeMapping = mapping;
            if (activeMapping.mode == "relative") {
                // Rebase on the actual live parameter before applying movement.
                // This keeps the encoder aligned if the value was also changed
                // by the dashboard or another control path.
                const auto info = findParameter(clients_, dsp_, activeMapping.target);
                if (info) {
                    const double liveValue = quantizeValue(activeMapping, info->currentValue);
                    const double tolerance = 1.0e-7 * std::max(1.0, activeMapping.maximum - activeMapping.minimum);
                    if (std::abs(liveValue - activeMapping.currentValue) > tolerance)
                        activeMapping.relativeRemainder = 0.0; // External change: discard stale fractional movement.
                    activeMapping.currentValue = liveValue;
                }
            }
            const int decodedDelta = activeMapping.mode == "relative"
                ? decodeRelativeDelta(value, activeMapping.relativeEncoding) : 0;
            const double mapped = activeMapping.mode == "relative"
                ? mapRelativeValue(activeMapping, value)
                : mapAbsoluteValue(activeMapping, value);
            // Relative encoders send movement, so maintain the target's live
            // position independently of the CC's absolute byte value. Neutral
            // packets are counted and shown but do not need a parameter write.
            const bool neutralRelative = activeMapping.mode == "relative" && decodedDelta == 0;
            const bool applied = neutralRelative || applyParameter(clients_, dsp_, activeMapping, mapped);
            double actualValue = mapped;
            if (applied) {
                const auto after = findParameter(clients_, dsp_, activeMapping.target);
                if (after) actualValue = after->currentValue;
            }
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                const auto found = std::find_if(mappings_.begin(), mappings_.end(),
                    [&](const Mapping& current) { return current.id == mapping.id; });
                if (found != mappings_.end()) {
                    found->hasEvent = true;
                    found->lastRawValue = value;
                    found->lastDelta = decodedDelta;
                    found->lastApplySucceeded = applied;
                    found->lastAppliedValue = actualValue;
                    ++found->eventCount;
                    if (applied) {
                        found->currentValue = actualValue;
                        if (activeMapping.mode == "relative")
                            found->relativeRemainder = activeMapping.relativeRemainder;
                    }
                }
            }
        }
        lastMatchedMappings_.store(matchedMappings, std::memory_order_relaxed);
    }

    bool updateMapping(uint32_t id, unsigned channel, bool invert,
                       const std::string& mode, const std::string& encoding,
                       const std::string& curve, double relativeMultiplier)
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        const auto found = std::find_if(mappings_.begin(), mappings_.end(),
            [id](const Mapping& mapping) { return mapping.id == id; });
        if (found == mappings_.end()) return false;
        found->channel = channel;
        found->invert = invert;
        found->mode = mode;
        found->relativeEncoding = encoding;
        found->curve = curve;
        found->relativeMultiplier = relativeMultiplier;
        found->relativeRemainder = 0.0;
        found->hasEvent = false;
        found->lastRawValue = 0;
        found->lastDelta = 0;
        found->lastAppliedValue = 0.0;
        found->lastApplySucceeded = false;
        found->eventCount = 0;
        const auto info = findParameter(clients_, dsp_, found->target);
        if (info) {
            found->minimum = info->minimum;
            found->maximum = info->maximum;
            found->integer = info->integer;
            found->boolean = info->boolean;
            found->relativeStep = relativeBaseStep(info->minimum, info->maximum,
                                                   info->integer, info->boolean);
            found->currentValue = quantizeValue(*found, info->currentValue);
        }
        return saveConfigurationLocked();
    }

    std::string selectedOutputForPipe_ = MANUAL_MIDI_SOURCE;
};

MidiCcController::MidiCcController(ClientManager& clients, DspEngine& dsp, unsigned httpPort)
    : m_impl(std::make_unique<Impl>(clients, dsp, httpPort)) {}

MidiCcController::~MidiCcController() = default;

bool MidiCcController::start() { return m_impl && m_impl->start(); }

void MidiCcController::stop() { if (m_impl) m_impl->stop(); }
