#ifndef CM5AUDIO_MIDI_CC_CONTROLLER_H
#define CM5AUDIO_MIDI_CC_CONTROLLER_H

#include <memory>

class ClientManager;
class DspEngine;

// Application-level MIDI CC learning and parameter control. The controller
// has its own HTTP page and applies only processor-declared realtime-safe
// controls without rebuilding the DSP graph.
class MidiCcController {
public:
    explicit MidiCcController(ClientManager& clients, DspEngine& dsp, unsigned httpPort = 8184);
    ~MidiCcController();

    MidiCcController(const MidiCcController&) = delete;
    MidiCcController& operator=(const MidiCcController&) = delete;

    bool start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
