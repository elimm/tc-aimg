#include "aimg_decoder.h"
#include "aimg_decoder_internal.h"
#include "aimg_abort.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// JsonValue lives in aimg_decoder_internal.h because comfyui_decoder.cpp
// needs it too; JsonParser stays here, since only DecodeCore parses.

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
            // Bulk-append the whole unescaped run: most strings in these
            // files carry no escapes at all.
            size_t special = src.find_first_of("\"\\", pos);
            if (special == std::string::npos) {
                res.append(src, pos, src.size() - pos);
                pos = src.size();
                break;
            }
            if (special > pos) res.append(src, pos, special - pos);
            char c = src[special];
            pos = special + 1;
            if (c == '"') return res;
            if (pos < src.size()) {
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
                    uint32_t codepoint = (uint32_t)AImgDecoderInternal::ParseHexPrefix(hexStr.c_str());

                    // Combine with a following low surrogate into the full
                    // astral codepoint (e.g. an emoji).
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
                        pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                        std::string lowHex = src.substr(pos + 2, 4);
                        uint32_t low = (uint32_t)AImgDecoderInternal::ParseHexPrefix(lowHex.c_str());
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
                res += c; // trailing lone backslash at end of input
            }
        }
        return res;
    }

public:
    JsonParser(const std::string& json) : src(json) {}

    std::unique_ptr<JsonValue> parse() {
        skipWS();
        if (pos >= src.size()) return nullptr;
        if (depth >= kMaxDepth) return nullptr; // guard against stack overflow on deeply-nested/adversarial JSON

        auto val = std::make_unique<JsonValue>();
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
                if (child) val->objVal.emplace(std::move(key), std::move(child));
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
                if (child) val->arrVal.push_back(std::move(child));
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
            // Invalid JSON, but Python's json.dumps -- and so ComfyUI --
            // emits bare NaN/Infinity. Unhandled, parse() returns nullptr
            // having consumed NOTHING, and the enclosing loop then aborts on
            // the same byte, truncating the whole rest of the document.
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
            const char* numEnd;
            // A lone "-" has no digit; numVal stays 0 as strtod's result did.
            if (!AImgDecoderInternal::ParseDouble(src.c_str() + start, &numEnd, val->numVal)) val->numVal = 0.0;
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

namespace AImgDecoderInternal {
// The parsed tree owns copies of every string, so it safely outlives `text`
// even though JsonParser itself only holds a reference to it.
std::unique_ptr<SimpleJson::JsonValue> ParseJson(const std::string& text) {
    SimpleJson::JsonParser parser(text);
    return parser.parse();
}
} // namespace AImgDecoderInternal


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

// The trimmed range within str, without copying: a caller that only needs
// to know WHERE the text is can skip the substr entirely. Trim() is built on
// it so the NUL-inclusive whitespace set is defined in one place.
static void TrimRange(const std::string& str, size_t& first, size_t& count) {
    static const char kWhitespace[] = " \t\r\n\0"; // 5-char overload so the embedded NUL isn't dropped as a C-string terminator
    first = str.find_first_not_of(kWhitespace, 0, sizeof(kWhitespace));
    if (first == std::string::npos) { first = 0; count = 0; return; }
    size_t last = str.find_last_not_of(kWhitespace, std::string::npos, sizeof(kWhitespace));
    count = last - first + 1;
}

std::string AImgDecoder::Trim(const std::string& str) {
    size_t first, count;
    TrimRange(str, first, count);
    if (count == 0) return "";
    if (first == 0 && count == str.size()) return str;
    return str.substr(first, count);
}

// Gate for DecodeCore's last-resort "text in a comment field is a prompt"
// fallback. Non-AI tools write signatures and boilerplate into the same
// fields, so this needs both negative checks and positive prose evidence:
//  - a raw hex address literal is build/diagnostic text, never a prompt;
//  - JSON reaching this catch-all already failed every structural decoder,
//    so it is unrelated app data, not a prompt to guess at;
//  - fewer than a handful of comma/whitespace-separated tokens rejects
//    opaque watermark blobs and short tool boilerplate, while still
//    accepting real prompts (prose, or A1111-style comma-separated tags).
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

    size_t firstNonSpace = s.find_first_not_of(" \t\r\n");
    if (firstNonSpace != std::string::npos && (s[firstNonSpace] == '{' || s[firstNonSpace] == '[')) return false;

    // Encoder/editor signature shapes, written vendor-neutrally on purpose:
    // the same four forms come from many different libraries and editors.
    if (firstNonSpace != std::string::npos) {
        static const char* kSignaturePrefixes[] = {
            "CREATOR:",
            "File written by",
            "Generated by",
        };
        for (const char* prefix : kSignaturePrefixes) {
            size_t len = strlen(prefix);
            if (s.compare(firstNonSpace, len, prefix) == 0) return false;
        }
    }
    if (s.find("JPEG Library") != std::string::npos) return false;

    static const char kTokenSeparators[] = " \t\r\n,\0"; // NUL-inclusive overload, see Trim() above
    int tokenCount = 0;
    size_t pos = s.find_first_not_of(kTokenSeparators, 0, sizeof(kTokenSeparators));
    while (pos != std::string::npos) {
        tokenCount++;
        size_t next = s.find_first_of(kTokenSeparators, pos, sizeof(kTokenSeparators));
        pos = (next == std::string::npos) ? std::string::npos
                                           : s.find_first_not_of(kTokenSeparators, next, sizeof(kTokenSeparators));
    }
    if (tokenCount < 4) return false;

    return true;
}

// Shared by InvokeAI/SwarmUI/NovelAI: "seed"/"steps" are consistent across
// them, only the CFG key name differs. A free function so the equally
// free DecodeSimpleGraphGenerator can call it without class access.
namespace {
// A key's mere PRESENCE is not enough: getNum() returns 0.0 for
// null/array/object/string, so gating on find() alone writes a fabricated 0
// that has_* marks as real and MergeGaps can never correct. A string that
// parses ENTIRELY as a number is accepted; a partial one ("7 steps") is not.
bool JsonNumericValue(const SimpleJson::JsonValue* v, double& out) {
    if (!v) return false;
    if (v->type == SimpleJson::JsonType::Number) {
        out = v->numVal;
        return true;
    }
    if (v->type == SimpleJson::JsonType::String) {
        const char* s = v->strVal.c_str();
        const char* endptr = nullptr;
        double parsed;
        if (!AImgDecoderInternal::ParseDouble(s, &endptr, parsed)) return false; // no digits consumed at all
        while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') endptr++;
        if (*endptr != '\0') return false; // trailing junk ("7 steps"): not a pure numeric string
        out = parsed;
        return true;
    }
    return false;
}

void ExtractSeedCfgSteps(const SimpleJson::JsonValue* jsonObj, const std::string& cfgKey, AImgInfo& info) {
    double num;
    if (JsonNumericValue(jsonObj->find("seed"), num)) {
        info.seed = SimpleJson::ClampDoubleToInt64(num);
        info.has_seed = true;
    }
    if (JsonNumericValue(jsonObj->find(cfgKey), num)) {
        info.cfg_scale = num;
        info.has_cfg = true;
    }
    if (JsonNumericValue(jsonObj->find("steps"), num)) {
        info.steps = (int32_t)SimpleJson::ClampDoubleToInt64(num);
        info.has_steps = true;
    }
}

// ComfyUI's A1111-compat save nodes pack "Sampler: <sampler> <scheduler>"
// into one value with no "Schedule type:" key. Splits off a trailing known
// ComfyUI scheduler name (separator " " or "_"; an "_" inside the scheduler
// name also matches a space, e.g. "SGM Uniform"). Only ever called on a
// "Version: ComfyUI" block, so "DPM++ 2M Karras" is never split.
void SplitComfyUISamplerScheduler(std::wstring& sampler, std::wstring& scheduler) {
    static const wchar_t* kSchedulers[] = {
        L"simple", L"normal", L"karras", L"exponential", L"sgm_uniform",
        L"beta", L"ddim_uniform", L"linear_quadratic", L"kl_optimal"
    };
    for (const wchar_t* sched : kSchedulers) {
        size_t schedLen = wcslen(sched);
        if (sampler.size() <= schedLen) continue;
        size_t sp = sampler.size() - schedLen - 1;
        // "er_sde_simple" also occurs: no ComfyUI sampler name itself ends in
        // "_<scheduler>", and this only runs on a Version: ComfyUI block.
        if (sampler[sp] != L' ' && sampler[sp] != L'_') continue;
        bool match = true;
        for (size_t i = 0; i < schedLen; i++) {
            wchar_t c = sampler[sp + 1 + i];
            if (sched[i] == L'_' ? (c != L'_' && c != L' ') : towlower(c) != (wchar_t)sched[i]) { match = false; break; }
        }
        if (match) {
            scheduler = sampler.substr(sp + 1);
            std::wstring samplerPart = sampler.substr(0, sp);
            size_t last = samplerPart.find_last_not_of(L" \t\r\n");
            sampler = (last == std::wstring::npos) ? std::wstring() : samplerPart.substr(0, last + 1);
            return;
        }
    }
}
} // namespace


// ---------------------------------------------------------------------------
// Main AImgDecoder Entry Point
// ---------------------------------------------------------------------------

namespace {
// Length of the model-file extension ending range [b, e) of `s`, or 0. The
// range must be strictly longer than the extension, so a bare ".pt" stays.
// Shared by StripModelExtension and NormalizeLoraField so they cannot drift.
size_t ModelExtLen(const std::wstring& s, size_t b, size_t e) {
    static const wchar_t* kExts[] = { L".safetensors", L".ckpt", L".pt", L".pth", L".bin", L".gguf" };
    size_t n = e - b;
    for (const wchar_t* ext : kExts) {
        size_t extLen = wcslen(ext);
        if (n <= extLen) continue;
        size_t pos = e - extLen;
        bool match = true;
        for (size_t i = 0; i < extLen; i++) {
            if (towlower(s[pos + i]) != towlower((wchar_t)ext[i])) { match = false; break; }
        }
        if (match) return extLen;
    }
    return 0;
}
} // namespace

std::wstring AImgDecoder::StripModelExtension(const std::wstring& name) {
    return name.substr(0, name.size() - ModelExtLen(name, 0, name.size()));
}

std::wstring AImgDecoder::NormalizeLoraField(const std::wstring& raw) {
    if (raw.empty()) return raw;
    // LoRA arrives in several shapes per generator (quoted "name: hash"
    // lists, bare filenames with extensions). This is the one place they all
    // funnel through, so every file displays the same shape. Index ranges
    // over `raw`: one output allocation, not several per entry.
    const size_t n = raw.size();
    std::wstring result;
    result.reserve(n);
    for (size_t start = 0; start <= n;) {
        size_t comma = raw.find(L',', start);
        size_t b = start, e = (comma == std::wstring::npos) ? n : comma;
        while (b < e && (raw[b] == L' ' || raw[b] == L'\t')) b++;
        while (e > b && (raw[e - 1] == L' ' || raw[e - 1] == L'\t')) e--;
        if (b < e && raw[b] == L'"') b++;
        if (b < e && raw[e - 1] == L'"') e--;
        if (b < e) {
            // Bounded to this part: a raw.find() would rescan the rest of the
            // list for every colon-less entry.
            size_t colon = b;
            while (colon + 1 < e && !(raw[colon] == L':' && raw[colon + 1] == L' ')) colon++;
            bool hasColon = colon + 1 < e;
            size_t nameEnd = hasColon ? colon : e;
            nameEnd -= ModelExtLen(raw, b, nameEnd);
            if (!result.empty()) result += L", ";
            result.append(raw, b, nameEnd - b);
            if (hasColon) result.append(raw, colon, e - colon);
        }
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return result;
}

// SHA-256 of empty input: a save node that hashed no file at all writes (a
// prefix of) this as "Model hash", which names no model.
static bool IsEmptyInputSha256Prefix(const std::string& v) {
    static const char kEmptySha256[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const size_t n = v.size();
    if (n < 8 || n > sizeof(kEmptySha256) - 1) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = v[i];
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        if (c != kEmptySha256[i]) return false;
    }
    return true;
}

AImgInfo AImgDecoder::Decode(const RawImageMetadata& rawMeta) {
    AImgInfo info = DecodeCore(rawMeta);
    info.model = StripModelExtension(info.model);
    info.vae = StripModelExtension(info.vae);
    info.lora = NormalizeLoraField(info.lora);
    return info;
}

// Fills fields `dst` left empty from `src`, never overwriting.
// `full_parameters_utf8` is excluded on purpose: callers run the `src` parse
// with populateFullParameters=false, so it is empty by construction, and
// `dst`'s own raw text is the one to keep.
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
    if (!dst.has_denoising_strength && src.has_denoising_strength) { dst.denoising_strength = src.denoising_strength; dst.has_denoising_strength = true; }
    if (!dst.has_clip_skip && src.has_clip_skip) { dst.clip_skip = src.clip_skip; dst.has_clip_skip = true; }
    if (!dst.has_hires_steps && src.has_hires_steps) { dst.hires_steps = src.hires_steps; dst.has_hires_steps = true; }
    if (dst.hires_upscale.empty()) dst.hires_upscale = src.hires_upscale;
    if (dst.hires_upscaler.empty()) dst.hires_upscaler = src.hires_upscaler;
}

AImgInfo AImgDecoder::DecodeCore(const RawImageMetadata& rawMeta) {
    AImgInfo info;

    // JSON is parsed lazily and memoized per candidate (GetJson below):
    // most files match a decoder after one or two, so parsing every
    // JSON-shaped chunk up front pays for trees never used, and every later
    // decoder attempt reuses the same tree instead of re-parsing.
    //
    // key/raw point into rawMeta, which is never mutated here, so a chunk
    // that is already trimmed costs no copy. text() is a function rather
    // than a cached pointer because moving the Candidate (push_back, vector
    // growth) would dangle a pointer into a short `trimmed`'s SSO buffer.
    struct Candidate {
        const std::string* key = nullptr;
        const std::string* raw = nullptr;
        std::string trimmed;
        bool useTrimmed = false;
        std::unique_ptr<SimpleJson::JsonValue> json;
        bool jsonAttempted = false;
        const std::string& text() const { return useTrimmed ? trimmed : *raw; }
    };
    // ParseXMP also stores the raw XML/RDF wrapper under the plain "xmp"
    // key. A loose substring gate can find "Steps:"/"Sampler:" buried in a
    // description inside that markup and take the literal "<x:xmpmeta ...>"
    // header as the prompt. One predicate, applied at EVERY candidate loop
    // that runs A1111/Fooocus, so a new call site can't omit it.
    auto isRawXmpWrapper = [](const Candidate& cand) {
        return cand.key && *cand.key == "xmp";
    };

    // The companion gap-fill loop's loose A1111 gate can also match the graph
    // JSON itself (a note widget saying "Steps:" is enough), which MergeGaps
    // would then write whole into the prompt fields. Reject graph chunks by
    // key, or a leading '{' with no line-start "Steps:"/"Negative prompt:"
    // (serialized JSON never holds a raw newline in a string; a real prompt
    // can still be JSON-styled text). No getJson here: that is the very parse
    // this loop avoids.
    auto trimWs = [](const std::wstring& w) {
        size_t b = w.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos) return std::wstring();
        return w.substr(b, w.find_last_not_of(L" \t\r\n") - b + 1);
    };
    auto isJsonShapedCompanion = [](const Candidate& cand) {
        if (cand.key && (*cand.key == "prompt" || *cand.key == "workflow")) return true;
        const std::string& t = cand.text();
        return !t.empty() && t[0] == '{' &&
               t.find("\nSteps:") == std::string::npos &&
               t.find("\nNegative prompt:") == std::string::npos;
    };

    std::vector<Candidate> candidates;
    bool hasComfyUIChunk = false;
    // See the seed override below: a "prompt" chunk is authoritative, so
    // only its absence makes a "workflow"-derived seed suspect.
    bool hasPromptChunk = rawMeta.text_chunks.count("prompt") > 0;

    for (const auto& kv : rawMeta.text_chunks) {
        size_t first, count;
        TrimRange(kv.second, first, count);
        if (count > 0) {
            Candidate cand;
            cand.key = &kv.first;
            cand.raw = &kv.second;
            if (first == 0 && count == kv.second.size()) {
                cand.useTrimmed = false; // already trimmed: text() reads *raw directly, no copy
            } else {
                cand.trimmed = kv.second.substr(first, count);
                cand.useTrimmed = true;
            }
            candidates.push_back(std::move(cand));
        }
        // Matches chunk KEYS, never free-text prompt content -- a prompt can
        // legitimately contain "comfy" or "KSampler". The two value scans
        // therefore run only when the chunk plausibly is a graph ('{').
        if (kv.first == "prompt" || kv.first == "workflow" || kv.first.find("ComfyUI") != std::string::npos ||
            (count > 0 && kv.second[first] == '{' &&
             (kv.second.find("class_type") != std::string::npos || kv.second.find("KSampler") != std::string::npos))) {
            hasComfyUIChunk = true;
        }
    }

    if (candidates.empty()) return info;

    // Parses on first request and caches, null results included.
    auto getJson = [](Candidate& cand) -> SimpleJson::JsonValue* {
        if (!cand.jsonAttempted) {
            cand.jsonAttempted = true;
            // Some writers label the object, e.g. "Workflow:{...}". Parsing
            // from the first '{' keeps that from hiding a valid graph; on
            // genuine free text the parse just fails as before.
            size_t bracePos = cand.text().find('{');
            if (bracePos != std::string::npos) {
                // if/else, not a ternary: with cand.text() returning a
                // const reference, MSVC's conditional-operator lowering was
                // measured materializing that branch TWICE, doubling the very
                // copy this structure exists to avoid. This form costs exactly
                // one copy either way.
                std::string jsonSlice;
                if (bracePos == 0) jsonSlice = cand.text();
                else jsonSlice = cand.text().substr(bracePos);
                SimpleJson::JsonParser parser(jsonSlice);
                auto root = parser.parse();
                if (root && root->type == SimpleJson::JsonType::Object) {
                    cand.json = std::move(root);
                }
            }
        }
        return cand.json.get();
    };

    // 1. Try ComfyUI on any candidate that parsed as a JSON object, in
    // priority order: prompt, workflow, then the rest in map order. A
    // metadata-hub chunk embedding its own copy of the workflow sorts before
    // "prompt" and would otherwise win with stale UI-format values. Only this
    // loop's order changes; every other candidate loop keeps map order.
    std::vector<Candidate*> comfyOrder;
    Candidate* promptCand = nullptr;
    Candidate* workflowCand = nullptr;
    for (auto& cand : candidates) {
        if (!promptCand && cand.key && *cand.key == "prompt") promptCand = &cand;
        else if (!workflowCand && cand.key && *cand.key == "workflow") workflowCand = &cand;
    }
    if (promptCand) comfyOrder.push_back(promptCand);
    if (workflowCand) comfyOrder.push_back(workflowCand);
    for (auto& cand : candidates) {
        if (&cand != promptCand && &cand != workflowCand) comfyOrder.push_back(&cand);
    }
    for (auto* candPtr : comfyOrder) {
        auto& cand = *candPtr;
        SimpleJson::JsonValue* json = getJson(cand);
        bool negZeroed = false;
        if (json && DecodeComfyUI(json, cand.text(), info, &negZeroed)) {
            // Some metadata-saving nodes embed a flattened A1111-style text
            // block alongside the graph, carrying fields no node holds. Fill
            // only the gaps; the graph's own values are more precise.
            for (const auto& cand2 : candidates) {
                if (&cand2 == &cand) continue; // see isJsonShapedCompanion's comment above
                if (isRawXmpWrapper(cand2)) continue; // see isRawXmpWrapper's comment above
                if (isJsonShapedCompanion(cand2)) continue;
                AImgInfo fallback;
                if (!DecodeAutomatic1111(cand2.text(), fallback, /*populateFullParameters=*/false)) continue;
                // "workflow" is a live UI snapshot, not an execution record:
                // control_after_generate advances the displayed seed the
                // instant a run finishes, so the saved value can already be
                // the NEXT run's. With no authoritative "prompt" chunk, the
                // companion text's seed OVERRIDES rather than fills a gap.
                if (!hasPromptChunk && fallback.has_seed) {
                    info.seed = fallback.seed;
                    info.has_seed = true;
                }
                // Some save nodes write the positive prompt into the negative
                // slot too (graph negative zeroed out), or leave the prompt
                // line empty and put the positive under "Negative prompt:". A
                // negative equal to either prompt is that artifact: drop it.
                std::wstring trimmedFallbackNeg = trimWs(fallback.negative_prompt);
                if (!trimmedFallbackNeg.empty() &&
                    (trimmedFallbackNeg == trimWs(fallback.prompt) || trimmedFallbackNeg == trimWs(info.prompt))) {
                    fallback.negative_prompt.clear();
                }
                // A zeroed or structurally absent graph negative (a guider
                // with no negative input at all) is deliberately blank, so
                // whatever the companion says ("-", "unknown" placeholders, or
                // the zeroed encoder's own text) must not fill it.
                if (negZeroed) fallback.negative_prompt.clear();
                MergeGaps(info, fallback);
                break;
            }
            return info;
        }
    }

    // 1b. Draw Things next, for the same reason as Fooocus below: it writes
    // BOTH a JSON payload and a duplicate human-readable block that satisfies
    // the loose A1111 check -- but under different key names, so that match
    // would drop CFG/LoRA/denoise, which only the JSON carries.
    for (auto& cand : candidates) {
        SimpleJson::JsonValue* json = getJson(cand);
        if (json && DecodeDrawThings(json, cand.text(), info)) {
            return info;
        }
    }

    // 2. Fooocus before A1111: it emits an A1111-style block, so the generic
    // check below would always claim it first.
    for (const auto& cand : candidates) {
        if (isRawXmpWrapper(cand)) continue; // see isRawXmpWrapper's comment above
        if (DecodeFooocus(cand.text(), info)) {
            return info;
        }
    }

    // 3. Try Automatic1111 / SD.Next / Forge on candidates.
    for (const auto& cand : candidates) {
        if (isRawXmpWrapper(cand)) continue;
        if (DecodeAutomatic1111(cand.text(), info)) {
            // Relabel when the file also carries a ComfyUI chunk/structural
            // marker (keys and graph shape only, never prompt text).
            if (hasComfyUIChunk) {
                info.generator = L"ComfyUI";
            }
            return info;
        }
    }

    // 4-8. One generator-major loop over five same-signature decoders. The
    // ORDER matters: Easy Diffusion and WanGP must both precede InvokeAI,
    // whose gate their own "negative_prompt" key would otherwise satisfy.
    using JsonGeneratorDecodeFn = bool (*)(const SimpleJson::JsonValue*, const std::string&, AImgInfo&);
    static const JsonGeneratorDecodeFn kJsonGeneratorDecoders[] = {
        DecodeEasyDiffusion, DecodeWanGP, DecodeInvokeAI, DecodeSwarmUI, DecodeNovelAI
    };
    for (auto decodeFn : kJsonGeneratorDecoders) {
        for (auto& cand : candidates) {
            SimpleJson::JsonValue* json = getJson(cand);
            if (json && decodeFn(json, cand.text(), info)) {
                return info;
            }
        }
    }

    // 8. Fallback: ONLY exif:UserComment and "parameters" may be read as a
    // freeform prompt. ImageDescription/Comment/xmp:* are excluded because
    // they are the standard caption and software fields -- a photo caption is
    // prose and no text heuristic separates it from a prompt. Accepted cost:
    // a generator writing a bare prompt into one of those is not detected,
    // traded for not reporting ordinary photographs as AI-generated.
    for (const auto& cand : candidates) {
        if ((*cand.key == "exif:UserComment" || *cand.key == "parameters") &&
            LooksLikeGenerationText(cand.text())) {
            info.has_metadata = true;
            info.generator = hasComfyUIChunk ? L"ComfyUI" : L"AI Image";
            info.prompt = Utf8ToWstring(cand.text());
            info.full_parameters_utf8 = cand.text();
            return info;
        }
    }

    return info;
}


// ---------------------------------------------------------------------------
// Automatic1111 Decoder
// ---------------------------------------------------------------------------

namespace {
// LoRA name handling works on UTF-8 byte spans: names only ever need ASCII
// case folding (non-ASCII bytes compare exactly), and the whole list is
// converted to UTF-16 once at the end.
struct LoraSpan { const char* b; const char* e; };

char LoraLow(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool LoraCiEq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) if (LoraLow(a[i]) != LoraLow(b[i])) return false;
    return true;
}

LoraSpan LoraTrim(LoraSpan s, bool quotes) {
    auto skip = [quotes](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || (quotes && c == '"'); };
    while (s.b < s.e && skip(*s.b)) s.b++;
    while (s.e > s.b && skip(s.e[-1])) s.e--;
    return s;
}

// Same extension list as StripModelExtension, ASCII case-insensitive.
LoraSpan LoraNoExt(LoraSpan s) {
    static const char* const kExts[] = { ".safetensors", ".ckpt", ".pt", ".pth", ".bin", ".gguf" };
    size_t n = (size_t)(s.e - s.b);
    for (const char* ext : kExts) {
        size_t el = strlen(ext);
        if (n > el && LoraCiEq(s.e - el, ext, el)) { s.e -= el; break; }
    }
    return s;
}

LoraSpan LoraBase(LoraSpan s) {
    for (const char* p = s.e; p > s.b; p--) if (p[-1] == '\\' ||p[-1] == '/') return LoraSpan{p, s.e};
    return s;
}

bool LoraEq(LoraSpan a, LoraSpan b) {
    return (a.e - a.b) == (b.e - b.b) && LoraCiEq(a.b, b.b, (size_t)(a.e - a.b));
}

// Dedupe key per listed name, computed once: extension stripped, plus the
// basename of that. Spans point into the caller's prompt/hashNames, which
// outlive the call (never into `out`, which reallocates).
// Each span also carries a case-folded hash, so the quadratic dedupe compares
// characters only on a hash hit.
struct LoraKey { LoraSpan noExt, base; uint32_t hNoExt, hBase; };

uint32_t LoraHash(LoraSpan s) {
    uint32_t h = 2166136261u;
    for (const char* p = s.b; p < s.e; p++) h = (h ^ (uint8_t)LoraLow(*p)) * 16777619u;
    return h;
}

LoraKey LoraMakeKey(LoraSpan name) {
    LoraKey k;
    k.noExt = LoraNoExt(name);
    k.base = LoraBase(k.noExt);
    k.hNoExt = LoraHash(k.noExt);
    k.hBase = LoraHash(k.base);
    return k;
}

// Same LoRA when equal after extension stripping, or when one equals the
// other's basename: prompt tags and hash lists disagree on whether a folder
// prefix is written.
bool LoraSameKey(const LoraKey& a, const LoraKey& b) {
    return (a.hNoExt == b.hNoExt && LoraEq(a.noExt, b.noExt)) ||
           (a.hBase == b.hNoExt && LoraEq(a.base, b.noExt)) ||
           (a.hNoExt == b.hBase && LoraEq(a.noExt, b.base));
}

// Appends the name to `out` (", "-separated) and its key to `keys` unless an
// already-listed name matches. Returns whether it was new. Keys are computed
// once per name so a long list does not re-scan every earlier name's
// extension and basename on each comparison.
bool LoraAddName(std::string& out, std::vector<LoraKey>& keys, LoraSpan name) {
    LoraKey n = LoraMakeKey(name);
    for (const LoraKey& k : keys) if (LoraSameKey(k, n)) return false;
    keys.push_back(n);
    if (!out.empty()) out += ", ";
    out.append(name.b, name.e);
    return true;
}

// "name: hash, name2: hash2" (whole value and/or each entry may be quoted)
// -> one name per line appended to `out`. Cut at the LAST ": " so the hash is
// always removed.
void LoraNamesFromHashList(const std::string& list, std::string& out) {
    const char* end = list.data() + list.size();
    for (const char* p = list.data(); p <= end;) {
        const char* comma = (const char*)memchr(p, ',', (size_t)(end - p));
        LoraSpan part = LoraTrim(LoraSpan{p, comma ? comma : end}, true);
        for (const char* q = part.e; q - 2 >= part.b; q--) {
            if (q[-2] == ':' && q[-1] == ' ') { part.e = q - 2; break; }
        }
        part = LoraTrim(part, false);
        if (part.b < part.e) { out.append(part.b, part.e); out += '\n'; }
        if (!comma) break;
        p = comma + 1;
    }
}

// "Lora hashes" carries name: hash, nothing about strength; weights live in
// the prompt's <lora:NAME:WEIGHT> tags. Returns "NAME: WEIGHT" per tag, then
// the hash-list names no tag covered. Empty means leave info.lora alone (e.g.
// a verbatim "Loras" value and no tags).

std::string BuildLoraList(const std::string& prompt, const std::string& hashNames, bool lorasKeyPresent) {
    std::string out;
    std::vector<LoraKey> keys;
    const char* end = prompt.data() + prompt.size();
    for (const char* p = prompt.data(); (p = (const char*)memchr(p, '<', (size_t)(end - p))) != nullptr;) {
        if (end - p < 6 || !(LoraCiEq(p + 1, "lora:", 5) || LoraCiEq(p + 1, "lyco:", 5))) { p++; continue; }
        const char* bs = p + 6;
        const char* close = (const char*)memchr(bs, '>', (size_t)(end - bs));
        if (!close) break;
        p = close + 1;
        const char* c1 = (const char*)memchr(bs, ':', (size_t)(close - bs));
        LoraSpan name = LoraTrim(LoraSpan{bs, c1 ? c1 : close}, false);
        if (name.b == name.e || !LoraAddName(out, keys, name)) continue;
        if (!c1) continue;
        const char* c2 = (const char*)memchr(c1 + 1, ':', (size_t)(close - c1 - 1));
        LoraSpan w = LoraTrim(LoraSpan{c1 + 1, c2 ? c2 : close}, false);
        if (w.b == w.e) continue;
        std::string ws(w.b, w.e);
        const char* pe = nullptr;
        double v = 0;
        out += ": ";
        if (AImgDecoderInternal::ParseDouble(ws.c_str(), &pe, v) && *pe == '\0' && std::fabs(v) < 1e15) {
            // Six places, not FormatCompactNumber's two: 0.125 must not show
            // as 0.13. Trailing zeros (and a bare dot) are trimmed.
            ws = AImgDecoderInternal::FormatFixed(v, 6);
            size_t dot = ws.find('.');
            if (dot != std::string::npos) {
                size_t last = ws.find_last_not_of('0');
                if (last == dot) last--;
                ws.erase(last + 1);
            }
        }
        out += ws;
    }
    if (out.empty() && lorasKeyPresent) return out;
    for (const char* p = hashNames.data(), *he = p + hashNames.size(); p < he;) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(he - p));
        if (nl != p) LoraAddName(out, keys, LoraSpan{p, nl});
        p = nl + 1;
    }
    return out;
}
} // namespace

namespace {
// Next top-level ", " at or after `from`, skipping any inside a double-quoted
// span. A backslash inside a quoted span escapes the next character, so an
// escaped quote (a value like "x \", y") does not close the span.
size_t FindTopLevelComma(const char* s, size_t n, size_t from) {
    bool inQuotes = false;
    for (size_t i = from; i < n; i++) {
        if (s[i] == '"') inQuotes = !inQuotes;
        else if (inQuotes && s[i] == '\\') i++;
        else if (!inQuotes && s[i] == ',' && i + 1 < n && s[i + 1] == ' ') return i;
    }
    return std::string::npos;
}

// True when [b, e) looks like A1111's parameters line: at least three
// top-level "key: value" segments with plain keys. A1111 itself finds the line
// as the last line of the text, so a prompt that merely mentions "Steps:" can
// never be mistaken for it.
bool LooksLikeParamsLine(const std::string& t, size_t b, size_t e) {
    int segments = 0;
    size_t start = b;
    while (start < e) {
        size_t comma = FindTopLevelComma(t.c_str(), e, start);
        size_t segEnd = comma == std::string::npos ? e : comma;
        size_t colon = t.find(':', start);
        if (colon < segEnd && (colon + 1 == segEnd || t[colon + 1] == ' ')) {
            size_t kb = start, ke = colon;
            while (kb < ke && t[kb] == ' ') kb++;
            while (ke > kb && t[ke - 1] == ' ') ke--;
            bool ok = ke > kb;
            for (size_t i = kb; ok && i < ke; i++) {
                char c = t[i];
                ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '_' || c == ' ' || c == '/' || c == '-';
            }
            if (ok) segments++;
        }
        if (comma == std::string::npos) break;
        start = comma + 2;
    }
    return segments >= 3;
}
} // namespace

bool AImgDecoder::DecodeAutomatic1111(const std::string& paramText, AImgInfo& info, bool populateFullParameters) {
    if (paramText.empty()) return false;

    if (paramText.find("Steps:") == std::string::npos &&
        paramText.find("Negative prompt:") == std::string::npos &&
        paramText.find("Sampler:") == std::string::npos) {
        return false;
    }

    info.has_metadata = true;
    info.generator = L"Automatic1111";
    // Set only by this block's own "Version: ComfyUI" key, not by any other
    // ComfyUI relabel: only that key licenses the sampler/scheduler split.
    bool isComfyUIVersionBlock = false;
    // The gap-fill caller never reads full_parameters_utf8 off its result,
    // so don't copy the whole companion text block just to discard it.
    if (populateFullParameters) info.full_parameters_utf8 = paramText;

    // A1111 takes the last non-empty line as the parameters line and only
    // honours "Negative prompt:" at a line start; a plain find() split a prompt
    // that mentions either label mid-text, and put a "Seed: N, ..." line that
    // precedes "Steps:" into the negative prompt.
    size_t negPos = std::string::npos;
    for (size_t p = paramText.find("Negative prompt:"); p != std::string::npos; p = paramText.find("Negative prompt:", p + 1)) {
        if (p == 0 || paramText[p - 1] == '\n') { negPos = p; break; }
    }
    size_t stepsPos = std::string::npos;
    bool paramsLineFound = false;
    {
        size_t e = paramText.size();
        while (e > 0 && (paramText[e - 1] == ' ' || paramText[e - 1] == '\t' || paramText[e - 1] == '\r' || paramText[e - 1] == '\n')) e--;
        size_t nl = e ? paramText.rfind('\n', e - 1) : std::string::npos;
        if (nl != std::string::npos) {
            size_t ls = nl + 1;
            while (ls < e && (paramText[ls] == ' ' || paramText[ls] == '\t' || paramText[ls] == '\r')) ls++;
            if (ls < e && LooksLikeParamsLine(paramText, ls, e)) {
                stepsPos = ls;
                paramsLineFound = true;
                if (negPos != std::string::npos && negPos >= stepsPos) negPos = std::string::npos;
            }
        }
    }
    if (!paramsLineFound) {
        stepsPos = paramText.find("Steps:");
        if (stepsPos == std::string::npos) stepsPos = paramText.find("Sampler:");
    }

    std::string posPrompt, negPrompt, paramsLine;

    if (negPos != std::string::npos) {
        posPrompt = paramText.substr(0, negPos);
        if (stepsPos != std::string::npos && stepsPos > negPos + 16) {
            negPrompt = paramText.substr(negPos + 16, stepsPos - (negPos + 16));
            paramsLine = paramText.substr(stepsPos);
        } else if (stepsPos != std::string::npos) {
            // Crafted input where "Steps:" falls inside the "Negative
            // prompt:" label itself: nothing between them to extract.
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
    // Some writers give "Hashes" a raw JSON object value rather than the
    // quoted comma-separated sub-list "Lora hashes" uses. Its quote pairs are
    // balanced per entry, so the quote-aware splitter below would treat every
    // internal comma as a top-level split and drop each LORA entry -- lift
    // the whole {...} span out by brace matching first.
    std::string hashesJson;
    {
        size_t hPos = paramsLine.find("Hashes: {");
        if (hPos != std::string::npos) {
            size_t braceStart = hPos + 8;
            int depth = 0;
            size_t i = braceStart;
            for (; i < paramsLine.size(); i++) {
                if (paramsLine[i] == '{') depth++;
                else if (paramsLine[i] == '}') { depth--; if (depth == 0) { i++; break; } }
            }
            if (depth == 0) hashesJson = paramsLine.substr(braceStart, i - braceStart);
        }
    }
    // Forge sometimes drops "VAE:" and logs auxiliary components as an
    // unordered "Module N:" list with no fixed VAE index. Remember the first;
    // used only as a fallback, so a real "VAE:" key always wins.
    std::wstring moduleVaeCandidate;
    std::string loraHashNames; // one per line
    if (!paramsLine.empty()) {
        // "Lora hashes" is itself a quoted comma-separated sub-list, so a
        // plain find(", ") would split inside the quotes and truncate it.
        auto findNextComma = [&](size_t from) -> size_t {
            return FindTopLevelComma(paramsLine.c_str(), paramsLine.size(), from);
        };
        // Compares the key span against a literal without materializing a
        // std::string per key: this runs over 15+ keys on every A1111 file.
        auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        size_t start = 0;
        while (start < paramsLine.size()) {
            size_t nextComma = findNextComma(start);
            if (nextComma == std::string::npos) nextComma = paramsLine.size();

            size_t colon = paramsLine.find(": ", start);
            if (colon != std::string::npos && colon < nextComma) {
                size_t kStart = start, kEnd = colon;
                while (kStart < kEnd && isWs(paramsLine[kStart])) kStart++;
                while (kEnd > kStart && isWs(paramsLine[kEnd - 1])) kEnd--;
                size_t kLen = kEnd - kStart;
                auto keyIs = [&](const char* lit) {
                    size_t litLen = strlen(lit);
                    return kLen == litLen && paramsLine.compare(kStart, kLen, lit, litLen) == 0;
                };
                auto keyStartsWith = [&](const char* lit) {
                    size_t litLen = strlen(lit);
                    return kLen >= litLen && paramsLine.compare(kStart, litLen, lit, litLen) == 0;
                };

                size_t vStart = colon + 2, vEnd = nextComma;
                while (vStart < vEnd && isWs(paramsLine[vStart])) vStart++;
                while (vEnd > vStart && isWs(paramsLine[vEnd - 1])) vEnd--;
                std::string v = paramsLine.substr(vStart, vEnd - vStart);

                if (keyIs("Steps")) {
                    info.steps = AImgDecoderInternal::ParseLeadingInt(v.c_str());
                    info.has_steps = true;
                } else if (keyIs("Sampler")) {
                    info.sampler = Utf8ToWstring(v);
                } else if (keyIs("Schedule type") || keyIs("Scheduler")) {
                    info.scheduler = Utf8ToWstring(v);
                } else if (keyIs("CFG scale")) {
                    info.cfg_scale = AImgDecoderInternal::ParseLeadingDouble(v.c_str());
                    info.has_cfg = true;
                } else if (keyIs("Seed")) {
                    info.seed = AImgDecoderInternal::ParseLeadingUInt64(v.c_str());
                    info.has_seed = true;
                } else if (keyIs("Size")) {
                    info.size = Utf8ToWstring(v);
                } else if (keyIs("Model")) {
                    info.model = Utf8ToWstring(v);
                } else if (keyIs("Model hash")) {
                    if (!IsEmptyInputSha256Prefix(v)) info.model_hash = Utf8ToWstring(v);
                } else if (keyIs("Clip skip")) {
                    info.clip_skip = AImgDecoderInternal::ParseLeadingInt(v.c_str());
                    info.has_clip_skip = true;
                } else if (keyIs("Denoising strength")) {
                    info.denoising_strength = AImgDecoderInternal::ParseLeadingDouble(v.c_str());
                    info.has_denoising_strength = true;
                } else if (keyIs("Hires upscale")) {
                    info.hires_upscale = Utf8ToWstring(v);
                } else if (keyIs("Hires upscaler")) {
                    info.hires_upscaler = Utf8ToWstring(v);
                } else if (keyIs("Hires steps")) {
                    info.hires_steps = AImgDecoderInternal::ParseLeadingInt(v.c_str());
                    info.has_hires_steps = true;
                } else if (keyIs("VAE")) {
                    info.vae = Utf8ToWstring(v);
                } else if (keyIs("Lora hashes")) {
                    LoraNamesFromHashList(v, loraHashNames);
                } else if (keyIs("Loras")) {
                    info.lora = Utf8ToWstring(v);
                } else if (keyIs("Civitai resources")) {
                    civitaiResources = v;
                } else if (moduleVaeCandidate.empty() && keyStartsWith("Module ")) {
                    moduleVaeCandidate = Utf8ToWstring(v);
                } else if (keyIs("Version")) {
                    // Forge's "Version:" is distinct from stock A1111's
                    // plain "v<ver>": classic Forge is "f<digit>...", neo is
                    // "neo"/"neo-<ver>". Unambiguous enough to relabel on.
                    if ((v.size() > 1 && v[0] == 'f' && isdigit((unsigned char)v[1])) ||
                        v.rfind("neo", 0) == 0) {
                        info.generator = L"Forge";
                    } else if (v.size() == 7 &&
                               std::equal(v.begin(), v.end(), "ComfyUI",
                                          [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); })) {
                        // ComfyUI save nodes writing A1111-style text name
                        // themselves here; trust it over the generic label.
                        info.generator = L"ComfyUI";
                        isComfyUIVersionBlock = true;
                    }
                }
            }
            start = nextComma + 2;
        }
    }
    // Only split when no explicit scheduler key was present.
    if (isComfyUIVersionBlock && info.scheduler.empty()) {
        SplitComfyUISamplerScheduler(info.sampler, info.scheduler);
    }
    // Same duplicated-negative artifact as the companion drop in DecodeCore:
    // a negative identical to the prompt is not a real negative.
    if (isComfyUIVersionBlock && !info.negative_prompt.empty()) {
        static const wchar_t* kWs = L" \t\r\n";
        const std::wstring& n = info.negative_prompt;
        const std::wstring& p = info.prompt;
        size_t nb = n.find_first_not_of(kWs);
        size_t pb = p.find_first_not_of(kWs);
        if (nb != std::wstring::npos && pb != std::wstring::npos) {
            size_t nl = n.find_last_not_of(kWs) - nb + 1;
            size_t pl = p.find_last_not_of(kWs) - pb + 1;
            if (nl == pl && n.compare(nb, nl, p, pb, pl) == 0) {
                info.negative_prompt.clear();
            }
        }
    }
    if (info.vae.empty() && !moduleVaeCandidate.empty()) {
        info.vae = moduleVaeCandidate;
    }

    // Only when "Lora hashes"/"Loras" left nothing; those are the real
    // A1111 convention and win when present.
    if (info.lora.empty() && loraHashNames.empty() && !hashesJson.empty()) {
        SimpleJson::JsonParser parser(hashesJson);
        auto root = parser.parse();
        if (root && root->type == SimpleJson::JsonType::Object) {
            for (const auto& kv : root->objVal) {
                if (kv.first.rfind("LORA:", 0) != 0) continue;
                loraHashNames.append(kv.first, 5, std::string::npos);
                loraHashNames += '\n';
            }
        }
    }

    // Weights come from the prompt's tags; see BuildLoraList. A JSON-shaped
    // "prompt" (a graph chunk reaching here as companion text) is not
    // scanned: its <lora:> strings sit in arbitrary widgets.
    size_t firstNonWs = posPrompt.find_first_not_of(" \t\r\n");
    bool promptIsJson = firstNonWs != std::string::npos && (posPrompt[firstNonWs] == '{' || posPrompt[firstNonWs] == '[');
    std::string loraList = BuildLoraList(promptIsJson ? std::string() : posPrompt, loraHashNames, !info.lora.empty());
    if (!loraList.empty()) info.lora = Utf8ToWstring(loraList);

    // Some online generators omit "Model:" entirely and log a resources
    // array of {type, modelVersionId, modelName} instead; recover the
    // checkpoint entry's modelName when the plain key is missing.
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
// (The ComfyUI decoder lives in comfyui_decoder.cpp.)
// ---------------------------------------------------------------------------

namespace {
// The "WxH" convention aimg_derived.h's ParseSize reads back. Empty when
// either dimension is non-positive.
std::wstring MakeSizeString(int64_t w, int64_t h) {
    if (w <= 0 || h <= 0) return L"";
    return std::to_wstring(w) + L"x" + std::to_wstring(h); // digits + 'x' are pure ASCII, no UTF-8 decoding needed
}
} // namespace



// ---------------------------------------------------------------------------
// Easy Diffusion Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeEasyDiffusion(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "use_*_model" is Easy Diffusion's own naming. Must run before
    // InvokeAI, whose gate this JSON's "negative_prompt" would satisfy.
    if (!root->objVal.count("use_stable_diffusion_model")) return false;

    info.has_metadata = true;
    info.generator = L"Easy Diffusion";
    info.prompt = Utf8ToWstring(root->getStr("prompt"));
    info.negative_prompt = Utf8ToWstring(root->getStr("negative_prompt"));
    info.model = Utf8ToWstring(root->getStr("use_stable_diffusion_model"));
    info.vae = Utf8ToWstring(root->getStr("use_vae_model"));
    info.lora = Utf8ToWstring(root->getStr("use_lora_model"));
    info.sampler = Utf8ToWstring(root->getStr("sampler_name"));

    // JsonNumericValue, not a bare find(): presence alone is not proof of a
    // number. See its definition above ExtractSeedCfgSteps.
    double num;
    if (JsonNumericValue(root->find("seed"), num)) {
        info.seed = SimpleJson::ClampDoubleToInt64(num);
        info.has_seed = true;
    }
    if (JsonNumericValue(root->find("num_inference_steps"), num)) {
        info.steps = (int32_t)SimpleJson::ClampDoubleToInt64(num);
        info.has_steps = true;
    }
    if (JsonNumericValue(root->find("guidance_scale"), num)) {
        info.cfg_scale = num;
        info.has_cfg = true;
    }
    // "prompt_strength" (img2img only) is A1111's "Denoising strength".
    if (JsonNumericValue(root->find("prompt_strength"), num)) {
        info.denoising_strength = num;
        info.has_denoising_strength = true;
    }

    info.size = MakeSizeString(root->getInt64("width"), root->getInt64("height"));

    info.full_parameters_utf8 = originalText;
    return true;
}


// ---------------------------------------------------------------------------
// Draw Things Decoder
// ---------------------------------------------------------------------------

// Declared in aimg_decoder_internal.h, defined here next to its other
// caller: comfyui_decoder.cpp formats its LoRA weights with the same helper.
namespace AImgDecoderInternal {
std::string FormatFixed(double v, int prec) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    bool neg = (bits >> 63) != 0;
    int expBits = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & ((1ULL << 52) - 1);
    if (expBits == 0x7FF) return frac ? std::string("nan") : std::string(neg ? "-inf" : "inf");
    if (prec < 0) prec = 0;
    if (prec > 6) prec = 6;
    uint64_t pow10 = 1;
    for (int i = 0; i < prec; i++) pow10 *= 10;

    // |v| = m * 2^e exactly. Splitting it into integer part I and a fraction
    // fracNum / 2^k keeps every intermediate within 128 bits (fracNum < 2^53
    // and 10^prec < 2^20), which is what lets the rounding decision be exact
    // instead of depending on v * 10^prec being representable.
    uint64_t m = expBits ? (frac | (1ULL << 52)) : frac;
    int e = (expBits ? expBits : 1) - 1075;
    uint64_t intPart = 0, fracQ = 0;
    if (e >= 0) {
        intPart = e > 10 ? 0x7FFFFFFFFFFFFFFFULL : (m << e);
    } else {
        int k = -e;
        uint64_t fracNum;
        if (k >= 53) {
            fracNum = m;
        } else {
            intPart = m >> k;
            fracNum = m & ((1ULL << k) - 1);
        }
        // P = fracNum * 10^prec as a 128-bit value (hi:lo), built from two
        // 32-bit halves so no 64x64->128 intrinsic is needed (x86 has none).
        uint64_t a = (fracNum & 0xFFFFFFFFULL) * pow10;
        uint64_t b = (fracNum >> 32) * pow10;
        uint64_t lo = a + (b << 32);
        uint64_t hi = (b >> 32) + (lo < a ? 1 : 0);
        // P < 2^73, so for k >= 74 the fraction is below one half unit and
        // the quotient is 0 (round down); only k <= 73 needs real shifting.
        if (k <= 73 && (hi | lo) != 0) {
            uint64_t remHi, remLo, halfHi, halfLo;
            if (k < 64) {
                fracQ = (lo >> k) | (hi << (64 - k));
                remHi = 0;
                remLo = lo & ((1ULL << k) - 1);
            } else {
                fracQ = hi >> (k - 64);
                remHi = (k == 64) ? 0 : (hi & ((1ULL << (k - 64)) - 1));
                remLo = lo;
            }
            if (k - 1 < 64) { halfHi = 0; halfLo = 1ULL << (k - 1); }
            else { halfHi = 1ULL << (k - 1 - 64); halfLo = 0; }
            bool greater = remHi > halfHi || (remHi == halfHi && remLo > halfLo);
            bool tie = remHi == halfHi && remLo == halfLo;
            // Exact ties round half to even on the last printed digit, as the
            // UCRT's printf does.

            if (greater || (tie && (((prec > 0 ? fracQ : intPart) & 1) != 0))) fracQ++;
        }
        if (fracQ >= pow10) { fracQ -= pow10; intPart++; }
    }

    std::string out;
    if (neg) out += '-';
    out += std::to_string(intPart);
    if (prec > 0) {
        out += '.';
        std::string f = std::to_string(fracQ);
        out.append((size_t)prec - f.size(), '0');
        out += f;
    }
    return out;
}

bool ParseDouble(const char* s, const char** end, double& out) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') p++;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    // At most 19 significant digits fit a uint64; later integer digits only
    // shift the exponent and later fraction digits are dropped.
    uint64_t mant = 0;
    int sigDigits = 0, exp10 = 0, digitCount = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        digitCount++;
        if (sigDigits < 19) {
            mant = mant * 10 + (uint64_t)(*p - '0');
            if (mant) sigDigits++;
        } else {
            exp10++;
        }
    }
    if (*p == '.') {
        const char* q = p + 1;
        for (; *q >= '0' && *q <= '9'; q++) {
            digitCount++;
            if (sigDigits < 19) {
                mant = mant * 10 + (uint64_t)(*q - '0');
                if (mant) sigDigits++;
                exp10--;
            }
        }
        // "5." is a number, "." alone is not: the dot is consumed only when a
        // digit exists on either side, same as strtod.
        if (digitCount > 0) p = q;
    }
    if (digitCount == 0) { *end = s; out = 0.0; return false; }

    if (*p == 'e' || *p == 'E') {
        const char* q = p + 1;
        bool eneg = false;
        if (*q == '+' || *q == '-') { eneg = (*q == '-'); q++; }
        if (*q >= '0' && *q <= '9') {
            int ev = 0;
            for (; *q >= '0' && *q <= '9'; q++) {
                if (ev < 100000) ev = ev * 10 + (*q - '0');
            }
            exp10 += eneg ? -ev : ev;
            p = q;
        }
    }
    *end = p;

    double d = (double)mant;
    if (mant != 0 && exp10 != 0) {
        static const double kPow10[] = {
            1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
            1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
        };
        // Both the mantissa (< 2^53) and the power of ten (<= 1e22) are exact
        // doubles here, so one multiply/divide is correctly rounded - the fast
        // path that makes ordinary values bit-identical to strtod.
        int ae = exp10 < 0 ? -exp10 : exp10;
        if (mant < (1ULL << 53) && ae <= 22) {
            d = exp10 < 0 ? d / kPow10[ae] : d * kPow10[ae];
        } else {
            while (ae > 22 && d != 0.0 && d <= 1.7e308) {
                d = exp10 < 0 ? d / 1e22 : d * 1e22;
                ae -= 22;
            }
            if (ae > 0) d = exp10 < 0 ? d / kPow10[ae] : d * kPow10[ae];
        }
    }
    out = neg ? -d : d;
    return true;
}

double ParseLeadingDouble(const char* s) {
    const char* end;
    double d;
    return ParseDouble(s, &end, d) ? d : 0.0;
}

int ParseLeadingInt(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f') s++;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    int64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (v < 100000000000LL) v = v * 10 + (*s - '0'); // saturates well past int range
    }
    if (neg) v = -v;
    if (v > 2147483647LL) return 2147483647;
    if (v < -2147483647LL - 1) return -2147483647 - 1;
    return (int)v;
}

uint64_t ParseLeadingUInt64(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f') s++;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return UINT64_MAX; // the CRT reports ERANGE and returns UINT64_MAX, sign ignored
        v = v * 10 + d;
    }
    return neg ? (uint64_t)0 - v : v;
}

uint32_t ParseHexPrefix(const char* s) {
    uint32_t v = 0;
    for (;; s++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

std::string FormatCompactNumber(double v) {
    std::string s = FormatFixed(v, 2);
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot) last--; // nothing left after the dot -- drop it too
        s.erase(last + 1);
    }
    return s;
}
} // namespace AImgDecoderInternal
using AImgDecoderInternal::FormatCompactNumber;

bool AImgDecoder::DecodeDrawThings(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "seed_mode" is Draw Things' own vocabulary; no other generator's JSON
    // uses this key.
    if (!root->objVal.count("seed_mode")) return false;

    info.has_metadata = true;
    info.generator = L"Draw Things";
    info.prompt = Utf8ToWstring(root->getStr("c"));
    info.negative_prompt = Utf8ToWstring(root->getStr("uc"));
    info.model = Utf8ToWstring(root->getStr("model"));
    info.sampler = Utf8ToWstring(root->getStr("sampler"));
    info.size = Utf8ToWstring(root->getStr("size"));

    ExtractSeedCfgSteps(root, "scale", info);

    // JsonNumericValue: presence alone isn't proof of a number.
    double num;
    if (JsonNumericValue(root->find("strength"), num)) {
        info.denoising_strength = num;
        info.has_denoising_strength = true;
    }

    if (const auto* v2 = root->getObj("v2")) {
        if (JsonNumericValue(v2->find("clipSkip"), num)) {
            info.clip_skip = (int32_t)SimpleJson::ClampDoubleToInt64(num);
            info.has_clip_skip = true;
        }
    }

    const auto* loraField = root->find("lora");
    if (loraField && loraField->type == SimpleJson::JsonType::Array) {
        std::string joined;
        for (const auto& item : loraField->arrVal) {
            if (!item || item->type != SimpleJson::JsonType::Object) continue;
            std::string name = item->getStr("model");
            if (name.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += name + ": " + FormatCompactNumber(item->getNum("weight"));
        }
        if (!joined.empty()) info.lora = Utf8ToWstring(joined);
    }

    info.full_parameters_utf8 = originalText;
    return true;
}


// ---------------------------------------------------------------------------
// InvokeAI / SwarmUI / NovelAI Decoders
// ---------------------------------------------------------------------------
//
// One shape shared by three generators: gate on a distinctive key, then read
// prompt/negative/model/cfg/sampler off flat keys whose NAMES differ. Each
// Decode* below just supplies the names. Easy Diffusion is deliberately NOT
// folded in -- its extra fields would add more config surface than the
// duplication they remove.
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

// A free function so the file-local SimpleGeneratorConfig can be passed by
// reference with real type checking, instead of the `const void*` a member
// declaration in the header would have forced.
bool DecodeSimpleGraphGenerator(const SimpleJson::JsonValue* root, const std::string& originalText,
                                 AImgInfo& info, const SimpleGeneratorConfig& cfg) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    bool gated = root->objVal.count(cfg.gateKey1) ||
                 (cfg.gateKey2[0] != '\0' && root->objVal.count(cfg.gateKey2));
    if (!gated) return false;

    info.has_metadata = true;
    info.generator = cfg.generatorName;
    std::string prompt = root->getStr(cfg.promptKey);
    if (prompt.empty() && cfg.promptFallbackKey[0] != '\0') prompt = root->getStr(cfg.promptFallbackKey);
    info.prompt = AImgDecoder::Utf8ToWstring(prompt);
    info.negative_prompt = AImgDecoder::Utf8ToWstring(root->getStr(cfg.negPromptKey));
    if (cfg.modelKey[0] != '\0') info.model = AImgDecoder::Utf8ToWstring(root->getStr(cfg.modelKey));
    ExtractSeedCfgSteps(root, cfg.cfgKey, info);
    if (cfg.samplerKey[0] != '\0') info.sampler = AImgDecoder::Utf8ToWstring(root->getStr(cfg.samplerKey));
    info.full_parameters_utf8 = originalText;
    return true;
}
} // namespace

bool AImgDecoder::DecodeInvokeAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    // "positive_prompt" alone, never OR'd with "negative_prompt": real
    // InvokeAI metadata always sets it, while unrelated tools carry a bare
    // "negative_prompt" that a looser gate would claim.
    static const SimpleGeneratorConfig cfg = {
        "positive_prompt", "", L"InvokeAI",
        "positive_prompt", "prompt", "negative_prompt", "model", "cfg_scale", "scheduler"
    };
    if (!DecodeSimpleGraphGenerator(root, originalText, info, cfg)) return false;

    // The shared helper reads flat string keys only, but real InvokeAI nests
    // model/vae as objects and LoRAs as an array -- read those here so those
    // columns aren't left empty despite the data being present.
    if (const auto* modelObj = root->getObj("model")) {
        std::string name = modelObj->getStr("name");
        if (!name.empty()) info.model = Utf8ToWstring(name);
        std::string hash = modelObj->getStr("hash");
        static const std::string kBlake3Prefix = "blake3:";
        if (hash.compare(0, kBlake3Prefix.size(), kBlake3Prefix) == 0) hash.erase(0, kBlake3Prefix.size());
        if (!hash.empty()) info.model_hash = Utf8ToWstring(hash);
    }
    if (const auto* vaeObj = root->getObj("vae")) {
        std::string name = vaeObj->getStr("name");
        if (!name.empty()) info.vae = Utf8ToWstring(name);
    }
    info.size = MakeSizeString(root->getInt64("width"), root->getInt64("height"));

    const auto* lorasField = root->find("loras");
    if (lorasField && lorasField->type == SimpleJson::JsonType::Array) {
        std::string joined;
        for (const auto& item : lorasField->arrVal) {
            if (!item || item->type != SimpleJson::JsonType::Object) continue;
            const auto* loraModel = item->getObj("model");
            std::string name = loraModel ? loraModel->getStr("name") : "";
            if (name.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += name + ": " + FormatCompactNumber(item->getNum("weight"));
        }
        if (!joined.empty()) info.lora = Utf8ToWstring(joined);
    }

    return true;
}


// ---------------------------------------------------------------------------
// WanGP Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeWanGP(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    // "activated_loras" is WanGP's own vocabulary. Must run before InvokeAI,
    // whose gate its bare "negative_prompt" key would otherwise satisfy.
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    if (!root->objVal.count("activated_loras")) return false;

    info.has_metadata = true;
    info.generator = L"WanGP";
    info.prompt = Utf8ToWstring(root->getStr("prompt"));
    info.negative_prompt = Utf8ToWstring(root->getStr("negative_prompt"));

    // JsonNumericValue: presence alone isn't proof of a number.
    double num;
    if (JsonNumericValue(root->find("seed"), num)) {
        info.seed = SimpleJson::ClampDoubleToInt64(num);
        info.has_seed = true;
    }
    if (JsonNumericValue(root->find("num_inference_steps"), num)) {
        info.steps = (int32_t)SimpleJson::ClampDoubleToInt64(num);
        info.has_steps = true;
    }

    info.size = Utf8ToWstring(root->getStr("resolution"));

    // model_filename is a full URL/path; display its last segment, falling
    // back to the short model_type code.
    std::string modelFile = root->getStr("model_filename");
    if (!modelFile.empty()) {
        size_t slash = modelFile.find_last_of('/');
        std::string baseName = (slash == std::string::npos) ? modelFile : modelFile.substr(slash + 1);
        info.model = StripModelExtension(Utf8ToWstring(baseName));
    } else {
        info.model = Utf8ToWstring(root->getStr("model_type"));
    }

    // Paths zipped against a parallel whitespace-separated weight list.
    // NormalizeLoraField strips extensions but not directories, so the folder
    // prefix has to go here.
    const auto* lorasField = root->find("activated_loras");
    if (lorasField && lorasField->type == SimpleJson::JsonType::Array) {
        std::vector<std::string> weights;
        {
            std::string multipliers = root->getStr("loras_multipliers");
            size_t pos = 0;
            while (pos < multipliers.size()) {
                while (pos < multipliers.size() && isspace((unsigned char)multipliers[pos])) pos++;
                size_t start = pos;
                while (pos < multipliers.size() && !isspace((unsigned char)multipliers[pos])) pos++;
                if (pos > start) weights.push_back(multipliers.substr(start, pos - start));
            }
        }
        std::string joined;
        for (size_t i = 0; i < lorasField->arrVal.size(); i++) {
            const auto& item = lorasField->arrVal[i];
            if (!item || item->type != SimpleJson::JsonType::String) continue;
            std::string path = item->strVal;
            size_t slash = path.find_last_of('/');
            std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
            if (name.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += name;
            if (i < weights.size()) joined += ": " + weights[i];
        }
        if (!joined.empty()) info.lora = Utf8ToWstring(joined);
    }

    info.full_parameters_utf8 = originalText;
    return true;
}


// ---------------------------------------------------------------------------
// SwarmUI Decoder
// ---------------------------------------------------------------------------

namespace {
// "loras"/"loraweights" are a JSON array of strings in real files but SwarmUI's
// format also allows one comma-separated string; accept both.
// Walks such a list one trimmed item at a time without materializing it.
struct SwarmListCursor {
    const SimpleJson::JsonValue* v;
    size_t idx = 0;      // array index, or byte offset into a string list
    bool done = false;
    explicit SwarmListCursor(const SimpleJson::JsonValue* list) : v(list) {}
    bool Next(std::string& out) {
        out.clear();
        if (!v || done) return false;
        if (v->type == SimpleJson::JsonType::Array) {
            if (idx >= v->arrVal.size()) return false;
            const SimpleJson::JsonValue* item = v->arrVal[idx++].get();
            if (item && item->type == SimpleJson::JsonType::Number) {
                out = AImgDecoderInternal::FormatCompactNumber(item->numVal);
                return true;
            }
            if (item && item->type == SimpleJson::JsonType::String) Assign(out, item->strVal, 0, item->strVal.size());
            return true;
        }
        if (v->type != SimpleJson::JsonType::String) return false;
        const std::string& s = v->strVal;
        size_t comma = s.find(',', idx);
        size_t end = comma == std::string::npos ? s.size() : comma;
        Assign(out, s, idx, end);
        if (comma == std::string::npos) done = true; else idx = comma + 1;
        return true;
    }
    static void Assign(std::string& out, const std::string& s, size_t b, size_t e) {
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
        out.assign(s, b, e - b);
    }
};

std::string SwarmLoraList(const SimpleJson::JsonValue* names, const SimpleJson::JsonValue* weights) {
    SwarmListCursor n(names), w(weights);
    std::string joined, name, weight;
    while (n.Next(name)) {
        bool hasWeight = w.Next(weight);
        if (name.empty()) continue;
        if (!joined.empty()) joined += ", ";
        joined += name;
        if (!hasWeight || weight.empty()) continue;
        joined += ": ";
        const char* end = nullptr;
        double d;
        if (AImgDecoderInternal::ParseDouble(weight.c_str(), &end, d) && *end == '\0') {
            joined += AImgDecoderInternal::FormatCompactNumber(d);
        } else {
            joined += weight; // non-numeric weight: keep as written
        }
    }
    return joined;
}
} // namespace

bool AImgDecoder::DecodeSwarmUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    // A real SwarmUI PNG nests its parameters under "sui_image_params", not
    // at the root -- unwrap when present, but keep the flat shape working.
    const SimpleJson::JsonValue* unwrapped = root;
    if (root) {
        if (const auto* p = root->getObj("sui_image_params")) unwrapped = p;
    }
    // "cfgscale"/"negativeprompt" (no separator) is SwarmUI's own naming;
    // without this gate any JSON object would match.
    static const SimpleGeneratorConfig cfg = {
        "cfgscale", "negativeprompt", L"SwarmUI",
        "prompt", "", "negativeprompt", "model", "cfgscale", "sampler"
    };
    if (!DecodeSimpleGraphGenerator(unwrapped, originalText, info, cfg)) return false;
    if (!unwrapped) return true;

    std::string s = unwrapped->getStr("scheduler");
    if (!s.empty()) info.scheduler = Utf8ToWstring(s);
    s = unwrapped->getStr("vae");
    if (!s.empty()) info.vae = Utf8ToWstring(s);

    double num;
    double w = 0, h = 0;
    if (JsonNumericValue(unwrapped->find("width"), w) && JsonNumericValue(unwrapped->find("height"), h)) {
        info.size = MakeSizeString(SimpleJson::ClampDoubleToInt64(w), SimpleJson::ClampDoubleToInt64(h));
    }

    std::string lora = SwarmLoraList(unwrapped->find("loras"), unwrapped->find("loraweights"));
    if (!lora.empty()) info.lora = Utf8ToWstring(lora);

    // clipstopatlayer is stored negative (-2 = A1111's "Clip skip: 2").
    if (JsonNumericValue(unwrapped->find("clipstopatlayer"), num) && num < 0) {
        info.clip_skip = (int32_t)SimpleJson::ClampDoubleToInt64(-num);
        info.has_clip_skip = true;
    }
    return true;
}


// ---------------------------------------------------------------------------
// Fooocus Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeFooocus(const std::string& paramText, AImgInfo& info) {
    if (paramText.find("Fooocus") == std::string::npos && paramText.find("Base Model:") == std::string::npos) {
        return false;
    }
    // Fooocus emits an A1111-style block: reuse that parser, then relabel.
    if (!DecodeAutomatic1111(paramText, info)) return false;
    info.generator = L"Fooocus";
    return true;
}


// ---------------------------------------------------------------------------
// NovelAI Decoder
// ---------------------------------------------------------------------------

bool AImgDecoder::DecodeNovelAI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;
    // "uc" is also Draw Things' negative-prompt key. Reject its distinctive
    // "seed_mode" explicitly so this gate stands on its own rather than on
    // DecodeCore happening to run DecodeDrawThings first.
    if (root->objVal.count("seed_mode")) return false;
    // "uc" (undesired content) is NovelAI's naming; without this gate any
    // JSON object would match.
    static const SimpleGeneratorConfig cfg = {
        "uc", "", L"NovelAI",
        "prompt", "", "uc", "", "scale", "sampler"
    };
    if (!DecodeSimpleGraphGenerator(root, originalText, info, cfg)) return false;
    info.size = MakeSizeString(root->getInt64("width"), root->getInt64("height"));
    return true;
}
