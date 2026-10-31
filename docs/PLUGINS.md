# Native C++ processors, plugin nodes, composite chains, and MIDI

This document describes the plugin architecture implemented in the `cm5audio` source tree at commit [`368779fa884f6e5a1512249218190bddf7eca22d`](https://github.com/Synergyst/cm5audio/commit/368779fa884f6e5a1512249218190bddf7eca22d).

The backend registry is the source of truth for processor identity, parameter definitions, port declarations, audio channel layouts, and composite-chain channel mode. The dashboard discovers this metadata from `GET /api/dsp/available`; it should not maintain a separate hard-coded plugin/parameter catalogue.

This is a **compiled-in native processor system**. It is not a VST/LV2/LADSPA/CLAP host, a plugin-directory scanner, or a runtime shared-library loader. Changing C++ implementation/descriptor code requires rebuilding and restarting `cm5audio`.

## Contents

- [Processing models](#processing-models)
- [Built-in processors](#built-in-processors)
- [Processor interface and real-time rules](#processor-interface-and-real-time-rules)
- [Typed port descriptors](#typed-port-descriptors)
- [Parameter descriptors and validation](#parameter-descriptors-and-validation)
- [Composite-chain channel modes](#composite-chain-channel-modes)
- [Plugin-chain nodes and presets](#plugin-chain-nodes-and-presets)
- [MIDI event model](#midi-event-model)
- [Registry discovery API](#registry-discovery-api)
- [Plugin-node and chain HTTP APIs](#plugin-node-and-chain-http-apis)
- [Route DSP compatibility API](#route-dsp-compatibility-api)
- [Registration and development workflow](#registration-and-development-workflow)
- [Persistence, restoration, and schema compatibility](#persistence-restoration-and-schema-compatibility)
- [Publication, ownership, and thread boundaries](#publication-ownership-and-thread-boundaries)
- [Validation and verification](#validation-and-verification)
- [Current limitations](#current-limitations)

## Processing models

There are three related but distinct ways a processor can be used.

### 1. Route-attached DSP chain

A route connects one source channel to one destination channel. Its optional DSP chain belongs to that route and receives a **mono** buffer. The route's ordered processors run in sequence, then DSP master gain is applied (when DSP is active), then ordinary route gain is applied before summing into the destination.

Route DSP remains useful for small per-edge processing changes and compatibility with the original route inspector. It cannot represent a linked stereo processor across two separate mono routes.

### 2. Individual graph-native plugin node

An individual plugin node owns one processor instance, a stable plugin instance ID, parameters, active/bypass state, and its declared typed ports. Audio input/output ports become patchbay endpoints; MIDI ports participate in the MIDI event graph. The general `AudioProcessContext` allows a processor to receive multiple named audio input/output buses and MIDI input/output buffers.

A standard legacy one-input/one-output descriptor is normalized by the registry into `audio_in` and `audio_out` typed audio ports. A processor that declares more than the primary pair, or declares MIDI, must implement the context-based processing interface to consume/produce those additional buses/events. The default legacy adapter only handles the primary audio bus.

### 3. Composite plugin-chain node

A composite chain node is a graph-native node whose internal definition is an ordered list of registered processors. It currently represents **one serial audio bus** with one symmetric channel width from 1 to 8. The chain engine prepares each stage's processor instance(s) before publishing the new node state.

- `per_channel`: create an independent mono processor instance/state for every channel.
- `multichannel`: create one processor instance and call its buffer processor with the complete interleaved bus and the channel count.
- `mono_only`: reject a composite chain wider than one channel.

The chain's `chain_gain` is applied after its internal stages. A composite chain is not an arbitrary multi-bus container: its stored chain array contains serial stages sharing the same bus.

## Built-in processors

The registry in `src/plugins/register_builtin_processors.cpp` registers these types in this revision:

| Type | Display name | Notes |
|---|---|---|
| `passthrough` | Passthrough | No-op processor. |
| `gain` | Gain | Linear amplitude multiplier. |
| `compressor` | Compressor | Hard-knee compressor with a linear-amplitude threshold and smoothed gain reduction. |
| `echo` | Echo | Delay/echo effect. |
| `chorus` | Chorus | Modulated chorus effect. |
| `reverb` | Reverb | Reverb effect. |
| `lush_reverb` | Lush Reverb | Extended reverb effect. |
| `eq` | EQ | Equalizer. |
| `autotune` | Autotune | Pitch correction. |
| `vowel_filter` | Vowel Filter | Vowel/formant-style filtering. |
| `soft_clipper` | Soft Clipper | Soft clipping/saturation. |
| `hpf` | High-Pass Filter | Removes low-frequency content. |
| `lpf` | Low-Pass Filter | Removes high-frequency content. |
| `deesser` | De-Esser | Sibilance reduction. |
| `normalize` | Normalize | Automatic gain control/level normalization. |
| `expander` | Expander | Downward expansion. |
| `noise_gate` | Noise Gate | Gate/noise reduction. |
| `bitcrusher` | Bitcrusher | Bit-depth/rate reduction. |
| `limiter` | Limiter | Look-ahead limiting. |
| `midi_pitch_shifter` | MIDI Pitch Shifter | Audio pitch processor with MIDI control. |
| `midi_harmonizer` | MIDI Harmonizer | MIDI-controlled harmony processor. |
| `rnnoise` | RNNoise | RNNoise-based denoising. |

Always query discovery for the exact parameters/defaults/ranges/ports in the executable being run. The table above is a type overview, not a replacement for registry metadata.

## Processor interface and real-time rules

Primary interface: `include/audio_processor.h`.

```cpp
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual void process(float* buffer,
                         size_t frames,
                         unsigned channels);

    virtual void process(AudioProcessContext& context);

    virtual void attach_midi_input(
        const std::string& portId,
        const std::shared_ptr<MidiEventQueue>& queue);
};
```

The actual declarations and default implementations in `include/audio_processor.h` are authoritative; the excerpt above is a shape summary.

### Legacy in-place processor

Existing/simple processors can override:

```cpp
void process(float* buffer, size_t frames, unsigned channels) override;
```

The buffer is interleaved float32. The `channels` argument is the actual number of channels represented by the buffer, not a compile-time constant. A route-attached DSP processor receives `channels == 1`. A composite-chain `multichannel` stage receives the entire interleaved chain bus; a `per_channel` stage receives one channel per independent instance.

When used as a normal graph-native plugin node, the base `process(AudioProcessContext&)` adapter copies the first audio input bus into the first audio output bus and calls the legacy in-place method on the output buffer. If there is no valid primary input, it fills the primary output with silence. The adapter does not process auxiliary buses or MIDI ports.

### Context-based processor

Override:

```cpp
void process(AudioProcessContext& context) override;
```

`AudioProcessContext` contains:

- Legacy primary-bus aliases: `input`, `output`, `frames`, `input_channels`, `output_channels`.
- `sample_rate` (engine default 48,000 Hz).
- `audio_inputs` and `audio_input_count`.
- `audio_outputs` and `audio_output_count`.
- `midi_inputs` and `midi_input_count`.
- `midi_outputs` and `midi_output_count`.

Each `AudioBusBuffer` has a stable descriptor `port_id`, input pointer, output pointer, and channel count. Inputs/outputs are interleaved float32. Pointers are prepared by the engine for the call and must not be retained after `process()` returns.

A context-based processor is required when the descriptor declares auxiliary audio ports or MIDI ports. It should look up the bus by `port_id` (or use the declared/stable descriptor ordering deliberately), handle absent/empty optional inputs safely, and write every sample of every declared output bus that it owns for the current block. Do not assume the first input and first output are the only ports if the descriptor declares more.

### Real-time processing contract

Native processors execute on the DSP worker. Inside `process()` and other audio-thread callbacks:

- Do not allocate/free memory or grow containers.
- Do not acquire mutexes or wait on condition variables.
- Do not perform filesystem, network, device, logging, or other blocking I/O.
- Do not throw exceptions.
- Do not retain engine-provided buffer pointers beyond the call.
- Keep loops bounded by `frames`, the declared channel count, and the finite capacities in the context.
- Preallocate state, scratch buffers, delay lines, and event output storage during construction/configuration on the control thread.

This is a processor-level contract. It does not mean the whole engine/server process is completely lock-free or allocation-free: graph rebuilds, metrics maps, API work, endpoint retirement, and control-side publication use locks/containers outside the processor callback.

## Typed port descriptors

The registry data model is in `include/processor_registry.h`.

A processor declares up to `CM5_MAX_PLUGIN_PORTS` (16) unique ports. Each port has:

| Field | Meaning |
|---|---|
| `id` | Stable port identifier used in endpoint names, graph/preset metadata, and processing context. |
| `name` | Human-readable port label. |
| `type` | `AUDIO` or `MIDI`. |
| `direction` | `INPUT` or `OUTPUT`. |
| `channels` | `ProcessorChannelLayout` for audio ports; MIDI ports do not have audio channels. |
| `source_parameter` | Optional string/enum parameter ID used by legacy MIDI processor input selection. |

`ProcessorChannelLayout` has `minimum`, `maximum`, and `default_channels`. Valid audio layouts have minimum at least 1, maximum no greater than `CM5_MAX_CHANNELS` (254), and default within the range.

Port direction maps to graph semantics:

| Processor port | Application patchbay endpoint direction |
|---|---|
| Audio input | Destination (a source route connects into it). |
| Audio output | Source (it can connect to downstream destinations). |
| MIDI input | Destination in the MIDI route API. |
| MIDI output | Source in the MIDI route API. |

A plugin node port endpoint uses:

```text
plugin/<plugin-instance-id>/<port-id>
```

For compatibility, descriptors that leave `ports` empty are normalized by the registry into two typed audio ports: `audio_in` and `audio_out`, using the legacy `audio_input` and `audio_output` channel layout fields. New code should prefer explicit `ports` declarations.

### Multiple audio buses

A normal graph-native plugin instance can declare multiple named audio input and output buses, each with its own channel layout. Its `AudioProcessContext` receives the buses for one block. If it declares auxiliary buses but only implements the legacy `process(float*,frames,channels)` method, those auxiliary buses will not be processed by the default adapter.

The composite chain format is narrower: it serializes one audio stream through an ordered list of processors. Supporting arbitrary multi-bus subgraphs inside a composite chain would require a different chain model; it is not implied by the typed port feature.

### MIDI source parameters

A MIDI input port may set `source_parameter` to a descriptor parameter whose type is `string` or `enum`. The registry verifies that reference. This is used by the current MIDI processors to retain their existing external MIDI source selection behavior. It should not be mistaken for a generic central MIDI device manager.

## Parameter descriptors and validation

`ProcessorParameter` includes:

| Field | Meaning |
|---|---|
| `id` | Stable JSON parameter key. |
| `name` | Human-readable label. |
| `type` | `number`, `integer`, `boolean`, `string`, or `enum`. |
| `default_value` | JSON value used for new instances and omitted parameter keys. |
| `min`, `max` | Optional numeric bounds. |
| `step` | Optional positive UI editing increment. |
| `unit` | Display-only unit string, such as `Hz`, `s`, `linear`, or `ms`. |
| `choices` | Enum label/value pairs. |

Example numeric and enum definitions:

```cpp
{
    "gain", "Gain", "number", 1.0,
    0.0, 8.0, 0.01, "linear", {}
}
```

```cpp
{
    "mode", "Mode", "enum", "fast",
    {}, {}, {}, "",
    {{"Fast", "fast"}, {"Smooth", "smooth"}}
}
```

Validation rules:

- Parameter IDs must be nonempty and unique within a processor.
- Parameter type must be one of the five supported types.
- Numeric min/max/step must be finite and internally valid; step must be positive.
- Numeric defaults/updates must be finite and within bounds.
- Integer parameters require whole-number JSON values.
- Enum choices must be nonempty, scalar, finite where numeric, and unique by value.
- Non-enum parameters cannot declare choices.
- Units are display-only; the backend does not convert them.
- `step` controls the dashboard increment; it is not backend quantization.
- Unknown parameter keys are rejected.
- Omitted parameter keys are filled from descriptor defaults before factory invocation.

Renaming/removing a parameter or changing its JSON type can invalidate old presets/settings. There is no general processor schema migration framework in this revision. Keep IDs stable when possible; when a schema break is unavoidable, provide explicit migration logic in settings restoration or document the manual correction.

## Composite-chain channel modes

A descriptor's `chain_channel_mode` tells the composite chain builder how to instantiate and call that processor. The enum and JSON names are defined in `include/processor_registry.h`.

### `per_channel`

The default mode. At chain-node preparation time, the manager creates one independent mono processor instance per channel. During processing, it extracts one channel into preallocated mono scratch, calls that channel's processor with `channels == 1`, then writes the result back into the interleaved bus. State is independent between channels.

Use this for a processor whose implementation is intrinsically mono or whose behavior should be independent for each channel. It does not provide linked stereo detection or state.

### `multichannel`

The chain manager creates one processor instance for the stage and calls its legacy buffer processing entry with the full interleaved bus and actual channel count. The implementation must iterate by frame/channel correctly and deliberately decide whether to link channel detectors/state or process channels independently.

Examples explicitly declared `multichannel` in this revision:

- `gain`: multiplies every interleaved sample by the same linear gain.
- `compressor`: calculates the peak across each complete frame and applies one shared envelope/gain value to all channels for that frame.
- `normalize`: detects a shared peak across channels and applies a shared gain state.
- `limiter`: detects a shared per-frame peak and applies linked gain across channels.
- `hpf`: keeps filter state per channel while processing a full multichannel interleaved bus.

These implementations do not all have identical stereo semantics. A multichannel declaration means one instance receives the entire bus; the code determines whether internal detector/state is linked or channel-specific.

### `mono_only`

The chain builder rejects any width greater than one. `rnnoise` is explicitly declared `mono_only` in this revision. This is the appropriate declaration when an effect's algorithm/model/processing assumptions should not be widened by per-channel cloning or interleaved multi-channel invocation.

Do not infer the mode from the implementation's buffer loop alone. The descriptor is authoritative. If a future processor is changed to support multi-channel chain use, update its descriptor and implementation together, and verify linked/per-channel state semantics.

## Plugin-chain nodes and presets

### Chain definition

A composite chain definition is an ordered JSON array:

```json
[
  {"type": "compressor", "params": {"threshold": 0.5, "ratio": 4.0}},
  {"type": "normalize", "params": {"target": 0.25, "speed": 0.1, "max_gain": 4.0}},
  {"type": "limiter", "params": {"threshold": 0.9, "attack": 0.005, "release": 0.05, "lookahead": 20.0}}
]
```

The chain must be nonempty and no longer than 16 stages. Each element must have a registered `type` and a `params` object. Missing parameter keys receive defaults; unknown types/keys and invalid values reject the entire requested chain.

### Chain-node properties

A chain node contains:

- `id`: live plugin instance ID.
- `name`: node label.
- `chain`: canonical ordered processor definition.
- `chain_gain`: finite linear multiplier in 0..8.
- `active`: chain active/bypass state.
- `channels`: symmetric input/output bus width, 1..8.
- audio input/output ports (usually `audio_in` and `audio_out`) and their channel widths.

The chain stages execute in array order. Chain gain is applied after the stages. Each stage's `chain_channel_mode` determines its processor instance count and call width.

### Preset properties and lifecycle

Presets are persistent definitions stored independently of live `plugin_nodes`. A preset carries an ID, name, chain, chain gain, active state, and channel count. Operations:

- Create a preset from a chain definition or dashboard chain.
- Edit a preset's name, processor parameters, processor list/order, chain gain, active state, and channel count.
- Instantiate a preset to create a new live node.
- Replace an existing live chain node from a preset. This updates the internal chain while retaining the live node ID and its existing graph connections where compatible.
- Delete a preset without deleting nodes already instantiated from it.
- Delete a live node without deleting the preset it may have originated from.

Editing a preset does not retroactively update already-instantiated nodes. The user must explicitly instantiate or replace from the revised preset to apply the new definition.

### Route extraction semantics

`POST /api/plugins/extract-chain` converts the complete DSP chain attached to **one route** into one composite chain node. Since a route represents one source channel connected to one destination channel and route DSP is mono, the extracted chain is normally created as a one-channel node. The handler splits the selected route into an upstream route feeding the node and a downstream route from the node output.

`POST /api/plugins/extract` extracts one processor at a selected index from one route DSP chain into an individual plugin node, splitting the original route's processing into before/after route chains around the new node.

These operations do not automatically combine multiple separate mono channel routes into a stereo chain node. For that, create/set the chain node to the desired width and explicitly connect each channel.

## MIDI event model

MIDI definitions are in `include/audio_processor.h`.

`MidiEvent` contains:

- `time`: event time field carried through the queue.
- `protocol`: `1` for MIDI 1.0, `2` for UMP.
- `size`: number of payload bytes.
- `data`: fixed payload array, maximum 16 bytes.

`MidiEventQueue` is a bounded multi-producer/single-consumer queue with capacity 1,024. It does not allocate or lock in its push/pop operations. When full, a push fails and increments a dropped-event counter. `MidiInputBuffer` exposes a pointer/count pair for events received in the current processing block. `MidiOutputBuffer::push()` appends to its preallocated bounded output array and returns false when invalid/full.

A MIDI-capable context processor should:

1. Locate the declared MIDI input/output buffer by port ID.
2. Treat input events as a read-only array valid for the current call.
3. Validate the protocol/size before interpreting payload bytes.
4. Write outgoing events with `MidiOutputBuffer::push()` and check the return value.
5. Avoid allocation, locks, I/O, exceptions, or unbounded processing.
6. Handle event-queue overflow as a normal bounded-capacity condition; do not block waiting for space.

The graph MIDI route API connects only declared plugin MIDI outputs to declared plugin MIDI inputs. The engine prepares dispatch destinations and routes events through the bounded queues. Audio and MIDI port topology is validated by the graph manager; use its API instead of creating arbitrary endpoint strings.

### Physical MIDI input

In this revision, `midi_pitch_shifter` and `midi_harmonizer` retain their own PipeWire MIDI input services. They discover/select external MIDI sources through plugin parameters and attach incoming events to their graph-side input queue. The shared plugin MIDI graph can then forward events between declared plugin ports.

This is not a centralized host-level MIDI device manager. Physical devices are not general source nodes that can be routed to any plugin without an existing plugin-specific service. A future central MIDI engine/device layer should move discovery/link ownership out of individual processors while preserving the graph queue interface.

## Registry discovery API

### `GET /api/dsp/available`

Returns no-store JSON describing the current compiled-in processors. The schema at this baseline is version 2 and includes limits such as `max_chain_length`, `max_plugin_ports`, and `max_chain_channels`.

A processor discovery object includes:

```json
{
  "type": "gain",
  "name": "Gain",
  "description": "Linear amplitude multiplier (1 = unity, 0 = silence).",
  "defaults": {"gain": 1.0},
  "parameters": [
    {
      "id": "gain",
      "name": "Gain",
      "type": "number",
      "default": 1.0,
      "min": 0.0,
      "max": 8.0,
      "step": 0.01,
      "unit": "linear"
    }
  ],
  "ports": [],
  "chain_channel_mode": "multichannel"
}
```

The exact fields depend on the processor; this example is abbreviated. Discovery also returns normalized audio input/output layout information. Use the endpoint instead of treating examples or this document's descriptions as a hard-coded list of parameter values.

If `ports` is omitted/empty in the C++ descriptor, registration synthesizes `audio_in` and `audio_out` from the legacy audio layout fields. Discovery reports the normalized typed ports.

## Plugin-node and chain HTTP APIs

All processor/node/preset mutation requests use JSON bodies unless specified otherwise. `application/json` should be sent for these endpoints.

### Individual plugin nodes

Create a registered processor instance:

```http
POST /api/plugins
Content-Type: application/json
```

```json
{
  "type": "gain",
  "name": "Stereo Gain",
  "params": {"gain": 0.75},
  "input_channels": 2,
  "output_channels": 2
}
```

For multi-bus descriptors, use `port_channels` keyed by audio port ID:

```json
{
  "type": "some_multi_bus_processor",
  "name": "Example",
  "params": {},
  "port_channels": {
    "main_in": 2,
    "sidechain_in": 2,
    "main_out": 2
  }
}
```

The second example is a schema illustration; the named processor/ports must actually be registered. The server validates all requested port IDs and channel counts against the descriptor.

Other individual-node endpoints:

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/api/plugins` | List live plugin nodes and configured MIDI graph routes. |
| POST | `/api/plugins/{id}` | Update instance name, params, active/bypass, and audio channel widths/`port_channels`. |
| DELETE | `/api/plugins/{id}` | Delete the live plugin instance; waits for adoption and saves. |
| POST | `/api/midi/routes` | Create plugin MIDI output → input graph route. |
| GET | `/api/midi/routes` | List MIDI graph routes. |
| DELETE | `/api/midi/routes/{id}` | Delete MIDI graph route. |

The plugin node response contains `id`, `name`, `type`, `active`, `params`, endpoint IDs, channel counts, and a normalized `ports` array. Composite chain nodes additionally return `chain`, `chain_gain`, `channels`, and `processor_count`.

### Composite chain nodes

Create a chain node:

```http
POST /api/plugins/chains
Content-Type: application/json
```

Body:

```json
{
  "name": "VOIPProc",
  "channels": 2,
  "active": true,
  "chain_gain": 1.0,
  "chain": [
    {"type": "compressor", "params": {"threshold": 0.5, "ratio": 4.0}},
    {"type": "normalize", "params": {"target": 0.25, "speed": 0.1, "max_gain": 4.0}},
    {"type": "limiter", "params": {"threshold": 0.9, "attack": 0.005, "release": 0.05, "lookahead": 20.0}}
  ]
}
```

Update an existing chain node:

```http
POST /api/plugins/{id}/chain
Content-Type: application/json
```

The body requires `chain`; optional `name`, `chain_gain`, `active`, and `channels` default to the current values. The whole replacement chain is validated/prepared before publication.

Replace from a preset:

```http
POST /api/plugins/{id}/replace-chain-preset
Content-Type: application/json
```

```json
{"preset_id": 2}
```

The backend verifies the target is a composite chain node and the preset exists. Its update path retains the live node ID, port IDs, meter banks, and existing graph cables when valid for the replacement.

### Preset endpoints

| Method | Endpoint | Body/behavior |
|---|---|---|
| GET | `/api/plugin-chain-presets` | Return `{presets:[...]}`. |
| POST | `/api/plugin-chain-presets` | Required `name` and `chain`; optional `chain_gain`, `active`, `channels`. Creates a preset. |
| POST | `/api/plugin-chain-presets/{id}/update` | Partial JSON update. Omitted fields preserve the existing preset values. |
| POST | `/api/plugin-chain-presets/{id}/instantiate` | Creates a live node from the preset; optional JSON `name` override. |
| DELETE | `/api/plugin-chain-presets/{id}` | Deletes only the preset definition. |

Preset data is separately persisted in `plugin_chain_presets`. Preset mutations are auto-saved by the current API wrapper. Live chain-node mutations via `/api/plugins/chains`, `/api/plugins/{id}/chain`, and `/api/plugins/{id}/replace-chain-preset` are also auto-saved. A preset being edited does not alter already-instantiated nodes until explicitly re-instantiated/replaced.

### MIDI graph endpoint format

A route source/destination string must identify a declared MIDI port and must have the correct direction. For example:

```json
{
  "source": "plugin/4/midi_out",
  "destination": "plugin/6/midi_in"
}
```

The source must be MIDI output; the destination must be MIDI input. Audio port IDs are not accepted by the MIDI route API.

## Route DSP compatibility API

The legacy route processor system remains supported. It is not the composite chain-node API.

Discovery:

```http
GET /api/dsp/available
```

Read an existing route's DSP:

```http
GET /api/routes/{id}/dsp
```

Apply the complete route DSP definition (legacy DSP-only endpoint):

```http
POST /api/routes/{id}/dsp
Content-Type: application/json
```

```json
{
  "active": true,
  "gain": 1.0,
  "chain": [
    {"type": "gain", "params": {"gain": 0.5}},
    {"type": "compressor", "params": {"threshold": 0.1, "ratio": 4.0}}
  ]
}
```

This replaces the whole DSP configuration. Omitted `active`, `gain`, and `chain` fields use defaults (`false`, `1.0`, and `[]`). Every processor element requires `type` and `params` object. Unknown types/keys, wrong JSON types, non-finite values, invalid integer values, and out-of-range parameters reject the request with HTTP 400; the previous state remains in place.

The dashboard's normal Route Inspector uses:

```http
POST /api/routes/{id}/apply
Content-Type: application/json
```

with a complete `{gain, enabled, dsp}` object. It validates ordinary route gain/enabled and the complete DSP definition before publishing. The inspector treats the work as a local draft until APPLY. Closing/discarding the draft sends no mutation.

## Registration and development workflow

### Add a simple processor

1. Implement an `AudioProcessor` subclass in `src/plugins/` (or an existing plugin source) and include its declaration/header as appropriate.
2. Give the implementation a registry descriptor and factory. The factory must build a fully initialized processor from the normalized `params` object.
3. Add a small `register_<name>_processor(ProcessorRegistry&)` function.
4. Declare/call it in `src/plugins/register_builtin_processors.cpp`, unless registering an application-owned processor during startup through the registry singleton.
5. Add any new `.cpp` file to `APP_SRCS` in the Makefile.
6. Rebuild `cm5audio`, restart it, then reload the dashboard / refresh discovery.

No plugin-specific JavaScript menu/parameter UI should be needed when the descriptor correctly advertises its metadata.

### Minimal registration shape

```cpp
#include "processor_registry.h"
#include <memory>

class MyFilter final : public AudioProcessor {
public:
    explicit MyFilter(float frequency) : m_frequency(frequency) {}

    void process(float* buffer, size_t frames, unsigned channels) override {
        // Process the interleaved buffer in-place.
        // Do not allocate, lock, throw, log, or perform I/O here.
        (void)buffer;
        (void)frames;
        (void)channels;
    }

private:
    float m_frequency;
};

void register_my_filter_processor(ProcessorRegistry& registry) {
    ProcessorDescriptor descriptor{
        "my_filter",
        "My Filter",
        "Example filter processor.",
        {{"frequency", "Frequency", "number", 1000.0,
          20.0, 20000.0, 1.0, "Hz", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<MyFilter>(
                params.at("frequency").get<float>());
        }
    };
    descriptor.audio_input = {1, 1, 1};
    descriptor.audio_output = {1, 1, 1};
    registry.register_processor(std::move(descriptor));
}
```

The sample algorithm is intentionally omitted; it demonstrates the descriptor/factory shape only. In a real implementation, choose the right input/output channel layout and chain mode. `PER_CHANNEL` is the default if no chain mode is set.

### Register explicit typed ports

For a plugin with separate main and sidechain buses and a MIDI input, declare ports explicitly:

```cpp
descriptor.ports = {
    {"main_in", "Main Input", ProcessorPortType::AUDIO,
     ProcessorPortDirection::INPUT, {1, 8, 2}, {}},
    {"sidechain_in", "Sidechain Input", ProcessorPortType::AUDIO,
     ProcessorPortDirection::INPUT, {1, 8, 2}, {}},
    {"main_out", "Main Output", ProcessorPortType::AUDIO,
     ProcessorPortDirection::OUTPUT, {1, 8, 2}, {}},
    {"midi_in", "MIDI Input", ProcessorPortType::MIDI,
     ProcessorPortDirection::INPUT, {}, {}}
};
```

This is a descriptor illustration. A plugin that declares these buses must override `process(AudioProcessContext&)` and actually process the auxiliary bus/MIDI event data. The default legacy adapter only copies/processes the first audio input/output bus.

### Register a multichannel chain processor

If a processor is intended to run as one linked/full-bus stage in a composite chain:

1. Implement `process(float*,frames,channels)` for arbitrary supported interleaved channel counts.
2. Ensure all sample addressing uses `frame * channels + channel` (or an equivalent correct interleaved traversal).
3. Decide and document whether detector/envelope/state is linked across channels or independent per channel.
4. Preallocate any per-channel state for the descriptor's maximum width; do not resize in `process()`.
5. Set `descriptor.chain_channel_mode = ProcessorChainChannelMode::MULTICHANNEL`.
6. Set the descriptor's audio input/output channel layouts to the supported range.
7. Test mono, stereo, and the maximum advertised width.

For processors that must never be widened in a composite chain, explicitly declare `MONO_ONLY` and expose a one-channel-only layout. Do not rely on UI-only restrictions; backend chain validation must reject invalid widths.

### Registration timing

Built-ins are registered during `ProcessorRegistry` construction. Application-owned processors may call `ProcessorRegistry::instance().register_processor(...)` from startup code **before settings are restored and before worker/server threads start**. Registration is startup-only; concurrent registration is not supported. The registry rejects duplicate type/parameter/port IDs and invalid descriptors.

## Persistence, restoration, and schema compatibility

The default file is `./cm5audio_settings.json`, override with `CM5AUDIO_SETTINGS`.

Relevant plugin keys:

```json
{
  "plugin_nodes": [
    {
      "id": 10,
      "name": "VOIPProc",
      "type": "plugin_chain",
      "input_channels": 2,
      "output_channels": 2,
      "port_channels": {"audio_in": 2, "audio_out": 2},
      "active": true,
      "params": {},
      "channels": 2,
      "chain_gain": 1.0,
      "chain": [
        {"type": "compressor", "params": {"threshold": 0.5, "ratio": 4.0}}
      ]
    }
  ],
  "plugin_chain_presets": [
    {
      "id": 2,
      "name": "VOIPProc",
      "channels": 2,
      "active": true,
      "chain_gain": 1.0,
      "processor_count": 1,
      "chain": [
        {"type": "compressor", "params": {"threshold": 0.5, "ratio": 4.0}}
      ]
    }
  ],
  "midi_routes": [
    {
      "id": 1,
      "source": "plugin/4/midi_out",
      "destination": "plugin/6/midi_in",
      "enabled": true
    }
  ]
}
```

This is an abbreviated shape illustration; the server writes other application settings in the same document. Live instances and saved presets are separate. A preset can exist without a live node; a live node can continue after its source preset is deleted.

On startup, registered processors are used to reconstruct plugin instances and validate saved chain definitions. A missing/unavailable type or invalid parameters can cause that instance/preset to be skipped with a diagnostic rather than manufacturing a placeholder processor. Keep processor type and parameter IDs stable to preserve old settings.

Settings saves use the server's accepted state and a same-directory temporary file/rename. `POST /api/settings/save` waits for engine adoption before serialization. Device activation/deactivation, tone updates, plugin-chain preset operations, composite chain-node operations, chain extraction, and plugin-node deletion have specific automatic save paths. Ordinary route edits, route DSP updates, fixed global DSP edits, and ordinary plugin-node create/update operations require explicit SAVE. Check response fields (`adopted`, `persisted`) rather than assuming every accepted POST was written.

The settings `version` is currently `1`; it is not a comprehensive migration system. Renaming/removing parameters or changing descriptor compatibility can require explicit migration/manual configuration correction. The server does not automatically recover `.bak` files.

## Publication, ownership, and thread boundaries

The registry is immutable after startup registration. Discovery/factory lookup occurs on control/API paths; processor factories, parameter normalization, and object construction are not audio-thread work.

Route DSP uses multiple preallocated state slots per route and a reader pin so a control-side replacement does not destroy a chain still being used by the DSP worker. Plugin-node and graph publications retain endpoint/plugin owners across graph rebuilds and retire old owners on control-side paths. The engine reports a revision as adopted after a completed processing block has acknowledged the requested graph/state revision.

This acknowledgement means the engine has completed a block using the adopted revision. It does not prove hardware/network buffers have drained or that an external device has rendered audible samples.

The processor callback itself must obey the no-allocation/no-lock/no-I/O/no-throw rule. The entire application is not lock-free: graph rebuilds, metrics map management, endpoint lifecycle, HTTP handlers, and persistence use mutexes/containers on their respective threads.

Enabled graph cycles are unsupported. The graph uses a topological order; a cycle can prevent cyclic nodes from appearing in the processing order. Do not use existing feedback buffers as a supported feedback/looping API.

## Validation and verification

The Makefile defines:

```bash
make test-plugins
make test-channels
make test-reliability
```

- `test-plugins` exercises registry metadata/validation, processor behavior, chain lifetime, API discovery/persistence and dashboard DOM logic. Its native test server uses loopback ports 28182/28183 and `DeviceManager(false)`; it does not activate physical devices.
- `test-channels` runs synthetic channel-width and route/meter tests with manually driven callbacks; it does not qualify physical connector mapping.
- `test-reliability` exercises API/adoption/persistence, null-device lifecycle, and process shutdown behavior.

A new processor should add deterministic tests for parameter defaults/range/type rejection, correct mono processing, channel-width behavior matching its declared mode, state independence/linking, bypass behavior, saved-state restoration, and graph routing. MIDI processors should test bounded queue overflow, protocol/size handling, event ordering/forwarding, and no allocation/locking in the processing callback.

## Current limitations

- No VST/LV2/LADSPA/CLAP host or runtime plugin loading.
- Composite plugin-chain definitions model one serial audio bus, not arbitrary nested/multi-bus plugin subgraphs.
- Route-attached DSP remains mono per route.
- Processor channel-mode metadata must match the real algorithm; unsupported modes should not be advertised.
- RNNoise is mono-only for composite chains in this revision.
- Physical PipeWire MIDI source discovery/connection is still owned by individual MIDI processors; no generic physical MIDI graph-source manager exists.
- MIDI queue/event capacities are bounded; a full queue may drop events.
- No general MIDI timeline, host clock/scheduler, or external MIDI device graph endpoint model.
- Enabled audio graph cycles are not supported.
- The processor interface is native C++ and requires rebuilding/restarting after registration changes.

For the application-level architecture, startup, routing API, settings and security notes, see the [main README](../README.md).
