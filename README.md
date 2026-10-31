# cm5audio

**A Linux audio patchbay and DSP graph with channel-level routing, native C++ processors, composite plugin-chain nodes, MIDI event routing, and a browser-based control surface.**

`cm5audio` routes and mixes audio between local capture/playback endpoints, a built-in sine-wave generator, and remote WebSocket audio clients. The native application is developed on Raspberry Pi Compute Module 5 hardware, but its audio-device layer uses miniaudio and is not hard-coded to one audio board or connector layout.

The project combines:

- A C++17 audio engine and device manager.
- A channel-addressable audio routing graph.
- A compiled-in C++ processor registry.
- Route-attached mono DSP chains and graph-native plugin instances.
- Composite plugin-chain nodes with configurable channel widths and persistent presets.
- Typed audio and MIDI ports, plus plugin-to-plugin MIDI event routing.
- HTTP/HTTPS control APIs and WebSocket/WSS audio transport.
- A monolithic HTML/CSS/JavaScript dashboard served by the native executable.

> **Documentation baseline:** This document describes the repository tree at commit [`368779fa884f6e5a1512249218190bddf7eca22d`](https://github.com/Synergyst/cm5audio/commit/368779fa884f6e5a1512249218190bddf7eca22d). Build-time registry metadata and API discovery remain the runtime source of truth where noted below. The project is actively developed; this document is not a guarantee that every hardware/backend combination has been qualified.

## Contents

- [Capabilities and boundaries](#capabilities-and-boundaries)
- [Architecture overview](#architecture-overview)
- [Audio format, timing, and resource limits](#audio-format-timing-and-resource-limits)
- [Backends and hardware devices](#backends-and-hardware-devices)
- [Dependencies and setup](#dependencies-and-setup)
- [Build and verification](#build-and-verification)
- [Running and configuration](#running-and-configuration)
- [Dashboard workflow](#dashboard-workflow)
- [Routing model](#routing-model)
- [DSP processors and plugin chains](#dsp-processors-and-plugin-chains)
- [MIDI support](#midi-support)
- [Tone generator, global DSP, and meters](#tone-generator-global-dsp-and-meters)
- [HTTP API reference](#http-api-reference)
- [WebSocket audio protocol](#websocket-audio-protocol)
- [Settings persistence and restoration](#settings-persistence-and-restoration)
- [TLS, browser access, and security](#tls-browser-access-and-security)
- [Source map](#source-map)
- [Diagnostics and troubleshooting](#diagnostics-and-troubleshooting)
- [Known limitations](#known-limitations)

## Capabilities and boundaries

### Implemented in this revision

- Enumerate local audio devices and independently activate/deactivate capture and playback directions.
- Request a channel count per device direction, including automatic selection or an explicit count up to the application/miniaudio channel limit.
- Route one source channel to one destination channel, fan one source channel out to several destinations, and sum several routes into one destination channel.
- Use hardware, tone-generator, plugin-node, and connected WebSocket-client endpoints in the application's routing graph.
- Apply an ordered mono DSP chain to an individual route.
- Instantiate registered processors as independent graph-native plugin nodes with typed audio/MIDI ports.
- Create composite plugin-chain nodes from ordered processor definitions, with a configurable symmetric audio width from one to eight channels.
- Save, edit, instantiate, replace from, and delete named plugin-chain presets.
- Convert a route's complete DSP chain into a composite plugin-chain node, or extract one processor from a route chain into an individual plugin node.
- Declare processor ports and parameters in C++; generate discovery metadata and dashboard controls from the registry.
- Support per-channel, multichannel, and mono-only processor behavior inside composite chains.
- Connect declared plugin MIDI outputs to declared plugin MIDI inputs through bounded event queues.
- Receive physical MIDI through the existing per-plugin PipeWire MIDI services used by the MIDI processors.
- Display per-channel capture/playback, tone-generator, and plugin audio-port meters.
- Generate a configurable mono sine tone and persist its enabled/frequency/amplitude settings.
- Persist activated-device selections, route state, plugin instances, plugin-chain presets, MIDI graph routes, tone state, global DSP settings, and known client names.
- Serve the main dashboard and optional browser audio terminal over HTTP/HTTPS and WS/WSS.
- Optionally display a separate PipeWire topology graph when started with `--enable-pipewire-patchbay`.

### Important boundaries

- **This is not a VST, LV2, LADSPA, CLAP, or external plugin host.** Processors are compiled into `cm5audio` and registered at startup. There is no runtime shared-library scanner or hot loader.
- **Route DSP and plugin-chain nodes are different mechanisms.** Route DSP is attached to one channel-to-channel route and processes a mono signal. A composite plugin-chain node is a graph node with a one-to-eight-channel audio bus.
- **A composite chain is not an arbitrary multi-bus container.** Its internal processor list is a serial single-bus chain. The general `AudioProcessContext` supports multiple typed buses for processors instantiated as graph-native plugin nodes, but the composite chain format itself represents one audio stream plus chain-level settings.
- **Multichannel support is processor-specific.** A processor must declare how it participates in a wide composite chain. A processor that is not declared `multichannel` is normally instantiated independently per channel. A `mono_only` processor prevents a chain wider than one channel.
- **RNNoise is declared `mono_only` in this revision.** Do not widen a chain containing RNNoise.
- **MIDI device management is not yet centralized.** Plugin-to-plugin MIDI graph connections exist, while physical PipeWire MIDI discovery/connection remains in the existing MIDI processor-specific services. Physical MIDI devices are not general-purpose source nodes in the main patchbay graph.
- **WebSocket audio is raw PCM, not a fully negotiated network-media protocol.** It has no versioned binary frame header, sequence tracking, jitter buffer, or clock-drift compensation.
- **Hardware channel gain/mute is not wired into the sample path.** `POST /api/control` returns HTTP 501. Legacy hardware control banks may be retained in settings for compatibility, but they do not change audio samples. Use route gain or a Gain processor instead.
- **A successful API request is not proof of audible output.** Device activation, graph adoption, persistence, physical routing, and downstream buffering are distinct concerns.
- **The intended deployment is a trusted development/LAN environment.** The current server has no application authentication and exposes sensitive download routes; review [TLS, browser access, and security](#tls-browser-access-and-security) before exposing it to an untrusted network.

## Architecture overview

### Main components

| Component | Responsibility |
|---|---|
| `DeviceManager` | miniaudio context selection, device enumeration, stream activation/deactivation, hardware ownership, and optional PipeWire topology access. |
| `HardwareDevice` | Separate capture/playback stream handling, callbacks, and PCM ring buffers. |
| `ClientManager` | Endpoint identity/NodeId mapping, routes, plugin instances, MIDI graph routes, presets, client sessions, and settings persistence. |
| `DspEngine` | Internal graph construction/order, audio processing, route DSP, plugin-node execution, tone generation, MIDI dispatch, and meters. |
| `ProcessorRegistry` | Compiled-in processor descriptors, port metadata, parameter validation, discovery, and factories. |
| `WebServer` | HTTP API, static dashboard serving, WebSocket sessions, settings/API coordination, and registry bridge. |
| `web_client/dashboard.html` | Main monolithic dashboard and visual routing/plugin-node editor. |

Normal startup creates the device manager and engine, restores saved state, then starts the DSP worker and web services. `--no-audio` bypasses miniaudio context initialization and hardware enumeration; the HTTP/WS application, logical graph, tone source, and client-related code can still be exercised, but physical hardware cannot be activated in that mode.

### Two different graphs

`cm5audio` exposes two graph views which should not be confused:

1. **Application patchbay graph** (`GET /api/graph`): the graph the `DspEngine` uses to route audio between hardware, tone, clients, and plugin-node audio ports. MIDI plugin-to-plugin routes are returned separately in `midi_routes`.
2. **Optional PipeWire topology graph** (`GET /api/pipewire/graph`): an inspection of PipeWire's own nodes and links. It is enabled only when the executable receives `--enable-pipewire-patchbay`. When disabled, the endpoint returns HTTP 200 with `{ "enabled": false, "nodes": [], "links": [] }`; disabled is a normal feature state, not an API failure.

The optional PipeWire topology view does not replace the application's own route graph and does not change the selected miniaudio backend.

## Audio format, timing, and resource limits

The implementation constants in `include/constants.h` are the source of truth for these fixed limits.

| Property | Value in this revision |
|---|---:|
| Language standard | C++17 |
| Internal sample representation | Interleaved 32-bit floating-point PCM |
| Engine sample rate | 48,000 Hz |
| DSP block | **256 frames** (nominally 5.333 ms at 48 kHz) |
| Maximum hardware/application endpoint width | 254 channels |
| Maximum WebSocket-client input/output width | 8 channels each |
| Maximum channel width of a composite plugin chain | 8 channels |
| Maximum typed ports on one processor descriptor | 16 |
| Maximum processors in a route DSP chain | 16 |
| Maximum processors in a composite plugin chain | 16 |
| Maximum application graph node slots | 256, including the tone node slot |
| Route DSP ID range | 1..1023 (slot 0 is not a supported route DSP ID) |
| Audio media staging constant | `CM5_MAX_AUDIO_FRAMES = 4320` (not the DSP block size) |
| MIDI event payload | Up to 16 bytes per event |
| MIDI event queue capacity | 1,024 events per queue |
| MIDI events per output buffer/block | Up to 256 events |

Channel indices in HTTP/settings data are **zero-based**. The dashboard generally labels the first channel `CH1`, corresponding to API channel index `0`.

The engine allocates three float sample-buffer banks for 256 graph node slots, 256 frames, and 254 channels per node. Based on those declared dimensions, the sample backing storage is approximately **190 MiB** (`3 × 256 × 256 × 254 × sizeof(float)`) before vector/object overhead and other application memory. Some comments in the source still contain the older 64-frame/48-MiB estimate; the constants and allocation dimensions above describe the code at this documentation baseline.

The 256-frame block corresponds to about 5.333 ms of samples at 48 kHz. It is **not** an end-to-end latency guarantee. Hardware/backend periods, ring buffering, format conversion, scheduling, independent device clocks, and network transport add latency. The engine's 48 kHz internal rate also does not prove that every physical device is operating at a native 48 kHz mode; miniaudio may perform format/rate conversion.

Hardware endpoints may use widths beyond eight channels, subject to the explicit 254-channel bound and actual backend/device capability. WebSocket clients remain separately capped at eight channels. The graph node/identity pool is finite: tone uses its reserved node slot, leaving at most 255 non-tone endpoint identities for a process lifetime. A client commonly consumes a capture and playback identity. Endpoint identities are retained/reused for the same endpoint identity; unrelated identities are not freely recycled.

## Backends and hardware devices

### miniaudio backend selection

`DeviceManager` supplies this priority list to `ma_context_init`:

1. JACK
2. PulseAudio
3. ALSA

miniaudio selects the first backend that initializes successfully. One context uses one selected backend; it does not merge all three device lists or select a different backend for each activated device. The startup log prints the candidate list and selected backend.

PipeWire is not a separate candidate in this list. PipeWire can expose devices through a PulseAudio-compatible or JACK-compatible server; in that case the selected miniaudio backend is still reported as PulseAudio or JACK. The optional PipeWire topology viewer is separate.

The project strongly recommends PipeWire's PulseAudio compatibility layer for general desktop/device use, but actual behavior depends on the selected backend, server permissions, device visibility, stream format/rate compatibility, channel layout, and hardware configuration. The application does not automatically change PulseAudio/PipeWire profiles, JACK connections/configuration, or ALSA system configuration.

### Enumerating and activating devices

- Enumeration does not automatically activate every device.
- Capture and playback are separate directions. A microphone can be capture-only; a DAC can be playback-only.
- The Hardware Manager groups directions by backend device ID and allows separate `capture_channels` and `playback_channels` requests.
- `0` means let the selected backend choose. It does **not** mean zero channels or necessarily the maximum available width.
- `1..254` explicitly requests a width. Deactivate the device before changing an already-open direction's width.
- Explicit paired channel requests are validated as a pair; activation reports failure rather than pretending that a partial requested pair succeeded. Legacy all-automatic activation may retain a usable direction when its peer fails and reports active directions/warnings.
- The selected backend may negotiate different channel positions/maps. `/api/devices` and `/api/graph` include opened stream layout metadata for diagnostics.
- Device capability probing is an explicit operation and may open/query a hardware device. Routine enumeration does not probe every device.

Device IDs are backend identities, not friendly names. Use IDs returned by `/api/devices`; do not construct IDs from display labels. Existing ALSA/PulseAudio identifiers remain name-based where provided; JACK identifiers use the `jack:` prefix. IDs may differ when the selected backend changes.

### Capability probe

`GET /api/devices/capabilities?id=DEVICE_ID&is_capture=true|false` returns native format tuples. Each tuple is a joint `(format, channels, sample_rate, flags)` observation. Do not combine the maximum channel count from one tuple with the sample rate from another tuple. Unknown/zero tuple fields may represent wildcards or incomplete data; the actual activation validates the request.

## Dependencies and setup

### Native build dependencies

- Linux with a miniaudio-supported backend.
- C++17 compiler (`g++`), GNU Make, and pthreads.
- ALSA development headers/library (`libasound2-dev` on Debian/Ubuntu).
- OpenSSL development headers/libraries (`libssl-dev`).
- zlib development headers/library (`zlib1g-dev`).
- TLS-enabled IXWebSocket headers and matching installed library (the Makefile expects `/usr/local` include/library paths by default).
- nlohmann/json headers. The Makefile defaults to `../json/include` and supports `NLOHMANN_JSON_INCLUDE`.

miniaudio and cpp-httplib are vendored in `include/`. The nlohmann/json checkout and the full IXWebSocket distribution are external dependencies. PipeWire/SPA development packages are required for the build paths using PipeWire headers. Emscripten is needed only for the optional browser audio bundle. Node.js is used by dashboard tests; Python 3 and the OpenSSL CLI are used by process-level reliability tests.

The Makefile links ALSA, PipeWire, IXWebSocket, OpenSSL, zlib, RNNoise, and pthreads. It also compiles three local IXWebSocket compatibility translation units under `third_party/ixwebsocket/` (`IXSocketOpenSSL.cpp`, `IXSocketServer.cpp`, and `IXWebSocketServer.cpp`) against the installed IX headers/library. These are project-specific compatibility sources, not a general replacement for matching IXWebSocket headers and library ABI.

### Debian/Ubuntu prerequisite starting point

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config cmake git \
    libasound2-dev libpipewire-0.3-dev libspa-0.2-dev \
    libssl-dev zlib1g-dev nlohmann-json3-dev
```

Install/configure the matching TLS-enabled IXWebSocket development library as required by the machine. If the distribution's nlohmann/json headers are used instead of a sibling checkout:

```bash
make -j7 NLOHMANN_JSON_INCLUDE=/usr/include
```

### Setup helper

From the repository root:

```bash
./scripts/setup.sh --install-ixwebsocket --no-wasm --host YOUR_SERVER_IP_OR_DNS
```

The helper checks/installs some prerequisites, prepares development TLS files, and can build the native executable. Without `--no-wasm`, it also builds the optional browser audio bundle. The helper's package list does not replace checking all native prerequisites (notably ALSA and JSON development headers). It can modify the machine by installing packages/libraries and creating or regenerating TLS files; it is not required for every rebuild.

Useful helper options:

| Option | Purpose |
|---|---|
| `--no-install` | Skip package installation while retaining tool checks. |
| `--no-build` | Skip the native build. |
| `--no-wasm` | Skip the browser audio bundle. |
| `--install-ixwebsocket` | Install IXWebSocket if the expected package is not found. |
| `--host IP_OR_DNS` | Set the development certificate subject/SAN. |
| `-h`, `--help` | Show helper usage. |

The standalone installer is `./scripts/install_ixwebsocket.sh`. It defaults to IXWebSocket `v12.0.1`, enables OpenSSL TLS/zlib, and installs under `/usr/local`; `IXWEBSOCKET_VERSION` and `IXWEBSOCKET_PREFIX` can override this. A custom prefix requires corresponding include/link configuration; the Makefile does not automatically discover arbitrary prefixes.

## Build and verification

### Build the native application

From the repository root:

```bash
make -j7
```

Choose parallelism based on available memory. The main output is `./cm5audio`. For a full rebuild:

```bash
make clean
make -j7
```

`make clean` removes application objects/dependency files, the main executable, plugin/channel/reliability test artifacts, and local IX compatibility objects. It intentionally retains `build/miniaudio_impl.o`; use `make clean-miniaudio` if that object must be rebuilt after changing miniaudio headers/macros/compiler settings:

```bash
make clean-miniaudio
make -j7
```

The Makefile emits dependency files under `build/` so header changes are tracked. If a downloaded archive produces clock-skew warnings, normalize timestamps with `touch` on the replaced files before interpreting incremental-build results. `make -B -j7` forces a rebuild of all targets.

### Optional browser audio bundle

```bash
./web_client/build_wasm.sh
```

Requires `emcc` in `PATH`. It creates `web_client/dist/client_audio.js` and `web_client/dist/client_audio.wasm`. These generated files are ignored by Git and must be rebuilt on a fresh checkout before using the browser audio terminal. The main native dashboard itself is plain HTML/CSS/JavaScript and does not require WASM.

### Test targets

```bash
make test-plugins
make test-channels
make test-reliability
```

| Target | Coverage / constraints |
|---|---|
| `test-plugins` | C++ registry, parameter validation, processor behavior, graph/plugin lifetime, HTTP discovery/rejection/persistence paths, and Node.js dashboard DOM-logic tests. Uses temporary settings, loopback ports 28182/28183, and `DeviceManager(false)`; it does not initialize a physical audio backend. Node.js is required for its dashboard test. |
| `test-channels` | Synthetic/manual hardware-callback and DSP-block tests at widths 1, 2, 3, 6, 8, 12, 16, 24, 32, and 254; checks channel mapping, routing, fan-out/summing, meters, partial reads/silence, bounds, and same-identity reopen behavior. It does not open a physical audio device. |
| `test-reliability` | Builds the application and reliability/device lifecycle fixtures, then runs the C++ reliability test, null-device lifecycle tests, and `tests/shutdown_test.py`. Requires Python 3 and OpenSSL CLI for process/TLS checks. |

These are focused software/synthetic tests, not a substitute for validating actual physical connectors, channel mapping, backend clock behavior, browser display, or end-to-end latency on the target system.

## Running and configuration

Start the executable from the repository root so the relative dashboard/static-asset paths resolve correctly.

### Common launch commands

Plain HTTP and WebSocket:

```bash
./cm5audio --plain-text
```

Default behavior (TLS is selected when both configured certificate and key are readable):

```bash
./cm5audio
```

Without the optional PipeWire topology viewer:

```bash
./cm5audio --plain-text
```

With the optional PipeWire topology viewer:

```bash
./cm5audio --plain-text --enable-pipewire-patchbay
```

Without initializing miniaudio or enumerating physical audio devices:

```bash
CM5AUDIO_SETTINGS=/tmp/cm5audio-isolated-settings.json \
CM5AUDIO_BIND_ADDRESS=127.0.0.1 \
CM5AUDIO_HTTP_PORT=28182 CM5AUDIO_WS_PORT=28183 \
./cm5audio --no-audio --plain-text
```

`--no-audio` is useful for isolated HTTP/API testing. It cannot activate physical devices or qualify audio output.

### Command-line options

| Option | Effect |
|---|---|
| `-h`, `--help` | Print usage and exit. |
| `--plain-text` | Force plain HTTP/WS instead of selecting TLS. |
| `--no-audio` | Skip audio context initialization and hardware enumeration. |
| `--enable-pipewire-patchbay` | Enable the optional PipeWire topology viewer. |

Unknown arguments fail startup. There are no command-line flags for HTTP/WS ports or the audio backend.

### Default listener URLs

Replace `HOST` with the server's reachable address.

| Service | Plain text | TLS |
|---|---|---|
| Main dashboard | `http://HOST:8182/` | `https://HOST:8182/` |
| Dashboard static mount | `http://HOST:8182/client/dashboard.html` | `https://HOST:8182/client/dashboard.html` |
| Browser audio terminal | `http://HOST:8182/client/index.html` | `https://HOST:8182/client/index.html` |
| Auxiliary scan page | `http://HOST:8182/client/scan.html` | `https://HOST:8182/client/scan.html` |
| Audio WebSocket | `ws://HOST:8183/ws/audio` | `wss://HOST:8183/ws/audio` |

The server defaults to binding `0.0.0.0`, not loopback only.

### Runtime environment variables

| Variable | Default | Purpose |
|---|---|---|
| `CM5AUDIO_HTTP_PORT` | `8182` | HTTP(S) listener. `0` requests an ephemeral HTTP port. |
| `CM5AUDIO_WS_PORT` | `8183` | WS(S) listener. Use a nonzero port; do not rely on discovering an ephemeral WS port. |
| `CM5AUDIO_BIND_ADDRESS` | `0.0.0.0` | Bind address shared by HTTP and WS listeners. Use `127.0.0.1` for loopback-only tests. |
| `CM5AUDIO_SETTINGS` | `./cm5audio_settings.json` | Settings file load/save path. |
| `CM5AUDIO_TLS_CERT` | `./certs/server.crt` | TLS server certificate and certificate download route. |
| `CM5AUDIO_TLS_KEY` | `./certs/server.key` | TLS private key and private-key download route. |
| `CM5AUDIO_TLS_P12` | `./certs/server.p12` | PKCS#12 download path; not required to enable TLS. |

Setup-only variables include `CM5AUDIO_HOST` (development certificate subject/SAN; default `192.168.168.172`) and `CM5AUDIO_P12_PASSWORD` (setup bundle password; default `cm5audio`). They do not select the audio backend, change the engine sample rate, or set the native listener bind address.

Without `--plain-text`, TLS is selected when both certificate and key files are readable; otherwise the server falls back to plain HTTP/WS. Readable but invalid credentials can fail startup. Check startup logs and actual listening sockets:

```bash
ss -ltnp | grep -E ':8182|:8183'
```

Only one normal instance should own the default ports and settings file at a time.

## Dashboard workflow

The main user interface is `web_client/dashboard.html`, served at `/`. It is intentionally a monolithic HTML/CSS/JavaScript application. Plugin parameter controls and port declarations are generated from the backend registry rather than being duplicated as effect-specific JavaScript definitions.

### Patchbay

1. Open **Hardware & DSP** and activate the capture/playback devices you intend to use.
2. Return to **Patchbay**. Capture endpoints are sources; playback endpoints are destinations. The tone generator is a mono source. Plugin nodes expose their declared audio input/output ports.
3. Connect channels by dragging from a source port/channel to a destination port/channel, or use the dashboard's source-then-destination selection interaction.
4. One source channel can feed multiple destinations. Multiple routes can sum into one destination channel. The engine does not automatically normalize sums.
5. Move endpoint/plugin nodes by dragging their headers. Pan and zoom the viewport using its canvas controls.
6. Click a cable to open **Route Inspector** for that specific route. Route gain, enabled state, and route DSP are edited as a local draft.
7. Click **APPLY** to validate and publish the whole route edit. Closing the inspector or clicking outside before APPLY discards the local draft.
8. Use top-level **SAVE** to write accepted server state. SAVE does not read, apply, or serialize unsubmitted browser drafts.

Node positions are browser-local (`localStorage`), not part of `cm5audio_settings.json`. Different browsers or cleared site data do not inherit the same viewport layout.

### Individual plugin nodes

Use the plugin-node creation UI to instantiate a registered processor as an independent graph node. Each node has its own instance ID, parameters, active/bypass state, and declared ports. Audio connections attach to the plugin's audio ports; MIDI connections use the separate MIDI route model.

Plugin-node configuration controls are generated from registry metadata. An individual plugin node is not the same as an entry inside a composite plugin-chain node.

### Route Inspector conversion

- **Extract processor / add as node:** removes one selected processor from a route's DSP chain and creates an individual plugin node between the route's remaining upstream and downstream DSP sections.
- **Upgrade entire chain to node:** converts the selected route's full ordered DSP chain into one composite plugin-chain node, splitting the original route into upstream and downstream audio connections.
- A route is one source-channel-to-destination-channel edge. Therefore, converting one route produces a one-channel chain node. The conversion does not automatically group two separate mono routes into a stereo chain; create or configure a multichannel chain node and explicitly connect each channel when stereo processing is required.

### Composite plugin-chain nodes and presets

A composite chain node has a single serial audio chain and a symmetric width from 1 to 8 channels. Every processor in the chain is configured according to its `chain_channel_mode` declaration:

- `per_channel`: an independent mono processor instance/state is created for each channel.
- `multichannel`: one processor instance receives the entire interleaved bus.
- `mono_only`: the chain must have exactly one channel; wider chains are rejected.

The UI supports editing the chain's processors/parameters, order, chain gain, active state, and channel count. Chain presets are independent saved definitions: they can be edited, instantiated as new nodes, or applied to replace an existing chain node's internal chain. Deleting a preset does not delete live nodes previously instantiated from it; deleting a live node does not delete its saved presets.

See [`docs/PLUGINS.md`](docs/PLUGINS.md) for implementation and compatibility rules.

## Routing model

### Endpoint identifiers

Application graph endpoint strings use these forms:

```text
hardware/<backend-device-id>/capture
hardware/<backend-device-id>/playback
client/<client-id>/capture
client/<client-id>/playback
tone/generator
plugin/<plugin-instance-id>/<audio-port-id>
```

Use the exact endpoint IDs returned by `/api/graph`. The display name is not an identifier. Plugin input ports are destinations; plugin output ports are sources. MIDI plugin-port connections use `plugin/<id>/<midi-port-id>` but are represented in the separate `midi_routes` collection rather than as audio channel routes.

### Channel mapping, fan-out, and mixing

`POST /api/routes` creates a connection between one source channel and one destination channel. Channel indices are zero-based. There is no automatic `capture CH1 -> playback CH1` mapping; configure every connection explicitly.

- Fan-out: the same source endpoint/channel can be connected to multiple destination endpoint/channels.
- Summing: multiple source routes can target one destination channel. Samples are summed; the engine does not apply an automatic normalization/limiter just because routes converge.
- Gain: route gain is a linear amplitude multiplier, finite and bounded from 0 to 8. `1.0` is unity, `0.0` is silence.
- Direction and width: a source must be a source endpoint, a destination must be a destination endpoint, and channel indices must fit the known endpoint width. The 254-channel index bound corresponds to `0..253`.
- Duplicated identical source-channel/destination-channel routes are rejected.

The application graph is scheduled using a topological order. **Enabled graph cycles are unsupported.** Existing feedback buffers are not general cycle support; cyclic nodes may be omitted from the execution order. Avoid creating feedback loops.

### Route DSP versus graph-native processing

A route DSP chain is stored on a route and processes that route's single source channel as mono. Its conceptual signal path is:

```text
source endpoint/channel
    -> route DSP processors in order (when active)
    -> DSP master gain (when active)
    -> ordinary route gain
    -> sum into destination endpoint/channel
```

When route DSP is bypassed, the processor chain and DSP master gain are bypassed; ordinary route gain remains active. Route gain and DSP master gain multiply each other.

A composite plugin-chain node is a separate graph node, not a special route DSP setting. Its audio input and output can have multiple channels, it owns its ordered internal processor chain, and its output can fan out to multiple destinations. Use a composite chain node for stereo/multichannel processing rather than trying to make one route DSP chain span several unrelated channel routes.

A separate fixed global Gain + Compressor stage operates on **hardware playback** after mixing. Its default values are gain `1.0`, threshold `0.7` (linear amplitude), and ratio `4`. The compressor state is shared across hardware playback calls; it is not an independently instantiated dynamics processor for every hardware channel/device. This fixed stage does not run on client playback and is separate from the registry-backed route/plugin processors.

## DSP processors and plugin chains

The registry is compiled into the native executable. It currently registers these 22 types:

| Type | Display name / purpose |
|---|---|
| `passthrough` | No-op sample processor. |
| `gain` | Linear amplitude multiplier. |
| `compressor` | Hard-knee compressor; threshold is linear amplitude, with smoothed gain reduction. |
| `echo` | Delay/echo effect. |
| `chorus` | Modulated chorus effect. |
| `reverb` | Reverb effect. |
| `lush_reverb` | Extended/lusher reverb implementation. |
| `eq` | Equalizer. |
| `autotune` | Pitch-correction processor. |
| `vowel_filter` | Vowel/formant-style filter. |
| `soft_clipper` | Soft clipping/saturation. |
| `hpf` | High-pass filter. |
| `lpf` | Low-pass filter. |
| `deesser` | De-essing processor. |
| `normalize` | Automatic gain control/level normalization. |
| `expander` | Downward expansion. |
| `noise_gate` | Noise gate. |
| `bitcrusher` | Bit-depth/rate-reduction effect. |
| `limiter` | Look-ahead limiter. |
| `midi_pitch_shifter` | MIDI-controlled audio pitch shifting. |
| `midi_harmonizer` | MIDI-controlled harmonizer. |
| `rnnoise` | RNNoise-based denoising processor. |

The registry's runtime discovery endpoint (`GET /api/dsp/available`) is authoritative for the exact compiled list, parameter defaults/ranges, port layouts, and channel mode of the running binary. A type listed above does not imply that every conceivable port layout is supported; use discovery.

### Parameter values and validation

The supported descriptor parameter types are:

- `number`
- `integer`
- `boolean`
- `string`
- `enum`

Numeric descriptors can specify minimum, maximum, positive UI step, and a display-only unit. Units are not converted by the backend. A step is a UI increment, not a promise that the backend quantizes values to that step. Numeric values must be finite and within declared bounds; integer values must be whole. Enum values must match a declared choice and retain their JSON type. Unknown parameter keys and invalid types/ranges are rejected rather than silently clamped or ignored.

**Compressor thresholds and other linear-amplitude controls are not dBFS values.** For example, -20 dBFS corresponds to an amplitude value of `0.1`, using `10^(dBFS/20)`. Do not pass `-20` to a parameter whose descriptor range is `0..1`.

### Chain channel modes

A processor descriptor declares one of three `chain_channel_mode` values that control how the processor is embedded in a composite plugin-chain node:

| Mode | Composite chain behavior |
|---|---|
| `per_channel` | Create one independent mono processor instance/state for every chain channel. The processor receives one channel at a time. This is the default for descriptors that do not explicitly declare a mode. |
| `multichannel` | Create one processor instance and call it with the full interleaved chain buffer and actual channel count. The processor implementation must correctly process interleaved samples and preserve intended cross-channel linking. |
| `mono_only` | Reject the composite chain if its channel count is greater than one. |

In this revision, `gain`, `compressor`, `normalize`, `limiter`, and `hpf` explicitly declare `multichannel`. `rnnoise` declares `mono_only`. Other processors use the descriptor's declared mode; descriptors default to `per_channel` unless they override it. Check discovery rather than assuming a processor is multichannel merely because its implementation loops over a `channels` argument.

Multichannel does not always mean linked stereo behavior. The processor implementation defines that behavior. For example, the compressor calculates a frame peak across the entire bus and applies one smoothed gain value to every channel in that frame; Normalize shares its envelope/gain state across channels. A `per_channel` compressor configuration uses independent mono instances/state, so it is not linked stereo compression.

### Composite chain limits and behavior

- Channel count is 1..8 and is symmetric for the chain's main audio input and output.
- The ordered chain must contain at least one processor and no more than 16.
- Chain gain must be finite and within 0..8.
- Every processor type and parameter object is validated by the registry.
- A `mono_only` processor makes any chain width greater than one invalid.
- `chain_gain` is applied after the internal processor stages.
- Each channel follows the same processor order; `per_channel` stages maintain independent per-channel state, while `multichannel` stages receive the complete interleaved bus.
- Existing chain presets without a `channels` value default to mono when restored, preserving compatibility with older settings.

### Presets and live nodes

Presets are stored in `plugin_chain_presets` and live node instances in `plugin_nodes`. They are separate collections. Editing a preset does not automatically mutate already-created nodes. Instantiate a preset to create a new node, or use the replace-preset operation to replace an existing chain node while retaining the node identity and its graph cabling where valid. Deleting a preset does not delete live nodes instantiated from it; deleting a live node does not delete the saved preset.

See [`docs/PLUGINS.md`](docs/PLUGINS.md) for the processor interface, port descriptors, channel-mode rules, MIDI event queues, registration steps, and plugin-specific constraints.

## MIDI support

### Typed MIDI ports and graph routes

A processor descriptor may declare MIDI input and/or output ports. MIDI is event data, not a channel-count audio bus. The native graph supports routes from a declared plugin MIDI output to a declared plugin MIDI input.

Endpoint names follow:

```text
plugin/<plugin-instance-id>/<midi-port-id>
```

Use `GET /api/plugins`, `GET /api/graph`, or `GET /api/midi/routes` to inspect declared ports and configured MIDI graph routes. `POST /api/midi/routes` requires a valid declared MIDI output source and MIDI input destination. MIDI graph connections are not audio routes and do not appear in the ordinary `.routes` array.

### Event representation and bounds

`MidiEvent` preserves the event timestamp/value field, a protocol marker (`1` for MIDI 1.0, `2` for UMP), a payload size, and up to 16 payload bytes. The bounded `MidiEventQueue` is a multi-producer/single-consumer queue with capacity 1,024 and a dropped-event counter. A `MidiOutputBuffer` has fixed capacity (up to 256 events per block in the current constants). This is bounded event transport, not an unrestricted MIDI stream or a full DAW MIDI scheduler.

### Physical MIDI-device boundary

The MIDI Pitch Shifter and MIDI Harmonizer still own their current PipeWire MIDI discovery/input services. Their source-selection parameters are compatibility controls for those existing services; they are not evidence that physical MIDI devices are already generic graph endpoints. The graph can route events between plugin MIDI ports, and the existing services can feed their plugin's input queue, but a centralized physical MIDI device manager/source-node system is not implemented in this revision.

See `docs/PLUGINS.md` for event-buffer usage and requirements for MIDI-aware processors.

## Tone generator, global DSP, and meters

### Tone generator

The tone generator is a mono source endpoint named `tone/generator`, channel index `0`. It generates a sine wave with:

- `enabled`: enabled/disabled state.
- `frequency_hz`: 1..20,000 Hz.
- `amplitude`: 0..1 linear peak amplitude.

Use `GET /api/tone` to read authoritative values. `POST /api/tone` accepts query/form parameters `enabled`, `frequency`, and `amplitude`; the POST field is called `frequency`, while GET/settings use `frequency_hz`. Accepted tone changes are applied and automatically persisted by the server wrapper after engine adoption.

### Global DSP

`GET/POST /api/dsp` controls the fixed hardware-playback Gain + Compressor stage (gain, threshold, ratio). It is not a dynamic registry processor chain and does not affect WebSocket-client playback. The global DSP object is stored in settings under `global_dsp`; use top-level SAVE after changing it if the endpoint does not report persistence.

### Meter API

`GET /api/meters` and its alias `GET /api/metrics` return a structure with endpoint and plugin meter collections:

```json
{
  "capture": {
    "hardware/<device-id>/capture": [
      {
        "ch": 0,
        "raw_value": 0.1,
        "raw_peak": 0.2,
        "rms_db": -20.0,
        "peak_db": -14.0,
        "peak_hold_db": -12.0,
        "clipped": false
      }
    ]
  },
  "playback": {},
  "tone": [],
  "plugins": {
    "<plugin-instance-id>": {
      "audio_in": [],
      "audio_out": []
    }
  }
}
```

Actual numeric values depend on the audio signal. Meter entry fields:

| Field | Meaning |
|---|---|
| `ch` | Zero-based channel index. |
| `raw_value` | RMS amplitude for the last measured block. |
| `raw_peak` | Absolute peak amplitude for the last measured block. |
| `rms_db` | RMS level in dBFS, with a low-level floor used by the implementation. |
| `peak_db` | Peak level in dBFS, with the same low-level floor. |
| `peak_hold_db` | Maximum peak level retained by the meter until reset/reinitialization. |
| `clipped` | Whether the most recently measured block exceeded absolute sample value 1.0; it is not a permanently latched clip-history flag. |

Tone has one meter channel. Plugin meters are grouped by plugin instance ID and audio port ID. The ordinary `capture` and `playback` maps are keyed by endpoint string. `/api/raw` provides a reduced capture/playback-only view with `ch`, `value`, and `peak`. `POST /api/reset_clips` resets clip/peak-hold state; use the endpoint/type/channel selectors or `all` according to the handler contract.

Meter polling in the dashboard is visual telemetry and is not the DSP block interval. Meter maps use synchronization; the values are atomically sampled, but the whole telemetry API should not be described as lock-free.

## HTTP API reference

**Base URL:** `http://HOST:8182` in plain mode or `https://HOST:8182` in TLS mode. API responses are JSON. The server does not provide application authentication.

Unless stated otherwise, POST handlers read URL query/form parameters through cpp-httplib. Endpoints explicitly described as **JSON body** require JSON. Sending JSON to a query/form endpoint is not equivalent.

### General completion fields

Mutating POST handlers are serialized by the shared control mutex and request an engine revision. Where the handler uses the shared adoption wrapper, it waits up to 1,500 ms for a completed DSP block to acknowledge that revision.

| Field | Meaning |
|---|---|
| `accepted_revision` | Engine revision requested for this mutation. |
| `adopted_revision` | Highest engine revision acknowledged after a completed processing block. |
| `adopted` | Whether this mutation's target revision was confirmed adopted. |
| `persisted` | Whether this request also wrote the settings file successfully. |
| `persisted_revision` | Process-local revision most recently saved by this WebServer. |

A normal APPLY can return HTTP 200 with `adopted:true` and `persisted:false`: this means live state was applied but not saved. A 503 `pending` response means the request was accepted but adoption was not confirmed within the timeout; **the request may still apply later**. It is not a rollback/cancellation. Refresh `/api/graph` and the relevant resource before retrying. A 500 response after `adopted:true` means live state was applied but persistence failed; fix the storage issue and retry SAVE. Revisions are process-local progress counters, not settings schema versions, ETags, physical-device flush acknowledgements, or compare-and-swap tokens.

### Pages and static assets

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Main dashboard HTML. |
| GET | `/favicon.ico` | Dashboard icon. |
| GET | `/client/*` | Broad static mount of `./web_client/`. |
| GET | `/client/index.html` | Browser audio terminal. |
| GET | `/client/scan.html` | Auxiliary device scan page. |
| GET | `/client/dist/client_audio.js` | Generated WASM bridge JS, if built. |
| GET | `/client/dist/client_audio.wasm` | Generated WASM module, if built. |
| GET | `/server.crt` | Configured certificate download. |
| GET | `/server.key` | Configured **private key download**, unauthenticated in this revision. |
| GET | `/server.p12` | Configured PKCS#12 bundle download, potentially containing the private key. |

The `/client/*` mount is a directory mount, not a strict whitelist. Files added under `web_client/` can become downloadable.

### Device and client APIs

| Method | Path | Parameters / response |
|---|---|---|
| GET | `/api/devices` | Enumerated devices with backend, active directions, widths, and opened layout metadata where available. |
| GET | `/api/devices/capabilities` | Required `id`, optional `is_capture`; performs an explicit capability probe that may open/query the device. |
| POST | `/api/devices/activate` | Required `id`; optional `capture_channels` and `playback_channels`; `0` automatic or `1..254` explicit. Saves after adoption. |
| POST | `/api/devices/deactivate` | Required `id`; deactivates matching directions, preserves logical endpoint IDs/routes, then saves after adoption. |
| GET | `/api/clients` | Active WebSocket client sessions and their input/output channel counts. |
| GET | `/api/graph` | Application graph: `revision`, `adopted_revision`, `persisted_revision`, `endpoints`, `routes`, `plugin_nodes`, `midi_routes`. |
| GET | `/api/pipewire/graph` | Optional PipeWire topology. Returns `enabled:false` with empty arrays when CLI-disabled; actual topology when enabled. |

Device activation requests with a changed explicit width are rejected while the device is active; deactivate it first. Capability tuples are advisory; the actual open remains authoritative.

### Audio route APIs

| Method | Path | Parameters / response |
|---|---|---|
| POST | `/api/routes` | Form/query: `source_node`, `destination_node` endpoint strings, `source_channel`, `destination_channel`; optional `gain`, `enabled`. |
| POST | `/api/routes/update` | Legacy route-only form/query update: required `id`, optional/defaulted `gain`, `enabled`. Omitted values use handler defaults rather than preserving unspecified old values. |
| GET | `/api/graph` | Use this for the route list; there is no separate `GET /api/routes` route-list endpoint. |
| DELETE | `/api/routes/{id}` | Remove route and cached route DSP state; adoption is awaited. |
| GET | `/api/routes/{id}/dsp` | Read `{active,gain,chain}` and optional warning for an invalid/bypassed stored chain. |
| POST | `/api/routes/{id}/dsp` | **JSON body** replacing the complete route DSP configuration. |
| POST | `/api/routes/{id}/apply` | **JSON body** `{gain,enabled,dsp}`; validates and applies ordinary route state plus complete DSP draft as one request. |

`POST /api/routes` creates a route using exact endpoint strings and zero-based channels. Route create rejects duplicate endpoint/channel edges, invalid directions, out-of-range indices, unknown endpoints, invalid gain, and exhausted IDs. Route IDs are finite; route DSP supports IDs 1..1023.

Whole route APPLY JSON example:

```json
{
  "gain": 0.8,
  "enabled": true,
  "dsp": {
    "active": true,
    "gain": 1.0,
    "chain": [
      {"type": "gain", "params": {"gain": 0.9}},
      {"type": "compressor", "params": {"threshold": 0.1, "ratio": 4.0}}
    ]
  }
}
```

All outer fields are required by the whole-route APPLY handler. `dsp` replaces the complete route DSP configuration, not a partial patch. Each chain item requires a known `type` and a `params` object; missing parameter keys receive descriptor defaults. Invalid types, unknown parameter keys, wrong JSON types, non-finite numbers, and out-of-range values are rejected. The maximum processor count is 16.

Route DSP-only example:

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

### Processor discovery and plugin-node APIs

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/dsp/available` | No-store registry discovery, schema version 2, processor parameters/ports/channel modes and resource limits. |
| GET | `/api/plugins` | `{plugin_nodes, midi_routes}`. |
| POST | `/api/plugins` | **JSON body** create a registered processor instance; can supply `type`, `name`, `params`, and either `input_channels`/`output_channels` or `port_channels`. |
| POST | `/api/plugins/{id}` | **JSON body** update instance name, parameters, active state, and channel/port widths. |
| DELETE | `/api/plugins/{id}` | Remove plugin node; waits for adoption and automatically saves. |
| POST | `/api/plugins/extract` | **JSON body** `{route_id,processor_index}`: extract one processor from a route DSP chain into an individual node. |
| POST | `/api/plugins/extract-chain` | **JSON body** `{route_id,name?,channels?}`: convert the selected route's complete DSP chain into a composite plugin-chain node. |
| POST | `/api/plugins/chains` | **JSON body** `{chain,name?,chain_gain?,active?,channels?}`: create a composite plugin-chain node. |
| POST | `/api/plugins/{id}/chain` | **JSON body** replacing an existing composite node's chain/configuration. |
| POST | `/api/plugins/{id}/replace-chain-preset` | **JSON body** `{preset_id}`: replace a live chain node's internal chain from a saved preset. |

For a standard plugin node, use `port_channels` to set named audio port widths when the processor declares more than the legacy primary pair. The registry descriptor is authoritative; unknown port IDs or invalid widths are rejected.

Example standard node creation:

```json
{
  "type": "gain",
  "name": "Stereo Gain",
  "params": {"gain": 0.75},
  "input_channels": 2,
  "output_channels": 2
}
```

Composite chain creation example:

```json
{
  "name": "VOIPProc",
  "channels": 2,
  "active": true,
  "chain_gain": 1.0,
  "chain": [
    {"type": "compressor", "params": {"threshold": 0.5, "ratio": 4.0}},
    {"type": "normalize", "params": {"target": 0.25, "speed": 0.1, "max_gain": 4.0}},
    {"type": "limiter", "params": {"threshold": 0.9, "attack": 0.005, "release": 0.05, "lookahead": 20}}
  ]
}
```

The chain endpoint validates every processor and the composite width before publication. `channels` is the symmetric main audio bus width and must be 1..8.

### Plugin-chain preset APIs

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/plugin-chain-presets` | `{presets:[...]}`. |
| POST | `/api/plugin-chain-presets` | **JSON body** create a preset: required `name` and `chain`; optional `chain_gain`, `active`, `channels`. |
| POST | `/api/plugin-chain-presets/{id}/update` | **JSON body** partial preset update; omitted fields keep their current values. |
| POST | `/api/plugin-chain-presets/{id}/instantiate` | Create a new live chain node from the preset; optional JSON `name` override. |
| DELETE | `/api/plugin-chain-presets/{id}` | Delete the preset definition; does not delete existing live nodes. |

Preset operations are persisted by the server after validation/adoption. Preset edits do not automatically rewrite live nodes already created from the preset. Use `/api/plugins/{id}/replace-chain-preset` to apply a chosen preset to an existing chain node. That operation retains the node ID and existing graph connections where the replacement remains compatible with the port layout.

### MIDI graph APIs

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/midi/routes` | `{midi_routes:[...]}` current plugin-to-plugin MIDI connections. |
| POST | `/api/midi/routes` | **JSON body** `{source,destination}`; source must be a declared MIDI output and destination a declared MIDI input. |
| DELETE | `/api/midi/routes/{id}` | Remove a MIDI graph connection. |

Example:

```json
{
  "source": "plugin/4/midi_out",
  "destination": "plugin/6/midi_in"
}
```

### Tone, fixed global DSP, meters, and settings

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/tone` | Current tone `{enabled,frequency_hz,amplitude}`. |
| POST | `/api/tone` | Query/form `enabled`, `frequency`, `amplitude`; automatically saves after adoption. |
| GET | `/api/dsp` | Current fixed global hardware-playback `gain`, `threshold`, `ratio`. |
| POST | `/api/dsp` | Query/form update of the fixed global controls; use top-level SAVE to persist. |
| GET | `/api/meters` | Capture/playback/tone/plugin meter banks. |
| GET | `/api/metrics` | Alias of `/api/meters`. |
| GET | `/api/raw` | Reduced capture/playback raw RMS/peak view. |
| POST | `/api/reset_clips` | Reset clip/peak-hold state (`all` or endpoint/type/channel selectors). |
| POST | `/api/control` | Returns HTTP 501; hardware gain/mute is not wired into samples. |
| POST | `/api/settings/save` | Wait for engine adoption, then serialize the server's accepted state. |

`POST /api/settings/save` does not apply browser drafts. It saves state already accepted by the backend and adopted by the engine. A 200 response should include `adopted:true` and `persisted:true`; a pending adoption is not saved, and a write failure after adoption is reported as an error.

## WebSocket audio protocol

**Default port:** 8183. The bundled browser terminal conventionally connects to `/ws/audio`; the server callback does not enforce only that literal path. Use an explicit nonzero port. TLS mode uses WSS.

### Text identity message

Client sends a JSON `hello` message:

```json
{
  "type": "hello",
  "client_id": "stable-client-id",
  "client_name": "Desk microphone",
  "input_channels": 1,
  "output_channels": 2
}
```

- `input_channels` means **client to server** (the client's capture stream).
- `output_channels` means **server to client** (the client's playback stream).
- Each count is clamped/validated to the supported 1..8 range; zero-channel stream modes are not supported.
- `client_key` and `name` are accepted aliases for `client_id` and `client_name`.

The server can send `assigned`, `identity_accepted`, `identity_conflict`, and `media_ready` messages. Application session creation occurs after a successful WebSocket upgrade/open event, not merely after a TCP accept. The browser terminal stores client identities in `localStorage`; identity collision handling is session naming/coordination, **not authentication**.

The first valid hello establishes rings and endpoint identity. Binary media before `media_ready` is ignored. A ready session cannot be resized or renamed in place by sending a different identity/channel configuration; reconnect with a new session when changing those values.

### Binary media

Binary payloads are raw interleaved float32 PCM. There is no application header/version, sequence number, timestamp, stream ID, byte-order marker, or explicit frame count. The current deployment assumes compatible float representation/byte order and a 48 kHz engine. The bundled WASM terminal captures in 480-frame/10 ms units; this is separate from the native engine's 256-frame processing block. The terminal may send `sample_rate` and `format` hello metadata, but the current server does not perform a full rate/format negotiation or compensate for clock drift.

There is no application-level network jitter buffer, backpressure policy, sequence-loss recovery, or clock-drift compensation. Do not equate successful WebSocket delivery with bounded low-latency media under arbitrary network conditions.

## Settings persistence and restoration

Default settings path: `./cm5audio_settings.json`, overridden by `CM5AUDIO_SETTINGS`.

The settings file is runtime state, not a portable/versioned project interchange format. The current top-level `version` marker is `1`; it is not a comprehensive migration framework.

| Top-level key | Contents |
|---|---|
| `version` | Current settings marker (`1`). |
| `activated_devices` | Backend device IDs the application should attempt to activate at startup. |
| `device_streams` | Optional per-device requested capture/playback channel counts. Missing entries use automatic selection. |
| `tone` | Tone generator `enabled`, `frequency_hz`, and `amplitude`. |
| `global_dsp` | Optional fixed hardware-playback DSP `gain`, `threshold`, and `ratio`. |
| `routes` | Route IDs, source/destination endpoint strings, source/destination channel indices, gain, enabled state, and optional route DSP object. |
| `plugin_nodes` | Live plugin instances: IDs, type/name, active state, parameters, audio port widths; chain nodes also include chain, chain gain, and channel width. |
| `plugin_chain_presets` | Named reusable chain definitions, parameters, chain gain, active state, and channel count. |
| `midi_routes` | Plugin MIDI output-to-input endpoint connections. |
| `clients` | Known client IDs mapped to display names. |
| `hardware.capture`, `hardware.playback` | Legacy per-channel gain/mute banks retained for compatibility; not applied to samples in this revision. |

The serializer builds a snapshot and writes a unique same-directory `PATH.tmp.XXXXXX` file using `mkstemp`, checks the full write loop, `fsync`s and closes the file, then renames it over the destination. A failed write/rename reports failure and cleans up the temporary file where possible. The existing file remains in place until successful rename.

Durability and ownership limitations:

- There is no parent-directory `fsync`, automatic previous-good snapshot selection, `.bak` recovery, or crash/power-loss rollback.
- The in-process mutex coordinates one server instance only. Use one process as the owner of a settings path.
- Keep your own backups before migrations or offline edits.
- Settings loading occurs at startup; there is no live settings-file reload/import endpoint. Stop the process, edit carefully, then restart.
- Invalid/unavailable saved processor definitions may be skipped or retained as invalid/bypassed route DSP with a warning, depending on the state type. Discovery does not rewrite the file merely because a parameter/type is no longer valid.
- Disconnected device/client endpoints and their routes can remain logically registered. A reconnect with the same identity can rebind the existing endpoint. Routes that do not fit the currently active width are retained but cannot process until a compatible width is available.

### What is automatically saved?

The current API has path-specific persistence behavior. In broad terms:

- Tone updates automatically save after engine adoption.
- Device activation/deactivation save after adoption.
- Plugin-chain preset create/update/delete operations save.
- Composite chain-node create/update/replace and chain extraction operations save.
- Plugin-node deletion saves after adoption.
- Ordinary route changes, route DSP changes, fixed global DSP changes, and ordinary non-chain plugin-node create/update operations require top-level SAVE if they are to survive restart.

Check the response's `persisted` field rather than assuming every accepted mutation was written. A successful `POST /api/settings/save` writes accepted engine state, not the dashboard's unsaved drafts.

Browser-local viewport positions and local client identity storage are not included in `cm5audio_settings.json`.

## TLS, browser access, and security

The dashboard can be used over plain HTTP on a trusted LAN. Browser microphone capture via `getUserMedia()` generally requires a secure context (HTTPS or localhost) and explicit browser permission; a LAN IP served over HTTP is usually not treated as a secure context.

TLS is selected when the certificate and key paths are readable and `--plain-text` is not supplied. TLS files are configurable through `CM5AUDIO_TLS_CERT`, `CM5AUDIO_TLS_KEY`, and `CM5AUDIO_TLS_P12`. `CM5AUDIO_TLS_P12` is a download path, not a prerequisite for enabling HTTPS/WSS.

### Current deployment warnings

This revision is designed for trusted development/LAN use, not direct Internet exposure:

- The default bind address is `0.0.0.0`.
- The application API has no authentication or authorization layer.
- `/server.key` serves the configured server private key when present.
- `/server.p12` can serve a PKCS#12 bundle containing the server key.
- These credential download routes are registered even in plain-text mode.
- `/client/*` is a broad static directory mount and can expose any file placed beneath `web_client/`.
- The WebSocket listener is a separate port from HTTP; securing/reverse-proxying HTTP alone does not secure or proxy the WS listener.

Before untrusted-network deployment, disable/remove direct private-key and PKCS#12 download routes, rotate credentials that may have been exposed, distribute only public certificate/CA material, add authentication/authorization/origin/rate/size controls, isolate network access, audit the static mount, and run the process under a least-privilege account with the required audio permissions. Do not rely on a reverse proxy if clients can still connect directly to the native listener ports.

The application does not inherently require root. Running as root may select a different audio server context/session than the desktop user; use the appropriate account and permissions for the selected backend.

## Source map

```text
cm5audio/
├── include/
│   ├── constants.h                  Engine block, channel, staging and port limits
│   ├── audio_processor.h            Audio/MIDI event types and processor interfaces
│   ├── processor_registry.h         Processor parameter/port/channel descriptors
│   ├── device_manager.h              Backend/device ownership declarations
│   ├── dsp_engine.h                  Graph processing, route DSP, revision and meter state
│   ├── web_server.h                  Routes, plugin instances, presets, settings and sessions
│   ├── reliable_http_server.h        Accepted HTTP socket cancellation/shutdown wrapper
│   ├── reliable_websocket_server.h   IXWebSocket accepted-worker shutdown wrapper
│   ├── ring_buffer.h                 PCM ring-buffer wrapper
│   ├── plugins/                      Built-in processor interfaces
│   ├── miniaudio.h                   Vendored miniaudio API/implementation declarations
│   └── httplib.h                     Vendored cpp-httplib
├── src/
│   ├── main.cpp                      Startup, arguments, signal handling, lifecycle
│   ├── device_manager.cpp            Backend priority, devices, activation and callbacks
│   ├── dsp_engine.cpp                Graph rebuilding, processing, routing and meters
│   ├── processor_registry.cpp        Registry validation, normalization and discovery
│   ├── web_server.cpp                HTTP/WS API, plugin instances, presets and settings
│   ├── miniaudio_impl.cpp             Single native miniaudio implementation
│   └── plugins/                      Built-in processor implementations and registration
├── web_client/
│   ├── dashboard.html                Main monolithic visual patchbay/plugin editor
│   ├── index.html                    Optional browser audio terminal
│   ├── client_main.cpp               Emscripten/miniaudio PCM bridge source
│   ├── scan.html                     Auxiliary device scan page
│   ├── build_wasm.sh                 Optional browser audio bundle build
│   └── dist/                         Generated/ignored WASM JS and module
├── docs/
│   └── PLUGINS.md                    Processor, port, chain and MIDI development rules
├── scripts/                          Setup, IXWebSocket install and certificate helpers
├── third_party/ixwebsocket/          Local matching IX compatibility sources
├── tests/                             Plugin, channel, reliability and shutdown checks
├── Makefile                           Native executable and optional test/tool targets
├── cm5audio_settings.json             Local runtime settings (normally ignored)
├── certs/                              Local TLS credentials (normally ignored)
└── build/                              Generated native objects and dependency files
```

The Makefile's `APP_SRCS` list defines what is compiled into the main `cm5audio` binary. Optional helper/test binaries are built only when their explicit targets are invoked; they are not the normal server executable.

## Diagnostics and troubleshooting

### Dashboard shows no audio devices

- Check the startup log for the selected miniaudio backend.
- Verify that the audio server/backend is running in the same user/session context as `cm5audio`.
- Compare `/api/devices` with what the selected backend exposes; `aplay -l`/`arecord -l` inspect ALSA and may not represent PulseAudio/JACK-visible devices.
- Activate the device/direction in Hardware Manager; enumeration alone does not create an active stream.

### No sound despite visible endpoints

- Inspect `/api/graph` for exact source/destination IDs, directions, widths, route channel indices, `enabled`, and gain.
- Verify that the route exists in the intended direction and that channels are zero-based.
- Check route gain, active route DSP master gain, processor parameters, and fixed global playback DSP. These gains can multiply.
- Check plugin-node input/output routes and the plugin's active/bypass state.
- Check whether the node has the intended channel width. A mono route extraction produces a mono chain; it does not automatically create a stereo chain.
- Use `/api/meters` to distinguish signal absence at capture, plugin input/output, tone, and playback endpoints.
- Remember that summed routes are not automatically normalized.
- Avoid enabled graph cycles.

### Plugin processor missing or saved chain is invalid

- Fetch `/api/dsp/available`; the running binary's registry is authoritative.
- Verify processor `type`, parameter IDs, JSON types, and bounds against discovery.
- A removed/renamed type or parameter has no automatic schema migration. Correct/remove the invalid chain entry and apply it again.
- For composite chains, check `chain_channel_mode`; `mono_only` entries reject widths above one.
- Rebuild and restart after changing C++ processor code; there is no runtime registry hot reload.

### Tone generator does not appear audible

- `GET /api/tone` shows authoritative enabled/frequency/amplitude values.
- Tone is a mono source at `tone/generator`, channel index 0. Create an explicit route to a playback endpoint.
- Start with a low amplitude and route gain to avoid unexpected output level.
- Inspect `.tone` in `/api/meters` to verify the generated source itself is active.

### Optional PipeWire topology tab is empty

- If started without `--enable-pipewire-patchbay`, `/api/pipewire/graph` should return `enabled:false` and empty `nodes`/`links`; this is expected.
- Restart with `--enable-pipewire-patchbay` to enable the optional topology view.
- This feature is separate from the application's `/api/graph` routing model.

### A POST returns 503 or 500

- A 503 `pending` response means accepted state may still be adopted. Refresh authoritative state before retrying; do not assume rollback.
- A 500 with `adopted:true` indicates the live change was applied but saving failed. Check settings path permissions/filesystem, then retry SAVE.
- A 400 indicates validation or request-shape failure; confirm whether the endpoint expects form/query parameters or a JSON body.

### Web browser microphone permission fails

- Use HTTPS with a trusted certificate or localhost; a LAN HTTP origin is generally not a secure context for microphone capture.
- Confirm browser microphone permission and use the browser audio terminal page.
- The native dashboard patchbay itself does not require the WASM bundle.

### Settings are missing or did not restore

- Confirm the process working directory and `CM5AUDIO_SETTINGS` path.
- Confirm the file is valid JSON and the running account can read it.
- Settings are loaded at startup; there is no runtime import/reload endpoint.
- The application does not automatically recover a `.bak` file. Preserve manual backups before editing.

## Known limitations

The following are implementation boundaries at this documentation baseline, not claims that they are impossible to add:

- No built-in authentication/authorization for HTTP or WebSocket control.
- Unauthenticated private-key/PKCS#12 download routes and a broad static mount require remediation before untrusted deployment.
- No automatic input-channel-to-output-channel routing.
- No supported enabled-cycle/feedback graph scheduler.
- Route-attached DSP remains a per-route mono processing path; use composite plugin-chain nodes for multichannel serial processing.
- Composite plugin-chain nodes support one serial audio bus (1..8 channels), not arbitrary multiple independent audio buses within one chain definition.
- Processor channel semantics depend on registry declarations. `per_channel` duplicates processor state per channel; `multichannel` relies on the processor's own interleaved-bus implementation; `mono_only` restricts width to one.
- RNNoise is explicitly mono-only in this revision.
- Physical MIDI discovery/connection is still handled by MIDI processor-specific PipeWire services; a centralized, generic physical MIDI device graph is not yet present.
- MIDI event queues are bounded and events can be dropped when full; this is not a general DAW timeline/clock implementation.
- WebSocket PCM has no framed/versioned packet format, sequence recovery, network jitter buffer, rate negotiation, or clock-drift compensation.
- Hardware channel gain/mute controls are not connected to the audio sample path.
- Settings file save does not implement parent-directory `fsync`, automatic backup recovery, crash rollback, or multi-process coordination.
- Hardware/backend/channel maps and physical latency still require validation on the target audio devices. Synthetic tests do not prove analog connector mapping or inter-device clock behavior.

For plugin-specific implementation requirements and API contracts, see [`docs/PLUGINS.md`](docs/PLUGINS.md).
