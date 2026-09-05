#ifndef AIMG_TYPES_H
#define AIMG_TYPES_H

#include <string>
#include <cstdint>

// Grouped by type (strings, then numerics, then bool flags) rather than by
// logical pairing: an interleaved bool forces 3-7 bytes of padding after
// each one. Layout-only -- every call site accesses fields by name.
struct AImgInfo {
    std::wstring generator;          // e.g. "Automatic1111", "ComfyUI", "Fooocus", etc.
    std::wstring prompt;             // Positive Prompt
    std::wstring negative_prompt;    // Negative Prompt
    std::wstring model;              // Checkpoint / Base Model name
    std::wstring model_hash;         // Model Hash (e.g. "7f92a4bc")
    std::wstring sampler;            // e.g. "DPM++ 2M Karras", "Euler a"
    std::wstring scheduler;          // e.g. "karras", "exponential", "normal"
    std::wstring size;               // e.g. "1024x1024"
    std::wstring hires_upscale;      // e.g. "2" (Hires upscale factor)
    std::wstring hires_upscaler;     // e.g. "Latent", "4x-UltraSharp"
    std::wstring vae;                // VAE name
    std::wstring lora;               // Used LoRAs list
    // Raw parameters text / workflow, kept as UTF-8 on purpose: for a large
    // ComfyUI graph this can be ~1 MB, and only the opt-in "Full Parameters"
    // field ever reads it. aimg.cpp converts to UTF-16 lazily and caches
    // that on the LRU entry, so files without that column never pay for it.
    std::string full_parameters_utf8;

    int64_t seed = 0;
    double cfg_scale = 0.0;
    double denoising_strength = 0.0;
    int32_t steps = 0;
    int32_t clip_skip = 0;
    int32_t hires_steps = 0;

    bool has_metadata = false;
    bool has_seed = false;
    bool has_cfg = false;
    bool has_steps = false;
    bool has_clip_skip = false;
    bool has_denoising_strength = false;
    bool has_hires_steps = false;
};

#endif // AIMG_TYPES_H
