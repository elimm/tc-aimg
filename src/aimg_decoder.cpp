#include "aimg_decoder.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <cwchar>
#include <windows.h>
#include <map>
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// Lightweight JSON Parser implementation
// ---------------------------------------------------------------------------

namespace SimpleJson {

// A double->int64_t cast is UB when n doesn't fit in int64_t's range (e.g. a
// crafted "seed": 1e300); clamp first so malformed/adversarial JSON can only
// produce a saturated value, never UB.
int64_t ClampDoubleToInt64(double n) {
    if (!std::isfinite(n)) return 0;
    if (n >= 9223372036854775807.0) return INT64_MAX;
    if (n <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)n;
}

enum class JsonType { Null, Bool, Number, String, Array, Object };

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<std::shared_ptr<JsonValue>> arrVal;
    // std::map, not unordered_map: measured with tests/run_benchmark.bat --
    // ComfyUI workflow JSON creates hundreds of small per-node objects (a
    // handful of keys each), and unordered_map's extra per-instance bucket-
    // array allocation loses to std::map's single tree-node allocation at
    // this N, both in speed (~2x slower on a 150-node graph) and in binary
    // size (more template instantiation for hashing/rehashing machinery).
    std::map<std::string, std::shared_ptr<JsonValue>> objVal;

    std::string getStr(const std::string& key = "") const {
        if (key.empty()) return type == JsonType::String ? strVal : "";
        if (type == JsonType::Object) {
            auto it = objVal.find(key);
            if (it != objVal.end() && it->second->type == JsonType::String) return it->second->strVal;
        }
        return "";
    }

    double getNum(const std::string& key = "") const {
        if (key.empty()) return type == JsonType::Number ? numVal : 0.0;
        if (type == JsonType::Object) {
            auto it = objVal.find(key);
            if (it != objVal.end() && it->second->type == JsonType::Number) return it->second->numVal;
        }
        return 0.0;
    }

    int64_t getInt64(const std::string& key = "") const {
        return ClampDoubleToInt64(getNum(key));
    }

    const JsonValue* getObj(const std::string& key) const {
        if (type == JsonType::Object) {
            auto it = objVal.find(key);
            if (it != objVal.end()) return it->second.get();
        }
        return nullptr;
    }
};

class JsonParser {
    const std::string& src;
    size_t pos = 0;
    int depth = 0;
    static const int kMaxDepth = 200;

    void skipWS() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n')) {
            pos++;
        }
    }

    std::string parseString() {
        pos++;
        std::string res;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return res;
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                if (esc == '"') res += '"';
                else if (esc == '\\') res += '\\';
                else if (esc == '/') res += '/';
                else if (esc == 'n') res += '\n';
                else if (esc == 'r') res += '\r';
                else if (esc == 't') res += '\t';
                else if (esc == 'u' && pos + 4 <= src.size()) {
                    std::string hexStr = src.substr(pos, 4);
                    pos += 4;
                    uint32_t codepoint = (uint32_t)strtoul(hexStr.c_str(), NULL, 16);

                    // High surrogate: combine with an immediately following
                    // low-surrogate \u escape into the full astral codepoint
                    // (e.g. emoji), per RFC 8259 / UTF-16 surrogate pairing.
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
                        pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                        std::string lowHex = src.substr(pos + 2, 4);
                        uint32_t low = (uint32_t)strtoul(lowHex.c_str(), NULL, 16);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            pos += 6;
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        }
                    }

                    if (codepoint <= 0x7F) {
                        res += (char)codepoint;
                    } else if (codepoint <= 0x7FF) {
                        res += (char)(0xC0 | ((codepoint >> 6) & 0x1F));
                        res += (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint <= 0xFFFF) {
                        res += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
                        res += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        res += (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        res += (char)(0xF0 | ((codepoint >> 18) & 0x07));
                        res += (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        res += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        res += (char)(0x80 | (codepoint & 0x3F));
                    }
                } else res += esc;
            } else {
                res += c;
            }
        }
        return res;
    }

public:
    JsonParser(const std::string& json) : src(json) {}

    std::shared_ptr<JsonValue> parse() {
        skipWS();
        if (pos >= src.size()) return nullptr;
        if (depth >= kMaxDepth) return nullptr; // guard against stack overflow on deeply-nested/adversarial JSON

        auto val = std::make_shared<JsonValue>();
        char c = src[pos];

        if (c == '{') {
            val->type = JsonType::Object;
            pos++;
            depth++;
            skipWS();
            if (pos < src.size() && src[pos] == '}') { pos++; depth--; return val; }
            while (pos < src.size()) {
                skipWS();
                if (pos >= src.size() || src[pos] != '"') break;
                std::string key = parseString();
                skipWS();
                if (pos < src.size() && src[pos] == ':') pos++;
                auto child = parse();
                if (child) val->objVal[key] = child;
                skipWS();
                if (pos < src.size() && src[pos] == ',') { pos++; continue; }
                if (pos < src.size() && src[pos] == '}') { pos++; break; }
                break; // malformed input: stop instead of re-parsing the same position forever
            }
            depth--;
            return val;
        } else if (c == '[') {
            val->type = JsonType::Array;
            pos++;
            depth++;
            skipWS();
            if (pos < src.size() && src[pos] == ']') { pos++; depth--; return val; }
            while (pos < src.size()) {
                auto child = parse();
                if (child) val->arrVal.push_back(child);
                skipWS();
                if (pos < src.size() && src[pos] == ',') { pos++; continue; }
                if (pos < src.size() && src[pos] == ']') { pos++; break; }
                break; // malformed input: stop instead of re-parsing the same position forever
            }
            depth--;
            return val;
        } else if (c == '"') {
            val->type = JsonType::String;
            val->strVal = parseString();
            return val;
        } else if (src.compare(pos, 9, "-Infinity") == 0) {
            // Not valid per the JSON spec, but Python's json.dumps (and
            // therefore ComfyUI, which serializes its execution graph with
            // it) happily emits bare NaN/Infinity/-Infinity for float('nan')/
            // float('inf') values (e.g. a LoadImage node's "is_changed": NaN
            // freshness marker). Left unhandled, this token matches none of
            // the branches below, so parse() returns nullptr having consumed
            // NO characters -- the enclosing object/array's loop then sees a
            // non-','/'}'/']' byte at the same position and aborts early,
            // silently truncating everything parsed after that point in the
            // WHOLE document, not just the one offending field.
            val->type = JsonType::Number; val->numVal = 0.0; pos += 9; return val;
        } else if (src.compare(pos, 8, "Infinity") == 0) {
            val->type = JsonType::Number; val->numVal = 0.0; pos += 8; return val;
        } else if (src.compare(pos, 3, "NaN") == 0) {
            val->type = JsonType::Number; val->numVal = 0.0; pos += 3; return val;
        } else if (isdigit((unsigned char)c) || c == '-') {
            val->type = JsonType::Number;
            size_t start = pos;
            if (c == '-') pos++;
            while (pos < src.size() && (isdigit((unsigned char)src[pos]) || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' || src[pos] == '+' || src[pos] == '-')) {
                pos++;
            }
            val->numVal = strtod(src.substr(start, pos - start).c_str(), NULL);
            return val;
        } else if (src.compare(pos, 4, "true") == 0) {
            val->type = JsonType::Bool; val->boolVal = true; pos += 4; return val;
        } else if (src.compare(pos, 5, "false") == 0) {
            val->type = JsonType::Bool; val->boolVal = false; pos += 5; return val;
        } else if (src.compare(pos, 4, "null") == 0) {
            val->type = JsonType::Null; pos += 4; return val;
        }

        return nullptr;
    }
};

} // namespace SimpleJson


// ---------------------------------------------------------------------------
// String Helpers
// ---------------------------------------------------------------------------

std::wstring AImgDecoder::Utf8ToWstring(const std::string& str) {
    if (str.empty()) return L"";
    int wreqSize = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    if (wreqSize <= 0) return L"";
    std::wstring wres(wreqSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wres[0], wreqSize);
    return wres;
}

std::string AImgDecoder::Trim(const std::string& str) {
    static const char kWhitespace[] = " \t\r\n\0"; // 5-char overload so the embedded NUL isn't dropped as a C-string terminator
    size_t first = str.find_first_not_of(kWhitespace, 0, sizeof(kWhitespace));
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(kWhitespace, std::string::npos, sizeof(kWhitespace));
    return str.substr(first, (last - first + 1));
}

// Used only by the last-resort "any text in a comment-like field is a
// prompt" fallback in DecodeCore: some non-AI tools (e.g. JPEGmini's JPEG
// re-compression) write their own signature into the same COM/EXIF fields a
// text-to-image generator would use for a freeform prompt, e.g. "Optimized by
// JPEGmini 3.13.0.4 0x7ff7a08" -- a raw memory address literal is a giveaway
// of build/diagnostic text, never legitimate prompt content, so reject any
// candidate containing one rather than mislabeling the file as AI-generated.
static bool LooksLikeGenerationText(const std::string& s) {
    if (s.size() < 15) return false;
    size_t hexPos = s.find("0x");
    while (hexPos != std::string::npos) {
        size_t i = hexPos + 2;
        int hexDigits = 0;
        while (i < s.size() && isxdigit((unsigned char)s[i])) { hexDigits++; i++; }
        if (hexDigits >= 5) return false;
        hexPos = s.find("0x", hexPos + 2);
    }
    return true;
}

// Shared by the InvokeAI/SwarmUI/NovelAI decoders: "seed"/"steps" keys are
// consistent across those generators, only the CFG scale key name differs.
// Only sets has_seed/has_cfg/has_steps when the key is actually present, so a
// genuinely missing field isn't indistinguishable from a real value of 0.
void AImgDecoder::ExtractSeedCfgSteps(const SimpleJson::JsonValue* jsonObj, const std::string& cfgKey, AImgInfo& info) {
    if (jsonObj->objVal.count("seed")) {
        info.seed = jsonObj->getInt64("seed");
        info.has_seed = true;
    }
    if (jsonObj->objVal.count(cfgKey)) {
        info.cfg_scale = jsonObj->getNum(cfgKey);
        info.has_cfg = true;
    }
    if (jsonObj->objVal.count("steps")) {
        info.steps = (int32_t)jsonObj->getInt64("steps");
        info.has_steps = true;
    }
}


// ---------------------------------------------------------------------------
// Main AImgDecoder Entry Point
// ---------------------------------------------------------------------------

std::wstring AImgDecoder::StripModelExtension(const std::wstring& name) {
    static const wchar_t* kExts[] = { L".safetensors", L".ckpt", L".pt", L".pth", L".bin", L".gguf" };
    for (const wchar_t* ext : kExts) {
        size_t extLen = wcslen(ext);
        if (name.size() <= extLen) continue;
        size_t pos = name.size() - extLen;
        bool match = true;
        for (size_t i = 0; i < extLen; i++) {
            if (towlower(name[pos + i]) != towlower((wchar_t)ext[i])) { match = false; break; }
        }
        if (match) return name.substr(0, pos);
    }
    return name;
}

std::wstring AImgDecoder::NormalizeLoraField(const std::wstring& raw) {
    if (raw.empty()) return raw;
    // LoRA fields arrive in several different shapes depending on generator:
    // A1111's "Lora hashes" is a comma-separated "name: hash" list, usually
    // wrapped in quotes (sometimes as one big quoted string, sometimes
    // per-entry); ComfyUI's LoraLoader gives bare filenames with extensions
    // and no quotes at all. This is the one place all of them funnel through
    // before display, so every file shows the same shape: no quotes, no
    // extension on the name portion.
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(L',', start);
        std::wstring part = (comma == std::wstring::npos) ? raw.substr(start) : raw.substr(start, comma - start);

        size_t a = part.find_first_not_of(L" \t");
        if (a == std::wstring::npos) { part.clear(); }
        else {
            size_t b = part.find_last_not_of(L" \t");
            part = part.substr(a, b - a + 1);
        }
        if (!part.empty() && part.front() == L'"') part.erase(part.begin());
        if (!part.empty() && part.back() == L'"') part.pop_back();

        size_t colonPos = part.find(L": ");
        if (colonPos != std::wstring::npos) {
            part = StripModelExtension(part.substr(0, colonPos)) + part.substr(colonPos);
        } else {
            part = StripModelExtension(part);
        }

        if (!part.empty()) parts.push_back(part);
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }

    std::wstring result;
    for (const auto& p : parts) {
        if (!result.empty()) result += L", ";
        result += p;
    }
    return result;
}

AImgInfo AImgDecoder::Decode(const RawImageMetadata& rawMeta) {
    AImgInfo info = DecodeCore(rawMeta);
    info.model = StripModelExtension(info.model);
    info.vae = StripModelExtension(info.vae);
    info.lora = NormalizeLoraField(info.lora);
    return info;
}

// Fills any field left empty/unset on `dst` from the corresponding field on
// `src`, never overwriting a field `dst` already has. Used when a generator's
// own parse leaves gaps that a secondary parse of a companion text block can
// fill in (see the ComfyUI + A1111-companion-text merge in DecodeCore).
static void MergeGaps(AImgInfo& dst, const AImgInfo& src) {
    if (dst.prompt.empty()) dst.prompt = src.prompt;
    if (dst.negative_prompt.empty()) dst.negative_prompt = src.negative_prompt;
    if (dst.model.empty()) dst.model = src.model;
    if (dst.model_hash.empty()) dst.model_hash = src.model_hash;
    if (dst.lora.empty()) dst.lora = src.lora;
    if (dst.sampler.empty()) dst.sampler = src.sampler;
    if (dst.scheduler.empty()) dst.scheduler = src.scheduler;
    if (dst.size.empty()) dst.size = src.size;
    if (dst.vae.empty()) dst.vae = src.vae;
    if (!dst.has_seed && src.has_seed) { dst.seed = src.seed; dst.has_seed = true; }
    if (!dst.has_steps && src.has_steps) { dst.steps = src.steps; dst.has_steps = true; }
    if (!dst.has_cfg && src.has_cfg) { dst.cfg_scale = src.cfg_scale; dst.has_cfg = true; }
}

AImgInfo AImgDecoder::DecodeCore(const RawImageMetadata& rawMeta) {
    AImgInfo info;

    // Collect all available text candidates. JSON parsing is lazy and
    // memoized per candidate (via GetJson below) rather than done for every
    // candidate up front: most files only need one or two candidates' JSON
    // before a decoder matches and DecodeCore returns, so eagerly parsing
    // every JSON-shaped chunk (e.g. a ComfyUI PNG's separate "prompt" AND
    // "workflow" chunks) would pay for parses that are never actually used.
    // Once a candidate's JSON is parsed, the SAME parsed tree is reused
    // across every decoder attempt (ComfyUI/EasyDiffusion/InvokeAI/SwarmUI/
    // NovelAI) below instead of re-parsing the raw text from scratch.
    struct Candidate {
        std::string key;
        std::string text;
        std::shared_ptr<SimpleJson::JsonValue> json;
        bool jsonAttempted = false;
    };
    std::vector<Candidate> candidates;
    bool hasComfyUIChunk = false;
    // See the seed-priority override below: "prompt" (API/execution format,
    // sent at queue time) is authoritative and never overridden; only its
    // absence makes a disagreeing "workflow"-derived seed suspect.
    bool hasPromptChunk = rawMeta.text_chunks.count("prompt") > 0;

    for (const auto& kv : rawMeta.text_chunks) {
        std::string cleaned = Trim(kv.second);
        if (!cleaned.empty()) {
            Candidate cand;
            cand.key = kv.first;
            cand.text = std::move(cleaned);
            candidates.push_back(std::move(cand));
        }
        // Only match on chunk keys and structural JSON graph markers, never on
        // substrings of free-text prompt content (a prompt could legitimately
        // contain the word "comfy"/"ComfyUI").
        if (kv.first == "prompt" || kv.first == "workflow" || kv.first.find("ComfyUI") != std::string::npos ||
            kv.second.find("class_type") != std::string::npos || kv.second.find("KSampler") != std::string::npos) {
            hasComfyUIChunk = true;
        }
    }

    if (candidates.empty()) return info;

    // Parses `cand.text` as JSON on first request and caches the result
    // (including a null result for non-JSON text), so repeated lookups
    // across decoder attempts are free.
    auto getJson = [](Candidate& cand) -> SimpleJson::JsonValue* {
        if (!cand.jsonAttempted) {
            cand.jsonAttempted = true;
            // Some writers prefix the JSON object with a label, e.g. EXIF
            // ImageDescription containing "Workflow:{...}" rather than a bare
            // "{...}". Parse from the first '{' so that prefix doesn't make
            // an otherwise-valid graph invisible to the JSON candidate pass;
            // this is safe because a non-JSON prefix on a genuine free-text
            // chunk just makes the parse fail (root stays null) same as today.
            size_t bracePos = cand.text.find('{');
            if (bracePos != std::string::npos) {
                std::string jsonSlice = (bracePos == 0) ? cand.text : cand.text.substr(bracePos);
                SimpleJson::JsonParser parser(jsonSlice);
                auto root = parser.parse();
                if (root && root->type == SimpleJson::JsonType::Object) {
                    cand.json = root;
                }
            }
        }
        return cand.json.get();
    };

    // 1. Try ComfyUI on any candidate that parsed as a JSON object
    for (auto& cand : candidates) {
        SimpleJson::JsonValue* json = getJson(cand);
        if (json && DecodeComfyUI(json, cand.text, info)) {
            // Some ComfyUI "save image w/ metadata" nodes also embed a
            // flattened A1111-style text block alongside the graph JSON,
            // carrying fields no node holds (full typed prompt, LoRA
            // name+hash list, model hash). Fill only the gaps DecodeComfyUI
            // left empty -- the graph's own values are more precise.
            for (const auto& cand2 : candidates) {
                AImgInfo fallback;
                if (!DecodeAutomatic1111(cand2.text, fallback, /*populateFullParameters=*/false)) continue;
                // "workflow" is a live snapshot of the frontend's node graph
                // state at save time, not a frozen execution record: a
                // KSampler with control_after_generate randomize/increment
                // advances the displayed (and thus saved) seed the instant
                // the queued run finishes, so by the time the file is
                // written, workflow's seed can already be the NEXT run's
                // seed rather than the one that produced this image. Without
                // a "prompt" chunk (queue-time, authoritative) to trust
                // instead, prefer the companion text block's seed over a
                // disagreeing workflow-derived one rather than just filling
                // gaps.
                if (!hasPromptChunk && fallback.has_seed) {
                    info.seed = fallback.seed;
                    info.has_seed = true;
                }
                MergeGaps(info, fallback);
                break;
            }
            return info;
        }
    }

    // 2. Try Fooocus first: it emits an A1111-style parameter block, so the
    // generic A1111 check below would always match it before this branch
    // gets a chance to run.
    for (const auto& cand : candidates) {
        if (DecodeFooocus(cand.text, info)) {
            return info;
        }
    }

    // 3. Try Automatic1111 / SD.Next / Forge on candidates
    for (const auto& cand : candidates) {
        if (DecodeAutomatic1111(cand.text, info)) {
            // If the file ALSO carries a distinct ComfyUI chunk/structural marker,
            // relabel the generator as ComfyUI. Deliberately does not match on
            // substrings of the prompt text itself (e.g. a prompt mentioning
            // "a comfy chair"), only on chunk keys / graph structure markers.
            if (hasComfyUIChunk) {
                info.generator = L"ComfyUI";
            }
            return info;
        }
    }

    // 4-7. Try Easy Diffusion, then InvokeAI, then SwarmUI, then NovelAI --
    // same signature, so one generator-major/candidate-minor loop replaces
    // four near-identical ones. Order matters and must stay exactly this:
    // Easy Diffusion before InvokeAI, since Easy Diffusion's own JSON schema
    // also has a "negative_prompt" key that would otherwise satisfy
    // InvokeAI's (looser) gate and mislabel the generator.
    using JsonGeneratorDecodeFn = bool (*)(const SimpleJson::JsonValue*, const std::string&, AImgInfo&);
    static const JsonGeneratorDecodeFn kJsonGeneratorDecoders[] = {
        DecodeEasyDiffusion, DecodeInvokeAI, DecodeSwarmUI, DecodeNovelAI
    };
    for (auto decodeFn : kJsonGeneratorDecoders) {
        for (auto& cand : candidates) {
            SimpleJson::JsonValue* json = getJson(cand);
            if (json && decodeFn(json, cand.text, info)) {
                return info;
            }
        }
    }

    // 8. Fallback: If EXIF UserComment, ImageDescription or Comment has non-empty text, treat as Prompt!
    for (const auto& cand : candidates) {
        if ((cand.key == "exif:UserComment" || cand.key == "exif:ImageDescription" || cand.key == "Comment" || cand.key == "parameters" || cand.key.find("xmp:") == 0) &&
            LooksLikeGenerationText(cand.text)) {
            info.has_metadata = true;
            info.generator = hasComfyUIChunk ? L"ComfyUI" : L"AI Image";
            info.prompt = Utf8ToWstring(cand.text);
            info.full_parameters = Utf8ToWstring(cand.text);
            return info;
        }
    }

    return info;
}


// ---------------------------------------------------------------------------
// Automatic1111 Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeAutomatic1111(const std::string& paramText, AImgInfo& info, bool populateFullParameters) {
    if (paramText.empty()) return false;

    if (paramText.find("Steps:") == std::string::npos &&
        paramText.find("Negative prompt:") == std::string::npos &&
        paramText.find("Sampler:") == std::string::npos) {
        return false;
    }

    info.has_metadata = true;
    info.generator = L"Automatic1111";
    // MergeGaps (the only caller that passes populateFullParameters=false, for
    // the ComfyUI-companion-text gap-fill pass) never reads full_parameters
    // off its `fallback` result, so skip the UTF8->UTF16 conversion of the
    // whole companion text block there rather than computing and discarding it.
    if (populateFullParameters) info.full_parameters = Utf8ToWstring(paramText);

    size_t negPos = paramText.find("Negative prompt:");
    size_t stepsPos = paramText.find("Steps:");
    if (stepsPos == std::string::npos) stepsPos = paramText.find("Sampler:");

    std::string posPrompt, negPrompt, paramsLine;

    if (negPos != std::string::npos) {
        posPrompt = paramText.substr(0, negPos);
        if (stepsPos != std::string::npos && stepsPos > negPos + 16) {
            negPrompt = paramText.substr(negPos + 16, stepsPos - (negPos + 16));
            paramsLine = paramText.substr(stepsPos);
        } else if (stepsPos != std::string::npos) {
            // Degenerate/crafted input where "Steps:"/"Sampler:" falls at or
            // before the end of the "Negative prompt:" label itself: nothing
            // between them, so there's no negative prompt text to extract.
            paramsLine = paramText.substr(stepsPos);
        } else {
            negPrompt = paramText.substr(negPos + 16);
        }
    } else {
        if (stepsPos != std::string::npos) {
            posPrompt = paramText.substr(0, stepsPos);
            paramsLine = paramText.substr(stepsPos);
        } else {
            posPrompt = paramText;
        }
    }

    info.prompt = Utf8ToWstring(Trim(posPrompt));
    info.negative_prompt = Utf8ToWstring(Trim(negPrompt));

    std::string civitaiResources;
    if (!paramsLine.empty()) {
        // Values like "Lora hashes" are themselves a quoted, comma-separated
        // sub-list (e.g. Lora hashes: "name1: hash1, name2: hash2") -- a
        // plain find(", ") would split into the quotes and truncate every
        // "key: value" pair after the first embedded comma. Skip over any
        // ", " that falls inside an open double-quote span instead.
        auto findNextComma = [&](size_t from) -> size_t {
            bool inQuotes = false;
            for (size_t i = from; i < paramsLine.size(); i++) {
                if (paramsLine[i] == '"') inQuotes = !inQuotes;
                else if (!inQuotes && paramsLine[i] == ',' && i + 1 < paramsLine.size() && paramsLine[i + 1] == ' ') {
                    return i;
                }
            }
            return std::string::npos;
        };
        size_t start = 0;
        while (start < paramsLine.size()) {
            size_t nextComma = findNextComma(start);
            if (nextComma == std::string::npos) nextComma = paramsLine.size();

            std::string item = paramsLine.substr(start, nextComma - start);
            size_t colon = item.find(": ");
            if (colon != std::string::npos) {
                std::string k = Trim(item.substr(0, colon));
                std::string v = Trim(item.substr(colon + 2));

                if (k == "Steps") {
                    info.steps = atoi(v.c_str());
                    info.has_steps = true;
                } else if (k == "Sampler") {
                    info.sampler = Utf8ToWstring(v);
                } else if (k == "Schedule type" || k == "Scheduler") {
                    info.scheduler = Utf8ToWstring(v);
                } else if (k == "CFG scale") {
                    info.cfg_scale = atof(v.c_str());
                    info.has_cfg = true;
                } else if (k == "Seed") {
                    info.seed = _strtoui64(v.c_str(), NULL, 10);
                    info.has_seed = true;
                } else if (k == "Size") {
                    info.size = Utf8ToWstring(v);
                } else if (k == "Model") {
                    info.model = Utf8ToWstring(v);
                } else if (k == "Model hash") {
                    info.model_hash = Utf8ToWstring(v);
                } else if (k == "Clip skip") {
                    info.clip_skip = atoi(v.c_str());
                    info.has_clip_skip = true;
                } else if (k == "Denoising strength") {
                    info.denoising_strength = atof(v.c_str());
                    info.has_denoising_strength = true;
                } else if (k == "Hires upscale") {
                    info.hires_upscale = Utf8ToWstring(v);
                } else if (k == "Hires upscaler") {
                    info.hires_upscaler = Utf8ToWstring(v);
                } else if (k == "Hires steps") {
                    info.hires_steps = atoi(v.c_str());
                    info.has_hires_steps = true;
                } else if (k == "VAE") {
                    info.vae = Utf8ToWstring(v);
                } else if (k == "Lora hashes" || k == "Loras") {
                    info.lora = Utf8ToWstring(v);
                } else if (k == "Civitai resources") {
                    civitaiResources = v;
                }
            }
            start = nextComma + 2;
        }
    }

    // CivitAI's own generation UI omits the classic "Model:"/"Model hash:"
    // keys entirely and instead logs a "Civitai resources" JSON array of
    // {type, modelVersionId, modelName} objects; when the plain key is
    // missing, fall back to the checkpoint entry's modelName so the Model
    // column isn't left blank for images that do carry a real name (some
    // only log modelVersionId with no modelName at all -- nothing to recover
    // there).
    if (info.model.empty() && !civitaiResources.empty()) {
        SimpleJson::JsonParser parser(civitaiResources);
        auto root = parser.parse();
        if (root && root->type == SimpleJson::JsonType::Array) {
            for (const auto& item : root->arrVal) {
                if (item && item->getStr("type") == "checkpoint") {
                    std::string modelName = item->getStr("modelName");
                    if (!modelName.empty()) info.model = Utf8ToWstring(modelName);
                    break;
                }
            }
        }
    }

    return true;
}


// ---------------------------------------------------------------------------
// ComfyUI Decoder (Supports API Graph format and UI Workflow format)
// ---------------------------------------------------------------------------

namespace {

using SimpleJson::JsonType;
using SimpleJson::JsonValue;
using SimpleJson::ClampDoubleToInt64;

// Renders a resolved width/height pair in the "WxH" convention aimg_derived.h's
// ParseSize expects on the read side. Returns empty if either dimension is
// non-positive (unknown/not present).
std::wstring MakeSizeString(int64_t w, int64_t h) {
    if (w <= 0 || h <= 0) return L"";
    return std::to_wstring(w) + L"x" + std::to_wstring(h); // digits + 'x' are pure ASCII, no UTF-8 decoding needed
}

// ComfyUI graph node/link ids appear as either a JSON number or a JSON
// string depending on export format/version; normalize either to the string
// form used as the common key type across UiLinkIndex/nodesObj lookups.
std::string JsonIdToString(const JsonValue* idv) {
    if (!idv) return "";
    if (idv->type == JsonType::Number) return std::to_string((int64_t)idv->numVal);
    if (idv->type == JsonType::String) return idv->strVal;
    return "";
}

// Extracts the referenced node id from a ComfyUI graph link, which is
// encoded as a 2-element array [node_id, output_slot].
std::string GetLinkNodeId(const JsonValue* v) {
    if (!v || v->type != JsonType::Array || v->arrVal.empty()) return "";
    return JsonIdToString(v->arrVal[0].get());
}

// Follows an API-format field that may be either a literal value or a link
// ([node_id, output_slot]) through a bounded chain of "primitive" nodes
// (PrimitiveStringMultiline/PrimitiveString/PrimitiveInt/PrimitiveFloat, or
// any other node exposing its literal under a "value" or "text" input) to
// the underlying literal string. Flux/Flux2-style graphs commonly route a
// CLIPTextEncode's "text" (or a latent-image node's "width"/"height")
// through a separate primitive node instead of embedding the value directly,
// so a plain getStr("text")/getNum("width") on the referencing node alone
// would see only the link array and come back empty/zero.
std::string ResolveTextField(const JsonValue* nodesObj, const JsonValue* field, int depth = 0) {
    if (!field || depth > 4) return "";
    if (field->type == JsonType::String) return field->strVal;
    if (field->type == JsonType::Number) {
        // A resolved literal turns out to be numeric (e.g. a PrimitiveInt
        // feeding a text field indirectly) -- stringify rather than lose it.
        double n = field->numVal;
        int64_t asInt = ClampDoubleToInt64(n);
        if (n == (double)asInt) return std::to_string(asInt);
        return std::to_string(n);
    }
    if (field->type != JsonType::Array) return "";
    std::string nodeId = GetLinkNodeId(field);
    if (nodeId.empty()) return "";
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return "";
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs) return "";
    if (refInputs->objVal.count("text")) {
        std::string r = ResolveTextField(nodesObj, refInputs->objVal.at("text").get(), depth + 1);
        if (!r.empty()) return r;
    }
    if (refInputs->objVal.count("value")) {
        std::string r = ResolveTextField(nodesObj, refInputs->objVal.at("value").get(), depth + 1);
        if (!r.empty()) return r;
    }
    return "";
}

// Resolves an API-format node id to its positive/negative prompt text -- the
// exact text-encode-shaped node a KSampler's "positive"/"negative" link
// points at. Preferred over the "first/second text-encode node found" guess
// since graphs with more than two text-encode nodes (regional prompting,
// IP-adapters, etc.) make that guess unreliable. Plain CLIPTextEncode-shaped
// nodes hold a single "text" field; some custom nodes (e.g.
// TextEncodeMageFlowEdit) instead expose two distinct fields, "prompt" and
// "negative_prompt", on the SAME node -- isPositive picks between them when
// "text" isn't present.
std::string ResolveClipText(const JsonValue* nodesObj, const std::string& nodeId, bool isPositive) {
    if (nodeId.empty()) return "";
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return "";
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs) return "";
    if (refInputs->objVal.count("text")) {
        return ResolveTextField(nodesObj, refInputs->objVal.at("text").get());
    }
    const char* fallbackKey = isPositive ? "prompt" : "negative_prompt";
    if (refInputs->objVal.count(fallbackKey)) {
        return ResolveTextField(nodesObj, refInputs->objVal.at(fallbackKey).get());
    }
    return "";
}

// Given a UI-format node, returns the first (skip == 0) or the
// (skip+1)-th string found in its widgets_values. Plain CLIPTextEncode has
// the prompt at index 0; some custom prompt nodes (e.g. LoRA-manager style
// "Prompt" nodes) prepend a metadata object before the text, so taking index
// 0 unconditionally would grab the wrong element -- the first *string*
// element is right either way. skip == 1 is for a node exposing two
// CONDITIONING outputs (positive/negative) from two distinct text fields on
// the SAME node (e.g. TextEncodeMageFlowEdit): resolving both outputs would
// otherwise return the identical first string for both roles.
std::string FirstWidgetString(const JsonValue* node, int skip = 0) {
    if (!node || !node->objVal.count("widgets_values") || node->objVal.at("widgets_values")->type != JsonType::Array) return "";
    for (const auto& v : node->objVal.at("widgets_values")->arrVal) {
        if (v && v->type == JsonType::String && !v->strVal.empty()) {
            if (skip > 0) { skip--; continue; }
            return v->strVal;
        }
    }
    return "";
}

// Looks up a widget-backed input's own value in widgets_values by name, for
// UI-format nodes with more than one widget-backed text field on the same
// node (e.g. TextEncodeMageFlowEdit's "prompt"/"negative_prompt") where
// FirstWidgetString's first/second-string guess can't tell them apart by
// name. widgets_values only holds entries for widget-backed inputs (an
// "inputs" entry with a "widget" key), in the same order they're declared in
// "inputs", so the Nth such entry maps onto widgets_values[N].
std::string NamedWidgetString(const JsonValue* node, const std::string& widgetName) {
    if (!node || node->type != JsonType::Object) return "";
    if (!node->objVal.count("inputs") || node->objVal.at("inputs")->type != JsonType::Array) return "";
    if (!node->objVal.count("widgets_values") || node->objVal.at("widgets_values")->type != JsonType::Array) return "";
    const auto& wArr = node->objVal.at("widgets_values")->arrVal;
    size_t widgetIndex = 0;
    for (const auto& inp : node->objVal.at("inputs")->arrVal) {
        if (!inp || inp->type != JsonType::Object || !inp->objVal.count("widget")) continue;
        if (inp->getStr("name") == widgetName) {
            if (widgetIndex < wArr.size() && wArr[widgetIndex] && wArr[widgetIndex]->type == JsonType::String) {
                return wArr[widgetIndex]->strVal;
            }
            return "";
        }
        widgetIndex++;
    }
    return "";
}

// UI-format node/link index: maps a node's "id" to the node itself, and a
// link id to its origin node's id. Built up-front so KSampler-type nodes can
// resolve their "positive"/"negative" CONDITIONING inputs to the actual
// source node's text -- mirroring ResolveClipText above, but for the
// UI/workflow representation, where "inputs" is an array of {name, type,
// link} objects (link = a numeric id into a separate top-level/subgraph
// "links" array) rather than a named object holding the value directly.
// Without this, a UI-format graph with more than one (or, worse, exactly one
// *negative*) CLIPTextEncode-shaped node falls back to the unreliable "first
// found = positive" guess and can silently swap prompt/negative_prompt.
//
// Every node id and link id is only unique WITHIN one "scope" -- the
// top-level graph, or one subgraph definition's own nodes/links (ComfyUI's
// subgraph feature renumbers each subgraph definition's internal ids from
// scratch, so two distinct subgraph definitions embedded in the same
// workflow can easily reuse the same small node/link ids). All lookups here
// are therefore keyed by "scopeId\x1f<id>" -- scopeId is "" for the
// top-level graph and a subgraph definition's own "id" (e.g. its UUID) for
// its nodes/links -- so resolving a link never crosses into a different
// subgraph's namesake id by accident.
struct UiLinkIndex {
    // std::map, not unordered_map: same rationale as JsonValue::objVal above --
    // small per-graph N, and consolidating on one associative-container
    // backend avoids carrying both the tree-map and hash-map template
    // machinery (rehashing, bucket lists, hash<string>) in the binary for no
    // measured benefit at this scale.
    std::map<std::string, const JsonValue*> nodesById;
    std::map<std::string, std::string> linkOrigin;
    // A link whose origin is the subgraph's own boundary-input sentinel node
    // (id matches that subgraph's "inputNode") maps here instead of
    // linkOrigin -- value is the link's "origin_slot", which indexes both
    // that subgraph's own "inputs[]" (for the exposed input's name) and, more
    // importantly, the instantiating node's widgets_values[] (for the real,
    // CURRENT value -- see TryResolveBoundary). Without this, a promoted
    // widget (e.g. a prompt/seed exposed at a subgraph's boundary) resolves
    // to whatever stale/demo value was frozen on the inner node at the
    // moment its widget got converted into a socket, not the value actually
    // used to generate the image.
    std::map<std::string, int> boundarySlot;
};

// Built via reserve+append rather than operator+ chaining (scopeId + '\x1f'
// + id) to do one allocation instead of two -- called once per node/link
// during CollectUiNodesAndLinks/ResolveLinkNode/TryResolveBoundary for every
// UI-format ComfyUI graph decoded.
std::string ScopedKey(const std::string& scopeId, const std::string& id) {
    std::string key;
    key.reserve(scopeId.size() + 1 + id.size());
    key.append(scopeId);
    key.push_back('\x1f');
    key.append(id);
    return key;
}

// One subgraph definition plus (heuristically) the node that instantiates
// it. A subgraph definition can in principle be instantiated more than once
// in the same workflow, but ComfyUI's serialized JSON gives no way to tell
// which instance a given copy of the shared internal node/link template
// "belongs" to at parse time (no execution-time context available here) --
// the first top-level instance node found for a given subgraph id is used
// for every promoted-widget resolution against that subgraph. Correct for
// the overwhelming majority of real workflows (one instance per subgraph
// type); a workflow that genuinely instantiates the same subgraph twice with
// different inputs could have its promoted values misattributed between the
// two calls.
struct SubgraphInfo {
    const JsonValue* def = nullptr;
    const JsonValue* instanceNode = nullptr;
};
using SubgraphRegistry = std::map<std::string, SubgraphInfo>;

// Follows a UI-format link id to the node that originates it. Returns
// nullptr if the link or its origin node is unknown, so callers can just
// early-return on a null result instead of repeating the two-step lookup.
const JsonValue* ResolveLinkNode(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index) {
    auto it = index.linkOrigin.find(ScopedKey(scopeId, std::to_string(linkId)));
    if (it == index.linkOrigin.end()) return nullptr;
    auto nodeIt = index.nodesById.find(it->second);
    if (nodeIt == index.nodesById.end()) return nullptr;
    return nodeIt->second;
}

// Resolves a link that crosses a subgraph boundary (see
// UiLinkIndex::boundarySlot) to the real, current value: the instantiating
// node's widgets_values at that boundary slot. Returns nullptr if the link
// isn't a boundary link, the owning subgraph has no located instance node,
// or the slot is out of range -- callers fall back to the node's own
// (possibly stale) local value/positional read in all of those cases.
const JsonValue* TryResolveBoundary(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                     const SubgraphRegistry& registry) {
    auto slotIt = index.boundarySlot.find(ScopedKey(scopeId, std::to_string(linkId)));
    if (slotIt == index.boundarySlot.end()) return nullptr;
    auto regIt = registry.find(scopeId);
    if (regIt == registry.end() || !regIt->second.instanceNode) return nullptr;
    const JsonValue* inst = regIt->second.instanceNode;
    if (!inst->objVal.count("widgets_values") || inst->objVal.at("widgets_values")->type != JsonType::Array) return nullptr;
    const auto& wArr = inst->objVal.at("widgets_values")->arrVal;
    int slot = slotIt->second;
    if (slot < 0 || slot >= (int)wArr.size() || !wArr[slot]) return nullptr;
    return wArr[slot].get();
}

void CollectUiNodesAndLinks(const JsonValue* scope, const std::string& scopeId, UiLinkIndex& index) {
    if (!scope || scope->type != JsonType::Object) return;
    if (scope->objVal.count("nodes") && scope->objVal.at("nodes")->type == JsonType::Array) {
        for (const auto& n : scope->objVal.at("nodes")->arrVal) {
            if (!n || n->type != JsonType::Object || !n->objVal.count("id")) continue;
            std::string idStr = JsonIdToString(n->objVal.at("id").get());
            if (!idStr.empty()) index.nodesById[ScopedKey(scopeId, idStr)] = n.get();
        }
    }
    // A subgraph definition's own boundary-input sentinel node id (e.g.
    // "-10"): links originating there are promoted-widget crossings, routed
    // to boundarySlot instead of being treated as an ordinary node-to-node
    // link. Top-level graphs have no "inputNode".
    std::string boundaryInputId;
    if (const auto* inputNode = scope->getObj("inputNode")) {
        if (inputNode->objVal.count("id")) boundaryInputId = JsonIdToString(inputNode->objVal.at("id").get());
    }
    // Boundary (promoted-widget) links are collected here and resolved to a
    // widgets_values index in a second pass below, once every link in the
    // scope is known -- see the comment on UiLinkIndex::boundarySlot.
    struct BoundaryLink { std::string linkKey; int originSlot; std::string targetId; int targetSlot; };
    std::vector<BoundaryLink> boundaryLinks;
    if (scope->objVal.count("links") && scope->objVal.at("links")->type == JsonType::Array) {
        for (const auto& l : scope->objVal.at("links")->arrVal) {
            if (!l) continue;
            int64_t linkId = 0;
            std::string originId, targetId;
            int originSlot = -1, targetSlot = -1;
            if (l->type == JsonType::Array) {
                // Classic ComfyUI link tuple: [link_id, origin_node_id,
                // origin_slot, target_node_id, target_slot, type].
                if (l->arrVal.size() < 2 || !l->arrVal[0] || l->arrVal[0]->type != JsonType::Number || !l->arrVal[1]) continue;
                linkId = (int64_t)l->arrVal[0]->numVal;
                originId = JsonIdToString(l->arrVal[1].get());
                if (l->arrVal.size() > 2 && l->arrVal[2] && l->arrVal[2]->type == JsonType::Number) originSlot = (int)l->arrVal[2]->numVal;
                if (l->arrVal.size() > 3 && l->arrVal[3]) targetId = JsonIdToString(l->arrVal[3].get());
                if (l->arrVal.size() > 4 && l->arrVal[4] && l->arrVal[4]->type == JsonType::Number) targetSlot = (int)l->arrVal[4]->numVal;
            } else if (l->type == JsonType::Object) {
                // Newer subgraph-capable ComfyUI versions instead store each
                // link as an object: {"id": 40, "origin_id": 28,
                // "origin_slot": 0, "target_id": 6, "target_slot": 1, "type":
                // "STRING"}. Without handling this shape, every link in a
                // subgraph-based workflow is silently dropped and positive/
                // negative-prompt/text-through-a-link resolution never fires.
                if (!l->objVal.count("id") || !l->objVal.count("origin_id")) continue;
                const auto& idv = l->objVal.at("id");
                const auto& originv = l->objVal.at("origin_id");
                if (!idv || idv->type != JsonType::Number || !originv) continue;
                linkId = (int64_t)idv->numVal;
                originId = JsonIdToString(originv.get());
                if (l->objVal.count("origin_slot") && l->objVal.at("origin_slot") && l->objVal.at("origin_slot")->type == JsonType::Number) {
                    originSlot = (int)l->objVal.at("origin_slot")->numVal;
                }
                if (l->objVal.count("target_id") && l->objVal.at("target_id")) targetId = JsonIdToString(l->objVal.at("target_id").get());
                if (l->objVal.count("target_slot") && l->objVal.at("target_slot") && l->objVal.at("target_slot")->type == JsonType::Number) {
                    targetSlot = (int)l->objVal.at("target_slot")->numVal;
                }
            } else {
                continue;
            }
            if (originId.empty()) continue;
            std::string linkKey = ScopedKey(scopeId, std::to_string(linkId));
            if (!boundaryInputId.empty() && originId == boundaryInputId && originSlot >= 0) {
                boundaryLinks.push_back({linkKey, originSlot, targetId, targetSlot});
            } else {
                index.linkOrigin[linkKey] = ScopedKey(scopeId, originId);
            }
        }
    }
    if (!boundaryLinks.empty()) {
        // A subgraph's boundary "inputs[]" is a flat list mixing pure-link
        // sockets (IMAGE/MODEL/CLIP/VAE/CONDITIONING/...) with promoted
        // widgets (STRING/INT/FLOAT/BOOLEAN/COMBO/...) -- only the latter get
        // an entry in the instantiating node's widgets_values[], so
        // origin_slot cannot be used as a widgets_values index directly. For
        // example, if subgraph input 0 is a socket-only IMAGE and input 1 is
        // a promoted "prompt", the real prompt value sits at
        // widgets_values[0], not widgets_values[1] -- indexing by raw
        // origin_slot silently grabs the wrong (often numeric/boolean) slot,
        // fails whatever type check the caller applies, and falls back to
        // the inner node's own stale/demo local value instead. Determine
        // widget-backed-ness per distinct origin_slot by checking whether
        // that slot's link target actually has a "widget" key on its own
        // node (mirrors NamedWidgetString's same-node widget-index
        // counting), then assign sequential widgets_values indices in
        // origin_slot order, skipping socket-only slots entirely.
        std::map<int, bool> slotIsWidget;
        for (const auto& bl : boundaryLinks) {
            if (slotIsWidget.count(bl.originSlot)) continue;
            bool isWidget = false;
            if (!bl.targetId.empty() && bl.targetSlot >= 0) {
                auto nodeIt = index.nodesById.find(ScopedKey(scopeId, bl.targetId));
                if (nodeIt != index.nodesById.end() && nodeIt->second->objVal.count("inputs") &&
                    nodeIt->second->objVal.at("inputs")->type == JsonType::Array) {
                    const auto& inputs = nodeIt->second->objVal.at("inputs")->arrVal;
                    if (bl.targetSlot < (int)inputs.size() && inputs[bl.targetSlot] &&
                        inputs[bl.targetSlot]->type == JsonType::Object) {
                        isWidget = inputs[bl.targetSlot]->objVal.count("widget") != 0;
                    }
                }
            }
            slotIsWidget[bl.originSlot] = isWidget;
        }
        std::map<int, int> slotToWidgetIndex;
        int nextIndex = 0;
        for (const auto& kv : slotIsWidget) {
            if (kv.second) slotToWidgetIndex[kv.first] = nextIndex++;
        }
        for (const auto& bl : boundaryLinks) {
            auto it = slotToWidgetIndex.find(bl.originSlot);
            if (it != slotToWidgetIndex.end()) index.boundarySlot[bl.linkKey] = it->second;
        }
    }
}

// Looks up a named input's "link" id on a UI-format node (inputs is an array
// of {name, type, link} objects). Returns false if the input is absent or
// unwired (link is null/missing), which is the common case for a widget that
// was never converted to a socket.
bool GetNodeInputLink(const JsonValue* node, const std::string& name, int64_t& outLinkId) {
    if (!node || node->type != JsonType::Object) return false;
    if (!node->objVal.count("inputs") || node->objVal.at("inputs")->type != JsonType::Array) return false;
    for (const auto& inp : node->objVal.at("inputs")->arrVal) {
        if (!inp || inp->type != JsonType::Object) continue;
        if (inp->getStr("name") != name) continue;
        if (!inp->objVal.count("link") || !inp->objVal.at("link") || inp->objVal.at("link")->type != JsonType::Number) return false;
        outLinkId = (int64_t)inp->objVal.at("link")->numVal;
        return true;
    }
    return false;
}

std::string ResolveUiTextThroughLink(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                      const SubgraphRegistry& registry, int depth);

// Resolves a boolean-valued UI link (e.g. a ComfySwitchNode's own "switch"
// input) to its literal true/false, following PrimitiveBoolean nodes.
// Returns -1 when the value can't be determined (unknown node type, missing
// link, or a non-boolean widget) so callers can fall back to a node's own
// widgets_values default instead of guessing.
int ResolveUiBoolLink(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index, int depth) {
    if (depth > 6) return -1;
    const JsonValue* node = ResolveLinkNode(scopeId, linkId, index);
    if (!node) return -1;
    if (!node->objVal.count("widgets_values") || node->objVal.at("widgets_values")->type != JsonType::Array) return -1;
    const auto& w = node->objVal.at("widgets_values")->arrVal;
    if (!w.empty() && w[0] && w[0]->type == JsonType::Bool) return w[0]->boolVal ? 1 : 0;
    return -1;
}

// Follows a CLIPTextEncode "text" input's link through pass-through/prompt-
// routing nodes (Reroute, PreviewAny, Primitive string nodes,
// StringConcatenate, ComfySwitchNode) to the literal text feeding it --
// prompt-enhancement/LoRA-trigger workflows route text through several of
// these, leaving widgets_values[0] a stale placeholder otherwise. Bounded
// depth guards against cycles in malformed graphs. Also checks, before
// anything else, whether this link crosses a subgraph boundary (see
// TryResolveBoundary) -- a promoted text widget's real value lives on the
// subgraph's instantiating node, not on whatever node the link nominally
// points at inside the subgraph.
std::string ResolveUiTextThroughLink(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                      const SubgraphRegistry& registry, int depth) {
    if (depth > 6) return "";
    if (const JsonValue* boundaryVal = TryResolveBoundary(scopeId, linkId, index, registry)) {
        return boundaryVal->type == JsonType::String ? boundaryVal->strVal : "";
    }
    const JsonValue* node = ResolveLinkNode(scopeId, linkId, index);
    if (!node) return "";
    std::string typeStr = node->getStr("type");

    if (typeStr == "ComfySwitchNode") {
        int switchVal = -1;
        int64_t switchLink = 0;
        if (GetNodeInputLink(node, "switch", switchLink)) switchVal = ResolveUiBoolLink(scopeId, switchLink, index, depth + 1);
        if (switchVal == -1 && node->objVal.count("widgets_values") && node->objVal.at("widgets_values")->type == JsonType::Array) {
            const auto& w = node->objVal.at("widgets_values")->arrVal;
            if (!w.empty() && w[0] && w[0]->type == JsonType::Bool) switchVal = w[0]->boolVal ? 1 : 0;
        }
        int64_t branchLink = 0;
        // Prefer whichever branch the switch's own condition actually
        // selects -- but if THAT branch resolves empty (e.g. it flows
        // through an LLM/vision node like TextGenerate whose output is
        // computed at runtime and never stored in the saved workflow, so it
        // can't be recovered at all), a plausible prompt from the other
        // branch is a far better result than surfacing nothing. Try the
        // selected branch first, then both branches as a fallback, same as
        // when the switch condition itself couldn't be resolved.
        if (switchVal != -1) {
            const char* branchName = switchVal == 1 ? "on_true" : "on_false";
            if (GetNodeInputLink(node, branchName, branchLink)) {
                std::string r = ResolveUiTextThroughLink(scopeId, branchLink, index, registry, depth + 1);
                if (!r.empty()) return r;
            }
        }
        if (GetNodeInputLink(node, "on_true", branchLink)) {
            std::string r = ResolveUiTextThroughLink(scopeId, branchLink, index, registry, depth + 1);
            if (!r.empty()) return r;
        }
        if (GetNodeInputLink(node, "on_false", branchLink)) {
            return ResolveUiTextThroughLink(scopeId, branchLink, index, registry, depth + 1);
        }
        return "";
    }

    if (typeStr == "StringConcatenate") {
        int64_t aLink, bLink;
        std::string a = GetNodeInputLink(node, "string_a", aLink) ? ResolveUiTextThroughLink(scopeId, aLink, index, registry, depth + 1) : "";
        std::string b = GetNodeInputLink(node, "string_b", bLink) ? ResolveUiTextThroughLink(scopeId, bLink, index, registry, depth + 1) : "";
        std::string sep;
        if (node->objVal.count("widgets_values") && node->objVal.at("widgets_values")->type == JsonType::Array) {
            const auto& w = node->objVal.at("widgets_values")->arrVal;
            if (w.size() > 2 && w[2] && w[2]->type == JsonType::String) sep = w[2]->strVal;
        }
        if (a.empty()) return b;
        if (b.empty()) return a;
        return a + sep + b;
    }

    if (typeStr == "Reroute" || typeStr == "PreviewAny") {
        int64_t srcLink;
        if (GetNodeInputLink(node, "source", srcLink) || GetNodeInputLink(node, "value", srcLink)) {
            return ResolveUiTextThroughLink(scopeId, srcLink, index, registry, depth + 1);
        }
        if (node->objVal.count("inputs") && node->objVal.at("inputs")->type == JsonType::Array &&
            !node->objVal.at("inputs")->arrVal.empty()) {
            const auto& inp0 = node->objVal.at("inputs")->arrVal[0];
            if (inp0 && inp0->type == JsonType::Object && inp0->objVal.count("link") &&
                inp0->objVal.at("link") && inp0->objVal.at("link")->type == JsonType::Number) {
                return ResolveUiTextThroughLink(scopeId, (int64_t)inp0->objVal.at("link")->numVal, index, registry, depth + 1);
            }
        }
        return "";
    }

    // Literal source: only node types whose entire purpose is to carry a
    // fixed string literal (regardless of whether that widget was also
    // exposed as an input socket for a parent subgraph to wire from
    // outside) are trusted here. Blindly taking FirstWidgetString() of ANY
    // node type is unsafe -- an LLM/vision node like TextGenerate has no
    // literal text of its own (its real output is computed at runtime and
    // never stored in the saved workflow), but its widgets_values still
    // contains other string-typed settings (e.g. a "on"/"off" toggle), and
    // grabbing the first one produces a confidently wrong one-word "prompt"
    // instead of an honest empty result the caller can fall back from.
    if (typeStr == "PrimitiveString" || typeStr == "PrimitiveStringMultiline") {
        return FirstWidgetString(node);
    }
    return "";
}

// Resolves a KSampler-type node's "positive"/"negative" CONDITIONING link to
// the origin node's actual text. The origin node's own "text" (or, for
// dual-role nodes like TextEncodeMageFlowEdit, "prompt"/"negative_prompt")
// field may itself be linked -- directly, through pass-through nodes, or
// promoted to a subgraph boundary -- rather than a plain local widget, so
// this must resolve through ResolveUiTextThroughLink the same way the
// CLIPTextEncode/TextEncode traversal branch does, instead of blindly taking
// FirstWidgetString() of the origin node (which would silently prefer a
// stale/demo widgets_values entry over the real, current value whenever one
// happens to be linked over).
std::string ResolveUiLinkText(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                               const SubgraphRegistry& registry, bool wantNegative, int skip = 0) {
    const JsonValue* node = ResolveLinkNode(scopeId, linkId, index);
    if (!node) return "";
    int64_t fieldLink;
    if (GetNodeInputLink(node, "text", fieldLink)) {
        std::string t = ResolveUiTextThroughLink(scopeId, fieldLink, index, registry, 0);
        if (!t.empty()) return t;
    }
    const char* namedField = wantNegative ? "negative_prompt" : "prompt";
    if (GetNodeInputLink(node, namedField, fieldLink)) {
        std::string t = ResolveUiTextThroughLink(scopeId, fieldLink, index, registry, 0);
        if (!t.empty()) return t;
    }
    return FirstWidgetString(node, skip);
}

// Positional offsets into a UI-format node's widgets_values array, for node
// types whose entire purpose is to carry one or more generation parameters
// (seed/steps/cfg/sampler/scheduler/denoise) at fixed indices. -1 means "this
// node type has no widget for this field". minSize is the smallest
// widgets_values length that must be present before any index in the map is
// trusted (mirrors each node type's own widget list length).
struct WidgetFieldMap {
    int minSize;
    int seedIdx, stepsIdx, cfgIdx, denoiseIdx, samplerIdx, schedulerIdx;
};

const std::map<std::string, WidgetFieldMap>& GetWidgetFieldMaps() {
    static const std::map<std::string, WidgetFieldMap> kMaps = {
        // Standard ComfyUI-core KSampler widget order:
        // [seed, control_after_generate, steps, cfg, sampler_name, scheduler, denoise].
        {"KSampler",         {7, /*seed*/0, /*steps*/2, /*cfg*/3, /*denoise*/6, /*sampler*/4, /*scheduler*/5}},
        // [add_noise, seed, control_after_generate, steps, cfg, sampler_name,
        // scheduler, start_at_step, end_at_step, return_with_leftover_noise].
        {"KSamplerAdvanced", {7, /*seed*/1, /*steps*/3, /*cfg*/4, /*denoise*/-1, /*sampler*/5, /*scheduler*/6}},
        // "Custom sampler" pipelines (Flux/SD3-style graphs that split a
        // single KSampler into several single-purpose nodes wired together
        // via links) spread seed/cfg/sampler/scheduler/steps/denoise across
        // these node types instead of one KSampler:
        {"RandomNoise",      {1, /*seed*/0, -1, -1, -1, -1, -1}},
        {"CFGGuider",        {1, -1, -1, /*cfg*/0, -1, -1, -1}},
        {"KSamplerSelect",   {1, -1, -1, -1, -1, /*sampler*/0, -1}},
        // BasicScheduler widgets: [scheduler, steps, denoise].
        {"BasicScheduler",   {3, -1, /*steps*/1, -1, /*denoise*/2, -1, /*scheduler*/0}},
        // Flux2Scheduler widgets: [steps, width, height] -- no separate
        // scheduler-name or denoise widget of its own (the Flux2 schedule is
        // implicit), so only steps can be recovered from it.
        {"Flux2Scheduler",   {1, -1, /*steps*/0, -1, -1, -1, -1}},
    };
    return kMaps;
}

// Attempts to resolve a node's named input to a value crossing a subgraph
// boundary (see TryResolveBoundary) before any positional widgets_values
// fallback is tried. A KSampler-family node's seed/steps/cfg/sampler/
// scheduler widget can just as easily be promoted to a subgraph boundary
// input as a CLIPTextEncode's "text" -- in that case the node's own
// widgets_values entry is a frozen leftover from before the widget was
// converted to a socket, not the value actually used.
bool TryBoundaryNumber(const JsonValue* node, const char* inputName, const std::string& scopeId,
                       const UiLinkIndex& index, const SubgraphRegistry& registry, double& outNum) {
    int64_t linkId;
    if (!GetNodeInputLink(node, inputName, linkId)) return false;
    const JsonValue* v = TryResolveBoundary(scopeId, linkId, index, registry);
    if (!v || v->type != JsonType::Number) return false;
    outNum = v->numVal;
    return true;
}

bool TryBoundaryString(const JsonValue* node, const char* inputName, const std::string& scopeId,
                        const UiLinkIndex& index, const SubgraphRegistry& registry, std::string& outStr) {
    int64_t linkId;
    if (!GetNodeInputLink(node, inputName, linkId)) return false;
    const JsonValue* v = TryResolveBoundary(scopeId, linkId, index, registry);
    if (!v || v->type != JsonType::String) return false;
    outStr = v->strVal;
    return true;
}

void ApplyWidgetFieldMap(const JsonValue* node, const WidgetFieldMap& m, const std::vector<std::shared_ptr<JsonValue>>& wArr,
                          const std::string& scopeId, const UiLinkIndex& index, const SubgraphRegistry& registry,
                          AImgInfo& info, std::string& samplerName, std::string& schedulerName) {
    double num;
    if (!info.has_seed && (TryBoundaryNumber(node, "seed", scopeId, index, registry, num) ||
                           TryBoundaryNumber(node, "noise_seed", scopeId, index, registry, num))) {
        info.seed = (int64_t)num; info.has_seed = true;
    }
    if (!info.has_steps && TryBoundaryNumber(node, "steps", scopeId, index, registry, num)) {
        info.steps = (int32_t)num; info.has_steps = true;
    }
    if (!info.has_cfg && TryBoundaryNumber(node, "cfg", scopeId, index, registry, num)) {
        info.cfg_scale = num; info.has_cfg = true;
    }
    if (!info.has_denoising_strength && TryBoundaryNumber(node, "denoise", scopeId, index, registry, num)) {
        info.denoising_strength = num; info.has_denoising_strength = true;
    }
    std::string str;
    if (samplerName.empty() && TryBoundaryString(node, "sampler_name", scopeId, index, registry, str)) samplerName = str;
    if (schedulerName.empty() && TryBoundaryString(node, "scheduler", scopeId, index, registry, str)) schedulerName = str;

    if ((int)wArr.size() < m.minSize) return;
    auto numAt = [&](int idx) -> const JsonValue* {
        if (idx < 0 || idx >= (int)wArr.size() || !wArr[idx]) return nullptr;
        return wArr[idx]->type == JsonType::Number ? wArr[idx].get() : nullptr;
    };
    auto strAt = [&](int idx) -> const JsonValue* {
        if (idx < 0 || idx >= (int)wArr.size() || !wArr[idx]) return nullptr;
        return wArr[idx]->type == JsonType::String ? wArr[idx].get() : nullptr;
    };
    if (!info.has_seed) { if (const auto* v = numAt(m.seedIdx)) { info.seed = (int64_t)v->numVal; info.has_seed = true; } }
    if (!info.has_steps) { if (const auto* v = numAt(m.stepsIdx)) { info.steps = (int32_t)v->numVal; info.has_steps = true; } }
    if (!info.has_cfg) { if (const auto* v = numAt(m.cfgIdx)) { info.cfg_scale = v->numVal; info.has_cfg = true; } }
    if (!info.has_denoising_strength) { if (const auto* v = numAt(m.denoiseIdx)) { info.denoising_strength = v->numVal; info.has_denoising_strength = true; } }
    if (samplerName.empty()) { if (const auto* v = strAt(m.samplerIdx)) samplerName = v->strVal; }
    if (schedulerName.empty()) { if (const auto* v = strAt(m.schedulerIdx)) schedulerName = v->strVal; }
}

// Accumulates every field DecodeComfyUI extracts from BOTH the API-format
// traversal and the UI/workflow-format traversal into one shared state, so
// each pass only fills gaps the other left empty -- mirroring the original
// single-function version's shared locals, without a half-dozen
// by-reference out-parameters threaded through TraverseUiNodes.
struct ComfyUiExtraction {
    std::string posPromptText, negPromptText;
    std::string modelName, vaeName, samplerName, schedulerName;
    std::string uiResolvedPos, uiResolvedNeg;
};

// A UI-format node's "mode" is an editor-only run-state flag ComfyUI
// preserves in the saved workflow regardless of whether the node actually
// participates in generation: 0 = enabled (the default when absent), 2 =
// muted, 4 = bypassed. Muted/bypassed nodes -- and, by extension, whole
// subgraph instances left disabled after being used for a previous edit
// step, an alternate draft branch, or an XY-grid/experiment helper -- are
// NOT part of the actual execution and must not contribute prompt/negative
// prompt/seed/etc. text just because they still physically appear in
// "nodes[]"/a subgraph definition. Unlike the API/"prompt" execution-graph
// format (which ComfyUI already excludes disabled nodes from at queue time),
// the UI/"workflow" format keeps everything the editor last had open.
bool IsNodeDisabled(const JsonValue* node) {
    if (!node || node->type != JsonType::Object) return false;
    if (!node->objVal.count("mode")) return false;
    const auto& m = node->objVal.at("mode");
    if (!m || m->type != JsonType::Number) return false;
    int mode = (int)m->numVal;
    return mode == 2 || mode == 4;
}

// UI Graph Format traversal of one "nodes" array. Called both for the
// top-level "nodes" array and for each subgraph definition's own "nodes"
// array -- ComfyUI's subgraph feature moves the actual
// KSampler/CLIPTextEncode/etc. nodes out of the top-level list into
// definitions.subgraphs[].nodes, with the top level holding only opaque
// subgraph-instance placeholder nodes.
void TraverseUiNodes(const std::vector<std::shared_ptr<JsonValue>>& nodesArray, const std::string& scopeId,
                      const UiLinkIndex& linkIndex, const SubgraphRegistry& registry, AImgInfo& info, ComfyUiExtraction& out) {
    for (const auto& item : nodesArray) {
        if (!item || item->type != JsonType::Object) continue;
        if (IsNodeDisabled(item.get())) continue;
        std::string typeStr = item->getStr("type");

        // UI-format "inputs" is an array of {name, type, link} objects
        // (unlike the API format's named object), so any node's wired-in
        // "positive"/"negative" CONDITIONING source can be resolved the same
        // way regardless of that node's own type (KSampler, KSamplerAdvanced,
        // CFGGuider, ...).
        if (item->objVal.count("inputs") && item->objVal.at("inputs")->type == JsonType::Array) {
            int64_t posLinkId = 0, negLinkId = 0;
            bool hasPosLink = false, hasNegLink = false;
            for (const auto& inp : item->objVal.at("inputs")->arrVal) {
                if (!inp || inp->type != JsonType::Object) continue;
                std::string inName = inp->getStr("name");
                if (inName != "positive" && inName != "negative") continue;
                if (!inp->objVal.count("link") || !inp->objVal.at("link") || inp->objVal.at("link")->type != JsonType::Number) continue;
                int64_t linkId = (int64_t)inp->objVal.at("link")->numVal;
                if (inName == "positive") { posLinkId = linkId; hasPosLink = true; }
                else { negLinkId = linkId; hasNegLink = true; }
            }
            if (hasPosLink) {
                std::string resolved = ResolveUiLinkText(scopeId, posLinkId, linkIndex, registry, /*wantNegative=*/false);
                if (!resolved.empty() && out.uiResolvedPos.empty()) out.uiResolvedPos = resolved;
            }
            if (hasNegLink) {
                // If positive and negative both link into the SAME origin node --
                // a custom node exposing two CONDITIONING outputs from two
                // distinct text fields, e.g. TextEncodeMageFlowEdit -- the
                // first-non-empty-string guess used for positive would return the
                // identical text for negative too; skip past it to the next one.
                bool sameOriginAsPositive = hasPosLink && ResolveLinkNode(scopeId, posLinkId, linkIndex) == ResolveLinkNode(scopeId, negLinkId, linkIndex);
                std::string resolved = ResolveUiLinkText(scopeId, negLinkId, linkIndex, registry, /*wantNegative=*/true, sameOriginAsPositive ? 1 : 0);
                if (!resolved.empty() && out.uiResolvedNeg.empty()) out.uiResolvedNeg = resolved;
            }
        }

        // Note: JsonValue::getObj() only checks that `item` itself is an
        // Object and returns whatever is stored under the key regardless of
        // the child's own type -- it does NOT verify the child is itself an
        // Object. "widgets_values" is an Array, so it must be read via
        // objVal + an explicit Array type check, not getObj().
        if (!item->objVal.count("widgets_values") || item->objVal.at("widgets_values")->type != JsonType::Array) continue;
        const auto& wArr = item->objVal.at("widgets_values")->arrVal;

        if (typeStr == "CLIPTextEncode" || typeStr.find("TextEncode") != std::string::npos) {
            std::string text;
            int64_t textLink;
            // The "text" widget may have been converted to a wired input
            // socket (prompt-enhancement/LoRA-trigger workflows, or a
            // subgraph promoting it to its own boundary, commonly do this)
            // -- in that case widgets_values[0] is a stale leftover from
            // before the conversion, not the text actually used, so prefer
            // following the link over the raw widget value.
            if (GetNodeInputLink(item.get(), "text", textLink)) {
                text = ResolveUiTextThroughLink(scopeId, textLink, linkIndex, registry, 0);
            }
            // Custom nodes exposing separate "prompt"/"negative_prompt" fields
            // on the SAME node (e.g. TextEncodeMageFlowEdit) rather than a
            // shared "text" -- each may itself be linked (directly, or
            // promoted to a subgraph boundary) rather than a plain local
            // widget, so resolve through the link BEFORE falling back to
            // wArr[0]/the node's own local widget value below -- checking the
            // raw widgets_values first would grab a stale linked-over value
            // whenever one happens to be present.
            std::string negFromNamedField;
            int64_t negLink;
            if (GetNodeInputLink(item.get(), "negative_prompt", negLink)) {
                negFromNamedField = ResolveUiTextThroughLink(scopeId, negLink, linkIndex, registry, 0);
            }
            if (text.empty()) {
                int64_t promptLink;
                if (GetNodeInputLink(item.get(), "prompt", promptLink)) {
                    text = ResolveUiTextThroughLink(scopeId, promptLink, linkIndex, registry, 0);
                }
            }
            if (text.empty() && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
                text = wArr[0]->strVal;
            }
            if (negFromNamedField.empty()) negFromNamedField = NamedWidgetString(item.get(), "negative_prompt");
            if (text.empty()) text = NamedWidgetString(item.get(), "prompt");
            if (!text.empty()) {
                if (out.posPromptText.empty()) out.posPromptText = text;
                else if (out.negPromptText.empty()) out.negPromptText = text;
            }
            if (!negFromNamedField.empty() && out.negPromptText.empty()) out.negPromptText = negFromNamedField;
        } else if (typeStr == "Load Checkpoint" || typeStr.find("CheckpointLoader") != std::string::npos ||
                   typeStr.find("UNETLoader") != std::string::npos || typeStr.find("DiffusionModelLoader") != std::string::npos) {
            std::string name;
            if (out.modelName.empty()) {
                if (TryBoundaryString(item.get(), "ckpt_name", scopeId, linkIndex, registry, name) ||
                    TryBoundaryString(item.get(), "unet_name", scopeId, linkIndex, registry, name) ||
                    TryBoundaryString(item.get(), "model_name", scopeId, linkIndex, registry, name)) {
                    out.modelName = name;
                } else if (!wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
                    out.modelName = wArr[0]->strVal;
                }
            }
        } else if (typeStr == "VAELoader") {
            std::string name;
            if (out.vaeName.empty()) {
                if (TryBoundaryString(item.get(), "vae_name", scopeId, linkIndex, registry, name)) {
                    out.vaeName = name;
                } else if (!wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
                    out.vaeName = wArr[0]->strVal;
                }
            }
        } else {
            // Standard fixed-position generation-parameter node types
            // (KSampler and its custom-sampler-pipeline siblings): looked up
            // in the table above instead of a long if/else-if chain.
            auto it = GetWidgetFieldMaps().find(typeStr);
            if (it != GetWidgetFieldMaps().end()) {
                ApplyWidgetFieldMap(item.get(), it->second, wArr, scopeId, linkIndex, registry, info, out.samplerName, out.schedulerName);
            }
        }
    }
}

} // namespace

bool AImgDecoder::DecodeComfyUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;

    const SimpleJson::JsonValue* nodesObj = root;
    if (root->objVal.count("prompt") && root->objVal.at("prompt")->type == SimpleJson::JsonType::Object) {
        nodesObj = root->objVal.at("prompt").get();
    } else if (root->objVal.count("workflow") && root->objVal.at("workflow")->type == SimpleJson::JsonType::Object) {
        nodesObj = root->objVal.at("workflow").get();
    }

    // Require at least one node that actually looks like a ComfyUI graph node
    // (API format: an object with both "class_type" and "inputs"; UI format:
    // a "nodes" array entry with a "type") before claiming this JSON as
    // ComfyUI. Without this, any unrelated JSON object -- including
    // InvokeAI/SwarmUI/NovelAI's own metadata JSON -- would match here first
    // (this is the first generator tried) and those decoders would never run.
    bool looksLikeComfyGraph = false;
    for (const auto& pair : nodesObj->objVal) {
        const auto* node = pair.second.get();
        if (node && node->type == SimpleJson::JsonType::Object &&
            !node->getStr("class_type").empty() && node->getObj("inputs")) {
            looksLikeComfyGraph = true;
            break;
        }
    }
    if (!looksLikeComfyGraph && nodesObj->objVal.count("nodes") &&
        nodesObj->objVal.at("nodes")->type == SimpleJson::JsonType::Array) {
        for (const auto& item : nodesObj->objVal.at("nodes")->arrVal) {
            if (item && item->type == SimpleJson::JsonType::Object && !item->getStr("type").empty()) {
                looksLikeComfyGraph = true;
                break;
            }
        }
    }
    if (!looksLikeComfyGraph) return false;

    info.has_metadata = true;
    info.generator = L"ComfyUI";

    std::string widthHeight;
    std::vector<std::string> loras;
    std::string positiveNodeId, negativeNodeId;
    ComfyUiExtraction ui;

    // 1. API Graph Format Traversal
    for (const auto& pair : nodesObj->objVal) {
        const auto* node = pair.second.get();
        if (!node || node->type != SimpleJson::JsonType::Object) continue;

        std::string classType = node->getStr("class_type");
        const auto* inputs = node->getObj("inputs");
        if (!inputs) continue;

        // Flux/SD3-style "custom sampler" graphs split a single KSampler into
        // several single-purpose nodes wired together via links --
        // RandomNoise (seed), CFGGuider (cfg + positive/negative), a
        // KSamplerSelect (sampler name), and a scheduler node (steps/denoise,
        // e.g. BasicScheduler/Flux2Scheduler) -- so route all of them through
        // this same key-presence-driven block rather than just literal
        // "KSampler"/"...Sampler" node types.
        if (classType == "KSampler" || classType == "KSamplerAdvanced" || classType == "KSamplerSelect" ||
            classType == "CFGGuider" || classType == "RandomNoise" ||
            classType.find("Sampler") != std::string::npos || classType.find("Scheduler") != std::string::npos) {
            if (inputs->objVal.count("seed")) {
                info.seed = inputs->getInt64("seed");
                info.has_seed = true;
            } else if (inputs->objVal.count("noise_seed")) {
                info.seed = inputs->getInt64("noise_seed");
                info.has_seed = true;
            }
            if (inputs->objVal.count("steps")) {
                info.steps = (int32_t)inputs->getInt64("steps");
                info.has_steps = true;
            }
            if (inputs->objVal.count("cfg")) {
                info.cfg_scale = inputs->getNum("cfg");
                info.has_cfg = true;
            }
            if (inputs->objVal.count("sampler_name")) {
                ui.samplerName = inputs->getStr("sampler_name");
            }
            if (inputs->objVal.count("scheduler")) {
                ui.schedulerName = inputs->getStr("scheduler");
            }
            if (inputs->objVal.count("denoise")) {
                info.denoising_strength = inputs->getNum("denoise");
                info.has_denoising_strength = true;
            }
            if (positiveNodeId.empty() && inputs->objVal.count("positive")) {
                positiveNodeId = GetLinkNodeId(inputs->objVal.at("positive").get());
            }
            if (negativeNodeId.empty() && inputs->objVal.count("negative")) {
                negativeNodeId = GetLinkNodeId(inputs->objVal.at("negative").get());
            }
        }

        if (classType == "CLIPTextEncode" || classType == "BNK_CLIPTextEncodeAdvanced" ||
            classType.find("Prompt") != std::string::npos || classType.find("TextEncode") != std::string::npos) {
            if (inputs->objVal.count("text")) {
                std::string text = ResolveTextField(nodesObj, inputs->objVal.at("text").get());
                if (!text.empty()) {
                    if (ui.posPromptText.empty()) ui.posPromptText = text;
                    else if (ui.negPromptText.empty()) ui.negPromptText = text;
                }
            } else {
                // Custom nodes exposing separate "prompt"/"negative_prompt"
                // fields on the SAME node (e.g. TextEncodeMageFlowEdit)
                // instead of a shared "text" -- read both by name rather than
                // the encounter-order guess above, which would otherwise only
                // ever see one of the two fields on such a node.
                if (ui.posPromptText.empty() && inputs->objVal.count("prompt")) {
                    std::string t = ResolveTextField(nodesObj, inputs->objVal.at("prompt").get());
                    if (!t.empty()) ui.posPromptText = t;
                }
                if (ui.negPromptText.empty() && inputs->objVal.count("negative_prompt")) {
                    std::string t = ResolveTextField(nodesObj, inputs->objVal.at("negative_prompt").get());
                    if (!t.empty()) ui.negPromptText = t;
                }
            }
        }

        // Custom-node packs commonly wrap the stock loader in a prefixed
        // variant (e.g. "ECHOCheckpointLoaderSimple") that keeps the same
        // "ckpt_name"/"unet_name" input but isn't literally one of the
        // stock class_type strings, so match on substring like the
        // "Sampler"/"Scheduler" routing above rather than an exact list.
        if (classType == "Load Checkpoint" || classType.find("CheckpointLoader") != std::string::npos ||
            classType.find("DiffusionModelLoader") != std::string::npos || classType.find("UNETLoader") != std::string::npos) {
            if (inputs->objVal.count("ckpt_name")) ui.modelName = inputs->getStr("ckpt_name");
            else if (inputs->objVal.count("unet_name")) ui.modelName = inputs->getStr("unet_name");
            else if (inputs->objVal.count("model_name")) ui.modelName = inputs->getStr("model_name");
        }

        if (classType == "VAELoader" && inputs->objVal.count("vae_name")) {
            ui.vaeName = inputs->getStr("vae_name");
        }

        if ((classType == "LoraLoader" || classType == "LoraLoaderModelOnly") && inputs->objVal.count("lora_name")) {
            loras.push_back(inputs->getStr("lora_name"));
        }

        if ((classType == "EmptyLatentImage" || classType == "EmptySD3LatentImage" || classType.find("LatentImage") != std::string::npos) &&
            inputs->objVal.count("width") && inputs->objVal.count("height")) {
            // Width/height are plain numbers in most graphs, but Flux2-style
            // graphs route them through a separate PrimitiveInt node instead
            // -- resolve through the same link-following helper as prompt text.
            std::string wStr = ResolveTextField(nodesObj, inputs->objVal.at("width").get());
            std::string hStr = ResolveTextField(nodesObj, inputs->objVal.at("height").get());
            if (!wStr.empty() && !hStr.empty()) widthHeight = wStr + "x" + hStr;
        }
    }

    // Prefer resolving the exact positive/negative CLIPTextEncode nodes wired
    // into the KSampler's "positive"/"negative" inputs over the "first/second
    // found" guess above, since graphs with more than two text-encode nodes
    // (regional prompting, IP-adapters, etc.) make that guess unreliable.
    std::string resolvedPos = ResolveClipText(nodesObj, positiveNodeId, /*isPositive=*/true);
    std::string resolvedNeg = ResolveClipText(nodesObj, negativeNodeId, /*isPositive=*/false);
    if (!resolvedPos.empty()) ui.posPromptText = resolvedPos;
    if (!resolvedNeg.empty()) ui.negPromptText = resolvedNeg;

    // Collect every subgraph definition up front (handles both the array and
    // legacy object-map serialization of definitions.subgraphs), then locate
    // each one's instantiating node in the top-level "nodes" array --
    // first-match heuristic; see SubgraphRegistry -- before any traversal, so
    // promoted-widget resolution always has somewhere to look up the real
    // (current) value instead of a stale one frozen inside the subgraph.
    std::vector<const SimpleJson::JsonValue*> subgraphDefs;
    if (const auto* defs = nodesObj->getObj("definitions")) {
        if (defs->objVal.count("subgraphs")) {
            const auto* sgVal = defs->objVal.at("subgraphs").get();
            if (sgVal && sgVal->type == SimpleJson::JsonType::Array) {
                for (const auto& sg : sgVal->arrVal) if (sg) subgraphDefs.push_back(sg.get());
            } else if (sgVal && sgVal->type == SimpleJson::JsonType::Object) {
                for (const auto& kv : sgVal->objVal) if (kv.second) subgraphDefs.push_back(kv.second.get());
            }
        }
    }

    SubgraphRegistry registry;
    for (const auto* sg : subgraphDefs) {
        if (!sg || sg->type != SimpleJson::JsonType::Object || !sg->objVal.count("id")) continue;
        std::string sgId = JsonIdToString(sg->objVal.at("id").get());
        if (!sgId.empty()) registry[sgId].def = sg;
    }
    if (nodesObj->objVal.count("nodes") && nodesObj->objVal.at("nodes")->type == SimpleJson::JsonType::Array) {
        for (const auto& n : nodesObj->objVal.at("nodes")->arrVal) {
            if (!n || n->type != SimpleJson::JsonType::Object) continue;
            auto regIt = registry.find(n->getStr("type"));
            if (regIt != registry.end() && !regIt->second.instanceNode) regIt->second.instanceNode = n.get();
        }
    }

    // A subgraph whose instantiating node is missing (defined but never
    // actually placed in the graph -- e.g. a leftover/unused template) or
    // disabled (mode 2/4 -- see IsNodeDisabled) contributes nothing to the
    // actual generation: skip it entirely, for both link collection and
    // widget traversal, rather than let its internal CLIPTextEncode/KSampler
    // nodes -- an inactive alternate-edit-step branch, XY-grid experiment,
    // etc. -- pollute prompt/negative_prompt/seed via the "any node found
    // anywhere in the document" traversal below.
    auto subgraphInstanceActive = [&](const std::string& sgId) {
        auto it = registry.find(sgId);
        return it != registry.end() && it->second.instanceNode && !IsNodeDisabled(it->second.instanceNode);
    };

    // UI-format node/link index, built up-front (before widget extraction) --
    // see UiLinkIndex/CollectUiNodesAndLinks above. Top-level graph uses scope
    // id "" ; each subgraph definition uses its own "id" as scope, since node
    // ids and link ids are only unique WITHIN one scope.
    UiLinkIndex linkIndex;
    CollectUiNodesAndLinks(nodesObj, "", linkIndex);
    for (const auto* sg : subgraphDefs) {
        std::string sgId = (sg->objVal.count("id")) ? JsonIdToString(sg->objVal.at("id").get()) : "";
        if (!subgraphInstanceActive(sgId)) continue;
        CollectUiNodesAndLinks(sg, sgId, linkIndex);
    }

    // 2. UI Graph Format Traversal ("nodes" array), plus subgraph definitions
    // (definitions.subgraphs[].nodes / .subgraphs{}.nodes) -- ComfyUI's
    // subgraph feature moves the actual KSampler/CLIPTextEncode/etc. nodes
    // out of the top-level list into there, with the top level holding only
    // opaque subgraph-instance placeholder nodes.
    if (nodesObj->objVal.count("nodes") && nodesObj->objVal.at("nodes")->type == SimpleJson::JsonType::Array) {
        TraverseUiNodes(nodesObj->objVal.at("nodes")->arrVal, "", linkIndex, registry, info, ui);
    }
    for (const auto* sg : subgraphDefs) {
        if (!sg->objVal.count("nodes") || sg->objVal.at("nodes")->type != SimpleJson::JsonType::Array) continue;
        std::string sgId = sg->objVal.count("id") ? JsonIdToString(sg->objVal.at("id").get()) : "";
        if (!subgraphInstanceActive(sgId)) continue;
        TraverseUiNodes(sg->objVal.at("nodes")->arrVal, sgId, linkIndex, registry, info, ui);
    }

    // Link-resolved text wins over the "first/second CLIPTextEncode found"
    // guess made during traversal, mirroring resolvedPos/resolvedNeg above.
    if (!ui.uiResolvedPos.empty()) ui.posPromptText = ui.uiResolvedPos;
    if (!ui.uiResolvedNeg.empty()) ui.negPromptText = ui.uiResolvedNeg;

    // modelName/vaeName alone aren't enough to call this a success: a loader
    // node is easy to find even when the real generation params live in node
    // types we don't understand, and claiming success here would block the
    // A1111-style fallback text some ComfyUI save nodes also embed. Require
    // genuine generation-parameter signal, not just loader metadata.
    bool foundAnything = !ui.posPromptText.empty() || !ui.negPromptText.empty() ||
                          !ui.samplerName.empty() || !ui.schedulerName.empty() ||
                          info.has_seed || info.has_steps || info.has_cfg;
    if (!foundAnything) {
        info.has_metadata = false;
        info.generator.clear();
        return false;
    }

    info.prompt = Utf8ToWstring(ui.posPromptText);
    info.negative_prompt = Utf8ToWstring(ui.negPromptText);
    info.model = Utf8ToWstring(ui.modelName);
    info.vae = Utf8ToWstring(ui.vaeName);
    info.sampler = Utf8ToWstring(ui.samplerName);
    info.scheduler = Utf8ToWstring(ui.schedulerName);
    info.size = Utf8ToWstring(widthHeight);

    if (!loras.empty()) {
        std::string joined;
        for (const auto& l : loras) {
            if (!joined.empty()) joined += ", ";
            joined += l;
        }
        info.lora = Utf8ToWstring(joined);
    }

    info.full_parameters = Utf8ToWstring(originalText);

    return true;
}


// ---------------------------------------------------------------------------
// Easy Diffusion Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeEasyDiffusion(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "use_stable_diffusion_model" (and its "use_*_model" siblings) is
    // distinctly Easy Diffusion's own field-naming convention. Must be
    // checked before InvokeAI: Easy Diffusion's JSON also happens to carry a
    // "negative_prompt" key, which alone would satisfy InvokeAI's looser gate.
    if (!root->objVal.count("use_stable_diffusion_model")) return false;

    info.has_metadata = true;
    info.generator = L"Easy Diffusion";
    info.prompt = Utf8ToWstring(root->getStr("prompt"));
    info.negative_prompt = Utf8ToWstring(root->getStr("negative_prompt"));
    info.model = Utf8ToWstring(root->getStr("use_stable_diffusion_model"));
    info.vae = Utf8ToWstring(root->getStr("use_vae_model"));
    info.lora = Utf8ToWstring(root->getStr("use_lora_model"));
    info.sampler = Utf8ToWstring(root->getStr("sampler_name"));

    if (root->objVal.count("seed")) {
        info.seed = root->getInt64("seed");
        info.has_seed = true;
    }
    if (root->objVal.count("num_inference_steps")) {
        info.steps = (int32_t)root->getInt64("num_inference_steps");
        info.has_steps = true;
    }
    if (root->objVal.count("guidance_scale")) {
        info.cfg_scale = root->getNum("guidance_scale");
        info.has_cfg = true;
    }
    // Easy Diffusion's "prompt_strength" (img2img mode only) is the same
    // concept as A1111's "Denoising strength": how much the source image is
    // allowed to change.
    if (root->objVal.count("prompt_strength")) {
        info.denoising_strength = root->getNum("prompt_strength");
        info.has_denoising_strength = true;
    }

    info.size = MakeSizeString(root->getInt64("width"), root->getInt64("height"));

    info.full_parameters = Utf8ToWstring(originalText);
    return true;
}


// ---------------------------------------------------------------------------
// InvokeAI / SwarmUI / NovelAI Decoders
// ---------------------------------------------------------------------------
//
// These three share one shape: gate on a generator-distinctive key (so an
// unrelated JSON object -- including another of these three's own metadata
// -- doesn't falsely match), then pull prompt/negative-prompt/model/cfg/
// sampler out of flat top-level keys whose *names* differ per generator.
// Parameterized into one helper; each Decode* below just supplies the key
// names. Easy Diffusion is NOT folded in here despite a similar shape: it
// has extra fields (vae/lora/denoising-strength/size) with no equivalent
// in this shape, so forcing it in would add more config surface than it
// would remove duplication.
namespace {
struct SimpleGeneratorConfig {
    const char* gateKey1;             // required key OR gateKey2 present to claim this JSON
    const char* gateKey2;              // "" if only one gate key
    const wchar_t* generatorName;
    const char* promptKey;
    const char* promptFallbackKey;     // used when promptKey's value is empty; "" if unused
    const char* negPromptKey;
    const char* modelKey;              // "" if this generator has no model field
    const char* cfgKey;                // passed through to ExtractSeedCfgSteps
    const char* samplerKey;            // "" if this generator has no sampler field
};
}

bool AImgDecoder::DecodeSimpleGraphGenerator(const SimpleJson::JsonValue* root, const std::string& originalText,
                                              AImgInfo& info, const void* cfgVoid) {
    const auto& cfg = *static_cast<const SimpleGeneratorConfig*>(cfgVoid);
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    bool gated = root->objVal.count(cfg.gateKey1) ||
                 (cfg.gateKey2[0] != '\0' && root->objVal.count(cfg.gateKey2));
    if (!gated) return false;

    info.has_metadata = true;
    info.generator = cfg.generatorName;
    std::string prompt = root->getStr(cfg.promptKey);
    if (prompt.empty() && cfg.promptFallbackKey[0] != '\0') prompt = root->getStr(cfg.promptFallbackKey);
    info.prompt = Utf8ToWstring(prompt);
    info.negative_prompt = Utf8ToWstring(root->getStr(cfg.negPromptKey));
    if (cfg.modelKey[0] != '\0') info.model = Utf8ToWstring(root->getStr(cfg.modelKey));
    ExtractSeedCfgSteps(root, cfg.cfgKey, info);
    if (cfg.samplerKey[0] != '\0') info.sampler = Utf8ToWstring(root->getStr(cfg.samplerKey));
    info.full_parameters = Utf8ToWstring(originalText);
    return true;
}

bool AImgDecoder::DecodeInvokeAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    // "positive_prompt"/"negative_prompt" naming is distinctly InvokeAI;
    // without this gate any JSON object (including SwarmUI/NovelAI's own
    // metadata) would match here, since this decoder runs before them.
    static const SimpleGeneratorConfig cfg = {
        "positive_prompt", "negative_prompt", L"InvokeAI",
        "positive_prompt", "prompt", "negative_prompt", "model", "cfg_scale", "scheduler"
    };
    return DecodeSimpleGraphGenerator(root, originalText, info, &cfg);
}


// ---------------------------------------------------------------------------
// SwarmUI Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeSwarmUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    // "cfgscale"/"negativeprompt" (no separator) is SwarmUI's distinctive
    // naming, as opposed to A1111's "CFG scale" or InvokeAI/NovelAI's
    // "cfg_scale"/"scale" -- without this gate any JSON object would match.
    static const SimpleGeneratorConfig cfg = {
        "cfgscale", "negativeprompt", L"SwarmUI",
        "prompt", "", "negativeprompt", "model", "cfgscale", ""
    };
    return DecodeSimpleGraphGenerator(root, originalText, info, &cfg);
}


// ---------------------------------------------------------------------------
// Fooocus Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeFooocus(const std::string& paramText, AImgInfo& info) {
    if (paramText.find("Fooocus") == std::string::npos && paramText.find("Base Model:") == std::string::npos) {
        return false;
    }
    // Fooocus emits an A1111-style parameter block, so reuse that parser and
    // then relabel the generator (DecodeAutomatic1111 unconditionally sets it
    // to "Automatic1111").
    if (!DecodeAutomatic1111(paramText, info)) return false;
    info.generator = L"Fooocus";
    return true;
}


// ---------------------------------------------------------------------------
// NovelAI Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeNovelAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    // "uc" (undesired content = negative prompt) is distinctly NovelAI naming
    // -- without this gate any JSON object would match here.
    static const SimpleGeneratorConfig cfg = {
        "uc", "", L"NovelAI",
        "prompt", "", "uc", "", "scale", "sampler"
    };
    if (!DecodeSimpleGraphGenerator(root, originalText, info, &cfg)) return false;
    info.size = MakeSizeString(root->getInt64("width"), root->getInt64("height"));
    return true;
}
