CXX = g++
NLOHMANN_JSON_INCLUDE ?= ../json/include
CXXFLAGS = -std=c++17 -O3 -Iinclude -I/usr/local/include -I$(NLOHMANN_JSON_INCLUDE) -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2 -Wall -pthread
LDFLAGS = -L/usr/local/lib -lasound -lpipewire-0.3 -lixwebsocket -lssl -lcrypto -lz -lrnnoise -lpthread

# Separate fast-changing app sources from rarely-changed miniaudio
APP_SRCS = src/main.cpp src/web_server.cpp src/device_manager.cpp src/dsp_engine.cpp \
	src/processor_registry.cpp src/plugins/register_builtin_processors.cpp \
	src/plugins/passthrough.cpp src/plugins/gain.cpp src/plugins/compressor.cpp \
	src/plugins/echo_processor.cpp src/plugins/chorus_processor.cpp src/plugins/reverb_processor.cpp \
	src/plugins/lush_reverb_processor.cpp src/plugins/eq_processor.cpp src/plugins/normalize.cpp src/plugins/autotune.cpp src/plugins/vowel_filter.cpp src/plugins/soft_clipper.cpp src/plugins/expander.cpp src/plugins/hpf_processor.cpp src/plugins/lpf_processor.cpp src/plugins/deesser.cpp src/plugins/bitcrusher.cpp src/plugins/noise_gate.cpp src/plugins/limiter.cpp src/plugins/midi_pitch_shifter.cpp src/plugins/midi_harmonizer.cpp src/plugins/rnnoise_processor.cpp
APP_OBJS = $(APP_SRCS:src/%.cpp=build/%.o)

MINIAUDIO_SRC = src/miniaudio_impl.cpp
MINIAUDIO_OBJ = build/miniaudio_impl.o

IX_TLS_OBJ = build/ix_tls_shutdown.o
IX_SERVER_OBJ = build/ix_socket_server_shutdown.o
IX_WS_OBJ = build/ix_websocket_server_shutdown.o
IX_COMPAT_OBJS = $(IX_TLS_OBJ) $(IX_SERVER_OBJ) $(IX_WS_OBJ)
OBJS = $(APP_OBJS) $(MINIAUDIO_OBJ) $(IX_COMPAT_OBJS)
TARGET = cm5audio

CAPS_OBJ = build/alsa_caps.o
CAPS_TARGET = alsa_caps

TEST_OBJ = build/alsa_bridge_test.o
TEST_TARGET = alsa_bridge_test

PLUGIN_TEST_OBJ = build/dsp_plugins_test.o
PLUGIN_TEST_TARGET = dsp_plugins_test
PLUGIN_TEST_DEPS = $(filter-out build/main.o,$(APP_OBJS)) $(MINIAUDIO_OBJ) $(IX_COMPAT_OBJS)

CHANNEL_TEST_OBJ = build/channel_width_test.o
CHANNEL_TEST_TARGET = channel_width_test

# --- Native PipeWire Channel Test Tool ---
PIPEWIRE_CHANNEL_TEST_OBJ = build/pipewire_channel_test.o
PIPEWIRE_CHANNEL_TEST_TARGET = pipewire_channel_test

all: build $(TARGET)

build:
	mkdir -p build

# The retained legacy miniaudio object may lack a generated .d file. Always
# rebuild it when its vendored header changes (including lifecycle fixes).
$(MINIAUDIO_OBJ): include/miniaudio.h

# Track headers so editing/registering a C++ processor rebuilds every affected object.
build/%.o: src/%.cpp | build
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(IX_TLS_OBJ): third_party/ixwebsocket/IXSocketOpenSSL.cpp | build
	$(CXX) $(CXXFLAGS) -DIXWEBSOCKET_USE_OPEN_SSL -DIXWEBSOCKET_USE_TLS -DIXWEBSOCKET_USE_ZLIB -MMD -MP -c $< -o $@

$(IX_SERVER_OBJ): third_party/ixwebsocket/IXSocketServer.cpp | build
	$(CXX) $(CXXFLAGS) -DIXWEBSOCKET_USE_OPEN_SSL -DIXWEBSOCKET_USE_TLS -DIXWEBSOCKET_USE_ZLIB -MMD -MP -c $< -o $@

$(IX_WS_OBJ): third_party/ixwebsocket/IXWebSocketServer.cpp | build
	$(CXX) $(CXXFLAGS) -DIXWEBSOCKET_USE_OPEN_SSL -DIXWEBSOCKET_USE_TLS -DIXWEBSOCKET_USE_ZLIB -MMD -MP -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Optional tool targets (only built when explicitly invoked)
alsatools: $(CAPS_TARGET) $(TEST_TARGET)

$(CAPS_TARGET): $(CAPS_OBJ)
	$(CXX) $(CAPS_OBJ) -o $@ $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJ)
	$(CXX) $(TEST_OBJ) -o $@ $(LDFLAGS)

# Native PipeWire Channel Test Target
$(PIPEWIRE_CHANNEL_TEST_OBJ): tests/pipewire_channel_test.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(PIPEWIRE_CHANNEL_TEST_TARGET): $(PIPEWIRE_CHANNEL_TEST_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

pipewiretools: $(PIPEWIRE_CHANNEL_TEST_TARGET)

clean-pipewiretools:
	rm -f $(PIPEWIRE_CHANNEL_TEST_OBJ) $(PIPEWIRE_CHANNEL_TEST_OBJ:.o=.d) $(PIPEWIRE_CHANNEL_TEST_TARGET)

$(PLUGIN_TEST_OBJ): tests/dsp_plugins_test.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CHANNEL_TEST_OBJ): tests/channel_width_test.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CHANNEL_TEST_TARGET): $(CHANNEL_TEST_OBJ) $(PLUGIN_TEST_DEPS)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Synthetic hardware callbacks and manual DSP blocks only; never opens a backend.
test-channels: $(CHANNEL_TEST_TARGET)
	timeout 30 ./$(CHANNEL_TEST_TARGET)

build/reliability_test.o: tests/reliability_test.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/reliability_test: build/reliability_test.o $(PLUGIN_TEST_DEPS)
	$(CXX) $^ -o $@ $(LDFLAGS)

build/device_lifecycle_test.o: tests/device_lifecycle_test.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/device_lifecycle_test: build/device_lifecycle_test.o build/device_manager.o $(MINIAUDIO_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

test-reliability: $(TARGET) build/reliability_test build/device_lifecycle_test
	CM5AUDIO_BIND_ADDRESS=127.0.0.1 timeout 45 ./build/reliability_test
	timeout 20 ./build/device_lifecycle_test
	python3 tests/shutdown_test.py

$(PLUGIN_TEST_TARGET): $(PLUGIN_TEST_OBJ) $(PLUGIN_TEST_DEPS)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Starts a test-only server on 28182/28183; never activates physical audio devices.
test-plugins: $(PLUGIN_TEST_TARGET)
	./$(PLUGIN_TEST_TARGET)
	node tests/dashboard_plugins_test.js

clean:
	rm -f $(APP_OBJS) $(APP_OBJS:.o=.d) $(TARGET) $(PLUGIN_TEST_OBJ) $(PLUGIN_TEST_OBJ:.o=.d) $(PLUGIN_TEST_TARGET) $(CHANNEL_TEST_OBJ) $(CHANNEL_TEST_OBJ:.o=.d) $(CHANNEL_TEST_TARGET) build/reliability_test.o build/reliability_test.d build/reliability_test build/device_lifecycle_test.o build/device_lifecycle_test.d build/device_lifecycle_test $(IX_COMPAT_OBJS) build/ix_tls_shutdown.d build/ix_socket_server_shutdown.d build/ix_websocket_server_shutdown.d

clean-miniaudio:
	rm -f $(MINIAUDIO_OBJ) $(MINIAUDIO_OBJ:.o=.d)

clean-alsatools:
	rm -f $(CAPS_OBJ) $(CAPS_OBJ:.o=.d) $(TEST_OBJ) $(TEST_OBJ:.o=.d) $(CAPS_TARGET) $(TEST_TARGET)

-include $(APP_OBJS:.o=.d) $(wildcard build/*.d)

.PHONY: all clean clean-miniaudio clean-alsatools clean-pipewiretools alsatools pipewiretools test-plugins test-reliability test-channels
