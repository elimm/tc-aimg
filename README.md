# AImg Metadata Content Plugin for Total Commander (`.wdx` / `.wdx64`)

[![Build](https://github.com/elimm/tc-aimg/actions/workflows/build.yml/badge.svg)](https://github.com/elimm/tc-aimg/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A high-performance Total Commander Content Plugin (**WDX**) that lets you inspect, sort, and search AI-generated images by their embedded generation metadata — right from the file list, no extra tools needed.

Tested against **Automatic1111 WebUI**, **Easy Diffusion**, and **ComfyUI** (ComfyUI support is best-effort — its workflow JSON varies too much between custom nodes/graphs to cover exhaustively). Decoders for **SD.Next**, **WebUI Forge**, **Fooocus**, **InvokeAI**, **SwarmUI**, and **NovelAI** are also included, implemented against their published metadata formats, but not yet verified against real output from those tools — feedback/sample files welcome.

---

## 🌟 Key Features

- **Multi-Format Image Support**:
  - **PNG (`.png`)**: Parses `tEXt`, `zTXt` (zlib compressed), `iTXt` (UTF-8), and `eXIf` chunks.
  - **JPEG / JPG (`.jpg`, `.jpeg`)**: Parses EXIF `UserComment` (`0x9286`), `ImageDescription`, JPEG Comments (`0xFFFE`), and XMP metadata.
  - **WebP (`.webp`)**: Parses RIFF `EXIF` and `XMP ` chunks.
  - **AVIF (`.avif`)**: Parses ISOBMFF `Exif` and `xml ` metadata boxes.
  - **TIFF (`.tif`, `.tiff`)**: Parses standard EXIF tags.
- **Deep Generator Recognition**:
  - **Automatic1111** (also covers **SD.Next** / **WebUI Forge**'s compatible parameter format, with Forge itself detected and labeled separately by version string) and **Easy Diffusion**: Extracts Positive Prompt, Negative Prompt, Steps, Sampler, Scheduler, CFG Scale, Seed, Model, Model Hash, Clip Skip, Denoising Strength, Hires Upscale parameters, VAE, and LoRA list.
  - **ComfyUI** *(best-effort)*: Parses execution graphs (`prompt`) and workflow JSON structure, including subgraphs with promoted widgets. Recursively traces KSampler nodes, CLIPTextEncode nodes, Checkpoint loaders, VAE loaders, LoRA loaders, and latent resolutions, with substring-based matching so many custom node packs are picked up too.
  - **Fooocus** *(untested)*: Extracts Fooocus prompt expansions, styles, samplers, base models, and refiners.
  - **InvokeAI** *(untested)*: Extracts `invokeai_metadata`.
  - **SwarmUI** *(untested)*: Extracts `sui_image_params`.
  - **NovelAI** *(untested)*: Extracts NovelAI Diffusion parameters and seeds.
  - **Draw Things** *(untested)*: Extracts its XMP-embedded generation record (prompt, seed, sampler, LoRAs, and more).
  - **WanGP** *(untested)*: Extracts its EXIF-embedded generation record (prompt, seed, LoRAs, and more).
- **Fast Performance & Thread-Safe Caching**:
  - Zero-lag file list scrolling in Total Commander due to built-in single-pass file result caching.
  - Large or network-hosted images are read on a background thread instead of Total Commander's main
    thread, so the file list stays responsive while their metadata is extracted; those columns fill in
    a moment later rather than freezing the window. Small local files are still read immediately, with
    no added delay. Interrupting a directory listing aborts an extraction already in progress.
- **Full Unicode (UTF-16) Support**:
  - Correctly displays prompts containing non-Latin scripts (Cyrillic, Japanese, Chinese, etc.) and emojis.

---

## 📋 Available Fields in Total Commander

`Prompt`, `Negative Prompt` and `Full Parameters` are full-text search fields (WDX `ft_fulltext`); Total Commander only shows fields of this type in its search dialog (`Alt+F7` -> Plugins tab), not as file-list columns or in the Multi-Rename Tool -- use `Prompt (Short)` / `Negative Prompt (Short)` for a column instead. Field indices changed in 0.2.0 (the three full-text fields moved to the end of the table, as the WDX SDK requires); if you had custom columns configured against the old indices, remove and re-add them in Total Commander's column configuration.

| Field Name | Type | Description |
|---|---|---|
| `Generator` | String | Generator name (`Automatic1111`, `ComfyUI`, `Fooocus`, `InvokeAI`, `SwarmUI`, `NovelAI`, etc.) |
| `Prompt` | Full Text | Positive prompt text, untruncated (search dialog only, see note above) |
| `Prompt (Short)` | String | Positive prompt text, cut off at TC's column buffer size (handy for compact columns) |
| `Negative Prompt` | Full Text | Negative prompt text, untruncated (search dialog only, see note above) |
| `Negative Prompt (Short)` | String | Negative prompt text, cut off at TC's column buffer size |
| `Model` | String | Model / Checkpoint name |
| `Model Hash` | String | Short model hash (e.g. `7f92a4bc`) |
| `Seed` | 64-bit Number | Generation seed |
| `CFG Scale` | Floating Point | CFG / Guidance scale |
| `Steps` | 32-bit Number | Generation step count |
| `Sampler` | String | Sampler algorithm (e.g. `Euler a`, `DPM++ 2M Karras`) |
| `Scheduler` | String | Scheduler algorithm (e.g. `karras`, `normal`, `exponential`) |
| `Clip Skip` | 32-bit Number | CLIP skip value |
| `Size` | String | Generation resolution (e.g. `1024x1024`) |
| `Aspect Ratio` | String | Width:height ratio in lowest terms, derived from `Size` (e.g. `16:9`) |
| `Megapixels` | Floating Point | Total resolution in megapixels, derived from `Size`, rounded to 2 decimals |
| `Denoising Strength` | Floating Point | Denoising strength value |
| `Hires Upscale` | String | Hires.fix upscale factor (e.g. `1.5`) |
| `Hires Upscaler` | String | Hires.fix upscaler name (e.g. `4x-UltraSharp`) |
| `Hires Steps` | 32-bit Number | Hires.fix step count |
| `VAE` | String | VAE model name |
| `LoRA` | String | Applied LoRAs and weights |
| `LoRA Count` | 32-bit Number | Number of LoRAs applied, derived from `LoRA` |
| `Full Parameters` | Full Text | Complete raw parameters block or JSON workflow (search dialog only, see note above) |
| `Has AI Metadata` | Boolean | `Yes` / `No` (useful for quick filtering in Total Commander) |

---

## 🚀 Installation

Each release ships two Total Commander auto-installer archives, both containing the same 32-bit (`aimg.wdx`) + 64-bit (`aimg.wdx64`) DLL pair, just linked differently:

- **`aimg.wdx.zip`** — static CRT (`/MT`). Self-contained, no extra dependencies. Recommended for most users.
- **`aimg-md.wdx.zip`** — dynamic CRT (`/MD`). Smaller DLLs, but requires the [Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) (`VCRUNTIME140`/`VCRUNTIME140_1`) to already be installed on the target machine.

### Automatic Installation (Recommended)
1. Download either `aimg.wdx.zip` or `aimg-md.wdx.zip`.
2. Open the archive inside Total Commander.
3. Total Commander will automatically prompt: *"Do you want to install plugin aimg?"*. Click **Yes**.

### Manual Installation
1. Extract the archive.
2. Go to Total Commander menu: **Configuration** -> **Options** -> **Plugins**.
3. Click **Content Plugins (.WDX)** -> **Configure**.
4. Click **Add** and select `aimg.wdx64` (for 64-bit TC) or `aimg.wdx` (for 32-bit TC).

---

## 🛠 Total Commander Usage Guide

### 1. Setting Up Custom Columns
1. Right-click the file list header in Total Commander and select **Configure Custom Columns...**
2. Click **New...** and set name to `AI Images`.
3. Add columns:
   - `[=aimg.Generator]`
   - `[=aimg.Model]`
   - `[=aimg.Seed]`
   - `[=aimg.Prompt (Short)]`
4. Click **OK** to save.

### 2. Setting Up File Tooltips (Hover Info)
1. Go to **Configuration** -> **Options** -> **Display** -> **Help texts**.
2. Enable **Win32-style tips with file comments (if available)**.
3. Custom tip rule for image extensions (`*.png;*.jpg;*.jpeg;*.webp;*.avif;*.tif;*.tiff`):
   ```
   Generator: [=aimg.Generator]
   Model: [=aimg.Model]
   Seed: [=aimg.Seed]
   Prompt: [=aimg.Prompt (Short)]
   Negative Prompt: [=aimg.Negative Prompt (Short)]
   ```

### 3. Searching for Specific Prompts or Seeds
1. Press `Alt + F7` in Total Commander to open **Find Files**.
2. Go to the **Plugins** tab.
3. Set condition:
   - `Plugin`: `aimg`
   - `Property`: `Prompt`
   - `OP`: `contains`
   - `Value`: `cyberpunk` (or any keyword)
4. Click **Start Search**.

---

## 🏗 Building from Source

Requirements: MSVC, GCC/MinGW or Clang, optionally with CMake. The easiest route is `build.bat`, which picks whichever of the three is installed; `build_msvc.bat` builds both bitnesses and the install zips.

### Build with CMake:
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### Build with MSVC:
```cmd
rc /fo build\aimg.res /i src src\aimg.rc
cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /DUNICODE /D_UNICODE /DNDEBUG /MT /LD /I src src\metadata_parser.cpp src\aimg_decoder.cpp src\comfyui_decoder.cpp src\aimg.cpp /Fo"build\\" /Fe:build\aimg.wdx64 /link /DEF:src\aimg.def build\aimg.res /MACHINE:X64 /LTCG /OPT:REF /OPT:ICF
```

### Build with MinGW / GCC:
```bash
windres -I src src/aimg.rc -O coff -o build/aimg.res.o
g++ -Wall -O3 -flto -fno-rtti -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -DNDEBUG -shared -I src src/metadata_parser.cpp src/aimg_decoder.cpp src/comfyui_decoder.cpp src/aimg.cpp src/aimg.def build/aimg.res.o -o build/aimg.wdx64 -static -s -Wl,--gc-sections
```

---

## 📜 License
[MIT License](LICENSE). Free for commercial and non-commercial use.

---

## 🙏 Acknowledgements

This plugin exists only because of [**Total Commander**](https://www.ghisler.com/) and its Content Plugin (WDX) API. Huge thanks to **Christian Ghisler** for creating and tirelessly maintaining the best file manager in the world.
