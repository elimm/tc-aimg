#include "aimg_decoder.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <cwchar>
#include <windows.h>
#include <map>
#include <unordered_map>
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

    // 4. Try Easy Diffusion. Must run before InvokeAI: Easy Diffusion's own
    // JSON schema also has a "negative_prompt" key, which would otherwise
    // satisfy InvokeAI's (looser) gate below and mislabel the generator.
    for (auto& cand : candidates) {
        SimpleJson::JsonValue* json = getJson(cand);
        if (json && DecodeEasyDiffusion(json, cand.text, info)) {
            return info;
        }
    }

    // 5. Try InvokeAI
    for (auto& cand : candidates) {
        SimpleJson::JsonValue* json = getJson(cand);
        if (json && DecodeInvokeAI(json, cand.text, info)) {
            return info;
        }
    }

    // 6. Try SwarmUI
    for (auto& cand : candidates) {
        SimpleJson::JsonValue* json = getJson(cand);
        if (json && DecodeSwarmUI(json, cand.text, info)) {
            return info;
        }
    }

    // 7. Try NovelAI
    for (auto& cand : candidates) {
        SimpleJson::JsonValue* json = getJson(cand);
        if (json && DecodeNovelAI(json, cand.text, info)) {
            return info;
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

// Resolves an API-format node id to its "inputs.text" value -- the exact
// CLIPTextEncode-shaped node a KSampler's "positive"/"negative" link points
// at. Preferred over the "first/second CLIPTextEncode found" guess since
// graphs with more than two text-encode nodes (regional prompting,
// IP-adapters, etc.) make that guess unreliable.
std::string ResolveClipText(const JsonValue* nodesObj, const std::string& nodeId) {
    if (nodeId.empty()) return "";
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return "";
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs || !refInputs->objVal.count("text")) return "";
    return ResolveTextField(nodesObj, refInputs->objVal.at("text").get());
}

// Given a UI-format node, returns the first string found in its
// widgets_values. Plain CLIPTextEncode has the prompt at index 0; some custom
// prompt nodes (e.g. LoRA-manager style "Prompt" nodes) prepend a metadata
// object before the text, so taking index 0 unconditionally would grab the
// wrong element -- the first *string* element is right either way.
std::string FirstWidgetString(const JsonValue* node) {
    if (!node || !node->objVal.count("widgets_values") || node->objVal.at("widgets_values")->type != JsonType::Array) return "";
    for (const auto& v : node->objVal.at("widgets_values")->arrVal) {
        if (v && v->type == JsonType::String && !v->strVal.empty()) return v->strVal;
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
struct UiLinkIndex {
    std::unordered_map<std::string, const JsonValue*> nodesById;
    std::unordered_map<int64_t, std::string> linkOrigin;
};

// Follows a UI-format link id to the node that originates it. Returns
// nullptr if the link or its origin node is unknown, so callers can just
// early-return on a null result instead of repeating the two-step lookup.
const JsonValue* ResolveLinkNode(int64_t linkId, const UiLinkIndex& index) {
    auto it = index.linkOrigin.find(linkId);
    if (it == index.linkOrigin.end()) return nullptr;
    auto nodeIt = index.nodesById.find(it->second);
    if (nodeIt == index.nodesById.end()) return nullptr;
    return nodeIt->second;
}

void CollectUiNodesAndLinks(const JsonValue* scope, UiLinkIndex& index) {
    if (!scope || scope->type != JsonType::Object) return;
    if (scope->objVal.count("nodes") && scope->objVal.at("nodes")->type == JsonType::Array) {
        for (const auto& n : scope->objVal.at("nodes")->arrVal) {
            if (!n || n->type != JsonType::Object || !n->objVal.count("id")) continue;
            std::string idStr = JsonIdToString(n->objVal.at("id").get());
            if (!idStr.empty()) index.nodesById[idStr] = n.get();
        }
    }
    if (scope->objVal.count("links") && scope->objVal.at("links")->type == JsonType::Array) {
        for (const auto& l : scope->objVal.at("links")->arrVal) {
            if (!l) continue;
            if (l->type == JsonType::Array) {
                // Classic ComfyUI link tuple: [link_id, origin_node_id,
                // origin_slot, target_node_id, target_slot, type].
                if (l->arrVal.size() < 2 || !l->arrVal[0] || l->arrVal[0]->type != JsonType::Number || !l->arrVal[1]) continue;
                int64_t linkId = (int64_t)l->arrVal[0]->numVal;
                std::string originId = JsonIdToString(l->arrVal[1].get());
                if (!originId.empty()) index.linkOrigin[linkId] = originId;
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
                int64_t linkId = (int64_t)idv->numVal;
                std::string originId = JsonIdToString(originv.get());
                if (!originId.empty()) index.linkOrigin[linkId] = originId;
            }
        }
    }
}

std::string ResolveUiLinkText(int64_t linkId, const UiLinkIndex& index) {
    const JsonValue* node = ResolveLinkNode(linkId, index);
    return node ? FirstWidgetString(node) : "";
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

std::string ResolveUiTextThroughLink(int64_t linkId, const UiLinkIndex& index, int depth);

// Resolves a boolean-valued UI link (e.g. a ComfySwitchNode's own "switch"
// input) to its literal true/false, following PrimitiveBoolean nodes.
// Returns -1 when the value can't be determined (unknown node type, missing
// link, or a non-boolean widget) so callers can fall back to a node's own
// widgets_values default instead of guessing.
int ResolveUiBoolLink(int64_t linkId, const UiLinkIndex& index, int depth) {
    if (depth > 6) return -1;
    const JsonValue* node = ResolveLinkNode(linkId, index);
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
// depth guards against cycles in malformed graphs.
std::string ResolveUiTextThroughLink(int64_t linkId, const UiLinkIndex& index, int depth) {
    if (depth > 6) return "";
    const JsonValue* node = ResolveLinkNode(linkId, index);
    if (!node) return "";
    std::string typeStr = node->getStr("type");

    if (typeStr == "ComfySwitchNode") {
        int switchVal = -1;
        int64_t switchLink = 0;
        if (GetNodeInputLink(node, "switch", switchLink)) switchVal = ResolveUiBoolLink(switchLink, index, depth + 1);
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
                std::string r = ResolveUiTextThroughLink(branchLink, index, depth + 1);
                if (!r.empty()) return r;
            }
        }
        if (GetNodeInputLink(node, "on_true", branchLink)) {
            std::string r = ResolveUiTextThroughLink(branchLink, index, depth + 1);
            if (!r.empty()) return r;
        }
        if (GetNodeInputLink(node, "on_false", branchLink)) {
            return ResolveUiTextThroughLink(branchLink, index, depth + 1);
        }
        return "";
    }

    if (typeStr == "StringConcatenate") {
        int64_t aLink, bLink;
        std::string a = GetNodeInputLink(node, "string_a", aLink) ? ResolveUiTextThroughLink(aLink, index, depth + 1) : "";
        std::string b = GetNodeInputLink(node, "string_b", bLink) ? ResolveUiTextThroughLink(bLink, index, depth + 1) : "";
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
            return ResolveUiTextThroughLink(srcLink, index, depth + 1);
        }
        if (node->objVal.count("inputs") && node->objVal.at("inputs")->type == JsonType::Array &&
            !node->objVal.at("inputs")->arrVal.empty()) {
            const auto& inp0 = node->objVal.at("inputs")->arrVal[0];
            if (inp0 && inp0->type == JsonType::Object && inp0->objVal.count("link") &&
                inp0->objVal.at("link") && inp0->objVal.at("link")->type == JsonType::Number) {
                return ResolveUiTextThroughLink((int64_t)inp0->objVal.at("link")->numVal, index, depth + 1);
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

const std::unordered_map<std::string, WidgetFieldMap>& GetWidgetFieldMaps() {
    static const std::unordered_map<std::string, WidgetFieldMap> kMaps = {
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

void ApplyWidgetFieldMap(const WidgetFieldMap& m, const std::vector<std::shared_ptr<JsonValue>>& wArr,
                          AImgInfo& info, std::string& samplerName, std::string& schedulerName) {
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

// UI Graph Format traversal of one "nodes" array. Called both for the
// top-level "nodes" array and for each subgraph definition's own "nodes"
// array -- ComfyUI's subgraph feature moves the actual
// KSampler/CLIPTextEncode/etc. nodes out of the top-level list into
// definitions.subgraphs[].nodes, with the top level holding only opaque
// subgraph-instance placeholder nodes.
void TraverseUiNodes(const std::vector<std::shared_ptr<JsonValue>>& nodesArray,
                      const UiLinkIndex& linkIndex, AImgInfo& info, ComfyUiExtraction& out) {
    for (const auto& item : nodesArray) {
        if (!item || item->type != JsonType::Object) continue;
        std::string typeStr = item->getStr("type");

        // UI-format "inputs" is an array of {name, type, link} objects
        // (unlike the API format's named object), so any node's wired-in
        // "positive"/"negative" CONDITIONING source can be resolved the same
        // way regardless of that node's own type (KSampler, KSamplerAdvanced,
        // CFGGuider, ...).
        if (item->objVal.count("inputs") && item->objVal.at("inputs")->type == JsonType::Array) {
            for (const auto& inp : item->objVal.at("inputs")->arrVal) {
                if (!inp || inp->type != JsonType::Object) continue;
                std::string inName = inp->getStr("name");
                if (inName != "positive" && inName != "negative") continue;
                if (!inp->objVal.count("link") || !inp->objVal.at("link") || inp->objVal.at("link")->type != JsonType::Number) continue;
                std::string resolved = ResolveUiLinkText((int64_t)inp->objVal.at("link")->numVal, linkIndex);
                if (resolved.empty()) continue;
                if (inName == "positive" && out.uiResolvedPos.empty()) out.uiResolvedPos = resolved;
                else if (inName == "negative" && out.uiResolvedNeg.empty()) out.uiResolvedNeg = resolved;
            }
        }

        // Note: JsonValue::getObj() only checks that `item` itself is an
        // Object and returns whatever is stored under the key regardless of
        // the child's own type -- it does NOT verify the child is itself an
        // Object. "widgets_values" is an Array, so it must be read via
        // objVal + an explicit Array type check, not getObj().
        if (!item->objVal.count("widgets_values") || item->objVal.at("widgets_values")->type != JsonType::Array) continue;
        const auto& wArr = item->objVal.at("widgets_values")->arrVal;

        if (typeStr == "CLIPTextEncode") {
            std::string text;
            int64_t textLink;
            // The "text" widget may have been converted to a wired input
            // socket (prompt-enhancement/LoRA-trigger workflows commonly do
            // this) -- in that case widgets_values[0] is a stale leftover
            // from before the conversion, not the text actually used, so
            // prefer following the link over the raw widget value.
            if (GetNodeInputLink(item.get(), "text", textLink)) {
                text = ResolveUiTextThroughLink(textLink, linkIndex, 0);
            }
            if (text.empty() && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
                text = wArr[0]->strVal;
            }
            if (!text.empty()) {
                if (out.posPromptText.empty()) out.posPromptText = text;
                else if (out.negPromptText.empty()) out.negPromptText = text;
            }
        } else if ((typeStr == "Load Checkpoint" || typeStr.find("CheckpointLoader") != std::string::npos ||
                    typeStr.find("UNETLoader") != std::string::npos || typeStr.find("DiffusionModelLoader") != std::string::npos) &&
                   !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
            if (out.modelName.empty()) out.modelName = wArr[0]->strVal;
        } else if (typeStr == "VAELoader" && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
            if (out.vaeName.empty()) out.vaeName = wArr[0]->strVal;
        } else {
            // Standard fixed-position generation-parameter node types
            // (KSampler and its custom-sampler-pipeline siblings): looked up
            // in the table above instead of a long if/else-if chain.
            auto it = GetWidgetFieldMaps().find(typeStr);
            if (it != GetWidgetFieldMaps().end()) {
                ApplyWidgetFieldMap(it->second, wArr, info, out.samplerName, out.schedulerName);
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

        if (classType == "CLIPTextEncode" || classType == "BNK_CLIPTextEncodeAdvanced" || classType.find("Prompt") != std::string::npos) {
            std::string text = inputs->objVal.count("text") ? ResolveTextField(nodesObj, inputs->objVal.at("text").get()) : "";
            if (!text.empty()) {
                if (ui.posPromptText.empty()) ui.posPromptText = text;
                else if (ui.negPromptText.empty()) ui.negPromptText = text;
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
    std::string resolvedPos = ResolveClipText(nodesObj, positiveNodeId);
    std::string resolvedNeg = ResolveClipText(nodesObj, negativeNodeId);
    if (!resolvedPos.empty()) ui.posPromptText = resolvedPos;
    if (!resolvedNeg.empty()) ui.negPromptText = resolvedNeg;

    // UI-format node/link index, built up-front (before widget extraction) --
    // see UiLinkIndex/CollectUiNodesAndLinks above.
    UiLinkIndex linkIndex;
    CollectUiNodesAndLinks(nodesObj, linkIndex);
    if (const auto* uiDefs = nodesObj->getObj("definitions")) {
        if (uiDefs->objVal.count("subgraphs")) {
            const auto* uiSubgraphsVal = uiDefs->objVal.at("subgraphs").get();
            if (uiSubgraphsVal && uiSubgraphsVal->type == SimpleJson::JsonType::Array) {
                for (const auto& sg : uiSubgraphsVal->arrVal) CollectUiNodesAndLinks(sg.get(), linkIndex);
            } else if (uiSubgraphsVal && uiSubgraphsVal->type == SimpleJson::JsonType::Object) {
                for (const auto& kv3 : uiSubgraphsVal->objVal) CollectUiNodesAndLinks(kv3.second.get(), linkIndex);
            }
        }
    }

    // 2. UI Graph Format Traversal ("nodes" array), plus subgraph definitions
    // (definitions.subgraphs[].nodes / .subgraphs{}.nodes) -- ComfyUI's
    // subgraph feature moves the actual KSampler/CLIPTextEncode/etc. nodes
    // out of the top-level list into there, with the top level holding only
    // opaque subgraph-instance placeholder nodes.
    if (nodesObj->objVal.count("nodes") && nodesObj->objVal.at("nodes")->type == SimpleJson::JsonType::Array) {
        TraverseUiNodes(nodesObj->objVal.at("nodes")->arrVal, linkIndex, info, ui);
    }
    if (const auto* definitions = nodesObj->getObj("definitions")) {
        if (definitions->objVal.count("subgraphs")) {
            const auto* subgraphsVal = definitions->objVal.at("subgraphs").get();
            auto visitSubgraph = [&](const SimpleJson::JsonValue* sg) {
                if (!sg || sg->type != SimpleJson::JsonType::Object) return;
                if (sg->objVal.count("nodes") && sg->objVal.at("nodes")->type == SimpleJson::JsonType::Array) {
                    TraverseUiNodes(sg->objVal.at("nodes")->arrVal, linkIndex, info, ui);
                }
            };
            if (subgraphsVal && subgraphsVal->type == SimpleJson::JsonType::Array) {
                for (const auto& sg : subgraphsVal->arrVal) visitSubgraph(sg.get());
            } else if (subgraphsVal && subgraphsVal->type == SimpleJson::JsonType::Object) {
                for (const auto& kv2 : subgraphsVal->objVal) visitSubgraph(kv2.second.get());
            }
        }
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
// InvokeAI Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeInvokeAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "positive_prompt"/"negative_prompt" naming is distinctly InvokeAI;
    // without this gate any JSON object (including SwarmUI/NovelAI's own
    // metadata) would match here, since this decoder runs before them.
    if (!root->objVal.count("positive_prompt") && !root->objVal.count("negative_prompt")) return false;

    info.has_metadata = true;
    info.generator = L"InvokeAI";
    info.prompt = Utf8ToWstring(root->getStr("positive_prompt").empty() ? root->getStr("prompt") : root->getStr("positive_prompt"));
    info.negative_prompt = Utf8ToWstring(root->getStr("negative_prompt"));
    info.model = Utf8ToWstring(root->getStr("model"));
    ExtractSeedCfgSteps(root, "cfg_scale", info);
    info.sampler = Utf8ToWstring(root->getStr("scheduler"));
    info.full_parameters = Utf8ToWstring(originalText);
    return true;
}


// ---------------------------------------------------------------------------
// SwarmUI Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeSwarmUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "cfgscale"/"negativeprompt" (no separator) is SwarmUI's distinctive
    // naming, as opposed to A1111's "CFG scale" or InvokeAI/NovelAI's
    // "cfg_scale"/"scale" -- without this gate any JSON object would match.
    if (!root->objVal.count("cfgscale") && !root->objVal.count("negativeprompt")) return false;

    info.has_metadata = true;
    info.generator = L"SwarmUI";
    info.prompt = Utf8ToWstring(root->getStr("prompt"));
    info.negative_prompt = Utf8ToWstring(root->getStr("negativeprompt"));
    info.model = Utf8ToWstring(root->getStr("model"));
    ExtractSeedCfgSteps(root, "cfgscale", info);
    info.full_parameters = Utf8ToWstring(originalText);
    return true;
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
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "uc" (undesired content = negative prompt) is distinctly NovelAI naming
    // -- without this gate any JSON object would match here.
    if (!root->objVal.count("uc")) return false;

    info.has_metadata = true;
    info.generator = L"NovelAI";
    info.prompt = Utf8ToWstring(root->getStr("prompt"));
    info.negative_prompt = Utf8ToWstring(root->getStr("uc"));
    ExtractSeedCfgSteps(root, "scale", info);
    info.sampler = Utf8ToWstring(root->getStr("sampler"));

    info.size = MakeSizeString(root->getInt64("width"), root->getInt64("height"));

    info.full_parameters = Utf8ToWstring(originalText);
    return true;
}
