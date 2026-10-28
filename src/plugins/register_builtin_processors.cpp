#include "processor_registry.h"

void register_gain_processor(ProcessorRegistry& registry);
void register_compressor_processor(ProcessorRegistry& registry);
void register_passthrough_processor(ProcessorRegistry& registry);
void register_echo_processor(ProcessorRegistry& registry);
void register_chorus_processor(ProcessorRegistry& registry);
void register_reverb_processor(ProcessorRegistry& registry);
void register_lush_reverb_processor(ProcessorRegistry& registry);
void register_eq_processor(ProcessorRegistry& registry);
void register_hpf_processor(ProcessorRegistry& registry);
void register_lpf_processor(ProcessorRegistry& registry);
void register_deesser_processor(ProcessorRegistry& registry);
void register_normalize_processor(ProcessorRegistry& registry);
void register_noise_gate_processor(ProcessorRegistry& registry);
void register_bitcrusher_processor(ProcessorRegistry& registry);
void register_limiter_processor(ProcessorRegistry& registry);

void register_builtin_processors(ProcessorRegistry& registry) {
    register_passthrough_processor(registry);
    register_gain_processor(registry);
    register_compressor_processor(registry);
    register_echo_processor(registry);
    register_chorus_processor(registry);
    register_reverb_processor(registry);
    register_lush_reverb_processor(registry);
    register_eq_processor(registry);
    register_hpf_processor(registry);
    register_lpf_processor(registry);
    register_deesser_processor(registry);
    register_normalize_processor(registry);
    register_noise_gate_processor(registry);
    register_bitcrusher_processor(registry);
    register_limiter_processor(registry);
}
