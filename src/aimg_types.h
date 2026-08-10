#ifndef AIMG_TYPES_H
#define AIMG_TYPES_H

#include <string>
#include <cstdint>

// Field order groups by type (wstring, then numeric, then bool flags) rather
// than by logical field pairing -- each bool sitting between two 8-byte-
// aligned members (std::wstring/int64_t/double) used to force 3-7 bytes of
// padding after it; grouping all seven flags together at the end leaves only
// one small padding gap instead of six. All call sites access fields by
// name, never by offset, so this reorder is layout-only.
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
    std::wstring full_parameters;    // Raw parameters text / workflow summary

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
