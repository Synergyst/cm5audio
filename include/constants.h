#ifndef CONSTANTS_H
#define CONSTANTS_H

// Maximum channel count accepted by the vendored miniaudio API (MA_MAX_CHANNELS).
// This is an explicit resource/validation bound, not a fixed engine stride.
constexpr unsigned CM5_MAX_CHANNELS = 254;
constexpr unsigned CM5_MAX_CLIENT_CHANNELS = 8;
constexpr unsigned CM5_MAX_CONTROL_CHANNELS = 8; // Legacy inaudible channel-control storage.
constexpr unsigned CM5_MAX_AUDIO_FRAMES = 4320;
constexpr unsigned CM5_BUFFER_FRAMES = 256;

constexpr unsigned CM5_MAX_PROCESSORS_PER_ROUTE = 16;
constexpr unsigned CM5_MAX_PLUGIN_PORTS = 16;
// Composite plugin chains use one symmetric audio bus, bounded to the
// channel width supported by the built-in DSP processors.
constexpr unsigned CM5_MAX_CHAIN_CHANNELS = CM5_MAX_CLIENT_CHANNELS;

#endif
