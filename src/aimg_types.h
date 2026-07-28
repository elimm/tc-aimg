#ifndef AIMG_TYPES_H
#define AIMG_TYPES_H

#include <string>
#include <cstdint>

struct AImgInfo {
    bool has_metadata = false;

    std::wstring generator;          // e.g. "Automatic1111", "ComfyUI", "Fooocus", etc.
    std::wstring prompt;             // Positive Prompt
    std::wstring negative_prompt;    // Negative Prompt
    std::wstring model;              // Checkpoint / Base Model name
    std::wstring model_hash;         // Model Hash (e.g. "7f92a4bc")
    
    int64_t seed = 0;
    bool has_seed = false;

    double cfg_scale = 0.0;
    bool has_cfg = false;

    int32_t steps = 0;
    bool has_steps = false;

    std::wstring sampler;            // e.g. "DPM++ 2M Karras", "Euler a"
    std::wstring scheduler;          // e.g. "karras", "exponential", "normal"

    int32_t clip_skip = 0;
    bool has_clip_skip = false;

    std::wstring size;               // e.g. "1024x1024"
    
    double denoising_strength = 0.0;
    bool has_denoising_strength = false;

    std::wstring hires_upscale;      // e.g. "2" (Hires upscale factor)
    std::wstring hires_upscaler;     // e.g. "Latent", "4x-UltraSharp"

    int32_t hires_steps = 0;
    bool has_hires_steps = false;

    std::wstring vae;                // VAE name
    std::wstring lora;               // Used LoRAs list
    std::wstring full_parameters;    // Raw parameters text / workflow summary
};

#endif // AIMG_TYPES_H
