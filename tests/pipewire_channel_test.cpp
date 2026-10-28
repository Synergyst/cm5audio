#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <atomic>
#include <csignal>

struct data {
    struct pw_main_loop *loop;
    struct pw_filter *filter;
    std::vector<void*> in_ports;
    std::vector<void*> out_ports;
};

static std::atomic<bool> g_running{true};
static struct data *g_data_ptr = nullptr;

static void sigint_handler(int) {
    g_running = false;
    if (g_data_ptr && g_data_ptr->loop) {
        pw_main_loop_quit(g_data_ptr->loop);
    }
}

// Helper function to parse comma-delimited lists with fallback to default names
static std::vector<std::string> parse_port_names(const char* arg, const std::string& prefix, int total) {
    std::vector<std::string> names;
    if (arg != nullptr) {
        std::stringstream ss(arg);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                names.push_back(item.substr(start, end - start + 1));
            } else if (!item.empty()) {
                names.push_back(item);
            }
        }
    }

    for (int i = (int)names.size(); i < total; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s_%02d", prefix.c_str(), i);
        names.push_back(buf);
    }
    return names;
}

// Real-time audio process callback
static void on_process(void *userdata, struct spa_io_position *position) {
    struct data *data = (struct data *)userdata;

    if (!position || data->in_ports.empty() || data->out_ports.empty()) {
        return;
    }

    uint32_t n_samples = position->clock.duration;

    for (int i = 0; i < 16; ++i) {
        float *in = (float *)pw_filter_get_dsp_buffer(data->in_ports[i], n_samples);
        float *out = (float *)pw_filter_get_dsp_buffer(data->out_ports[i], n_samples);

        if (in != nullptr && out != nullptr) {
            for (uint32_t s = 0; s < n_samples; ++s) {
                out[s] = in[s];
            }
        }
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

int main(int argc, char **argv) {
    std::cout << "[Native PipeWire Test] Initializing 16-channel duplex DSP filter (32-bit float @ 48000Hz)...\n";

    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    std::vector<std::string> input_names = parse_port_names((argc > 1) ? argv[1] : nullptr, "input", 16);
    std::vector<std::string> output_names = parse_port_names((argc > 2) ? argv[2] : nullptr, "output", 16);

    pw_init(&argc, &argv);

    struct data data = {0};
    g_data_ptr = &data;

    data.loop = pw_main_loop_new(NULL);
    if (!data.loop) {
        std::cerr << "[Error] Failed to create PipeWire main loop.\n";
        return -1;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, "cm5audio_native_16ch",
        PW_KEY_NODE_DESCRIPTION, "16-Channel Native PipeWire Duplex Passthrough",
        NULL
    );

    data.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(data.loop),
        "cm5audio_native_16ch",
        props,
        &filter_events,
        &data
    );

    if (!data.filter) {
        std::cerr << "[Error] Failed to create PipeWire filter.\n";
        pw_main_loop_destroy(data.loop);
        return -1;
    }

    for (int i = 0; i < 16; ++i) {
        struct pw_properties *in_props = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, input_names[i].c_str(),
            NULL
        );
        void *in_p = pw_filter_add_port(data.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0, in_props, NULL, 0);
        data.in_ports.push_back(in_p);

        struct pw_properties *out_props = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, output_names[i].c_str(),
            NULL
        );
        void *out_p = pw_filter_add_port(data.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0, out_props, NULL, 0);
        data.out_ports.push_back(out_p);
    }

    if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
        std::cerr << "[Error] Failed to connect native PipeWire filter!\n";
        pw_filter_destroy(data.filter);
        pw_main_loop_destroy(data.loop);
        return -1;
    }

    std::cout << "[Success] Native PipeWire 16-channel node active. Press Ctrl+C to exit.\n";

    pw_main_loop_run(data.loop);

    std::cout << "[Stopping] Cleaning up PipeWire resources...\n";
    pw_filter_destroy(data.filter);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    std::cout << "[Done] Exited cleanly.\n";

    return 0;
}
