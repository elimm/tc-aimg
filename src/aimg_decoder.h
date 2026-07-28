#ifndef AIMG_DECODER_H
#define AIMG_DECODER_H

#include "aimg_types.h"
#include "metadata_parser.h"

namespace SimpleJson { struct JsonValue; }

class AImgDecoder {
public:
    static AImgInfo Decode(const RawImageMetadata& rawMeta);

private:
    // Runs per-generator dispatch, then normalizes display fields (model/vae
    // extension, LoRA formatting) uniformly across every generator's result.
    static AImgInfo DecodeCore(const RawImageMetadata& rawMeta);
    // Strips a known model-file extension so "model"/"vae" display a bare
    // name regardless of whether the source generator included one.
    static std::wstring StripModelExtension(const std::wstring& name);
    // Normalizes LoRA field shape across generators: no quotes, no extension.
    static std::wstring NormalizeLoraField(const std::wstring& raw);

    static bool DecodeAutomatic1111(const std::string& paramText, AImgInfo& info, bool populateFullParameters = true);
    static bool DecodeComfyUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeEasyDiffusion(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeInvokeAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeSwarmUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);
    static bool DecodeFooocus(const std::string& paramText, AImgInfo& info);
    static bool DecodeNovelAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info);

    static std::wstring Utf8ToWstring(const std::string& str);
    static std::string Trim(const std::string& str);
    static void ExtractSeedCfgSteps(const SimpleJson::JsonValue* jsonObj, const std::string& cfgKey, AImgInfo& info);
};

#endif // AIMG_DECODER_H
