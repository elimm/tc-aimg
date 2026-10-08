#ifndef AIMG_DECODER_H
#define AIMG_DECODER_H

#include "aimg_types.h"
#include "metadata_parser.h"

namespace SimpleJson { struct JsonValue; }

class AImgDecoder {
public:
    static AImgInfo Decode(const RawImageMetadata& rawMeta);
    // Public so aimg.cpp can reuse it for the lazy conversion of
    // full_parameters_utf8 instead of carrying a second wrapper.
    static std::wstring Utf8ToWstring(const std::string& str);

private:
    // Per-generator dispatch, then display normalization (model/vae
    // extension, LoRA shape) applied uniformly to every generator's result.
    static AImgInfo DecodeCore(const RawImageMetadata& rawMeta);
    static std::wstring StripModelExtension(const std::wstring& name);
    static std::wstring NormalizeLoraField(const std::wstring& raw);

    static bool DecodeAutomatic1111(const std::string& paramText, AImgInfo& info, bool populateFullParameters = true);
    static bool DecodeComfyUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info, bool* outNegativeZeroed = nullptr);
    static bool DecodeEasyDiffusion(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeInvokeAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeSwarmUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeFooocus(const std::string& paramText, AImgInfo& info);
    static bool DecodeNovelAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    // Not folded into SimpleGeneratorConfig: extra fields (lora[] array,
    // "strength", nested v2.clipSkip) with no equivalent in that shape.
    static bool DecodeDrawThings(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    // Not folded into SimpleGeneratorConfig: LoRA must be zipped from two
    // parallel arrays, which the shared flat-key helper can't do.
    static bool DecodeWanGP(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);

    static std::string Trim(const std::string& str);
};

#endif // AIMG_DECODER_H
