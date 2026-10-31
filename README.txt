CM5Audio plugin node VU meter fix

Full replacements for the current typed-port/plugin-node codebase:
  include/web_server.h
  src/web_server.cpp
  src/dsp_engine.cpp
  web_client/dashboard.html

What this fixes:
- Adds a preallocated atomic meter bank to each audio plugin port.
- Measures plugin audio inputs before processor execution.
- Measures plugin audio outputs after processor execution.
- Extends GET /api/meters with a `plugins` object keyed by plugin instance ID
  and port ID, while preserving the existing capture/playback fields.
- Renders and updates VU bars for every audio input and output port in each
  plugin node window.

No Makefile changes are required. The audio-thread measurement uses preallocated
storage and atomics; it does not allocate or acquire a mutex in the processing loop.

Install from the project root with:
  unzip -o /path/to/cm5audio-vu-meter-fix.zip -d /root/Sources/cm5audio

Then build and launch as usual:
  make -B -j7
  ./cm5audio --plain-text
