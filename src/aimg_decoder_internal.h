#ifndef AIMG_DECODER_INTERNAL_H
#define AIMG_DECODER_INTERNAL_H

// Implementation header shared by aimg_decoder.cpp and comfyui_decoder.cpp
// only; aimg.cpp must not include it. SimpleJson::JsonParser deliberately
// stays in aimg_decoder.cpp -- comfyui_decoder.cpp reaches it only through
// the ParseJson wrapper below, so putting it here would make both files pay
// for it.

#include <cstdint>
#include <cmath>
#include <string>
#include <map>
#include <vector>
#include <memory>

namespace SimpleJson {

// A double->int64_t cast is UB when n doesn't fit in int64_t's range (e.g. a
// crafted "seed": 1e300); clamp first so malformed/adversarial JSON can only
// produce a saturated value, never UB.
inline int64_t ClampDoubleToInt64(double n) {
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
    // unique_ptr, not shared_ptr: ownership is strictly tree-exclusive and
    // consumers only ever hold raw `const JsonValue*` views, so the
    // control-block allocation and atomic refcount per node buy nothing.
    std::vector<std::unique_ptr<JsonValue>> arrVal;
    // std::map, not unordered_map: a ComfyUI graph creates hundreds of tiny
    // per-node objects, where unordered_map's per-instance bucket array
    // measured ~2x slower and larger than std::map's single tree node.
    // Re-benchmark before re-attempting this swap.
    std::map<std::string, std::unique_ptr<JsonValue>> objVal;

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

    // Single lookup, for call sites that would otherwise do count()+at().
    const JsonValue* find(const std::string& key) const {
        if (type != JsonType::Object) return nullptr;
        auto it = objVal.find(key);
        return it != objVal.end() ? it->second.get() : nullptr;
    }
};

} // namespace SimpleJson

// Renders a LoRA weight compactly (1.14, 0.9, 1) instead of float noise
// (1.1399999856948853). The one helper genuinely shared across the two
// decoder translation units; defined in aimg_decoder.cpp. Namespaced rather
// than global so it can't silently collide at file scope in either.
namespace AImgDecoderInternal {
std::string FormatCompactNumber(double v);

// Own number parsing/formatting instead of strtod/atof/atoi/strtoul/printf,
// which pull the CRT's locale-aware conversion engines into the /MT build.
// Deviation: inf/nan/hex-float spellings do not parse as numbers.
//
// strtod-compatible subset; false with *end == s when no digit was consumed.
// Bit-identical to strtod when the significand fits 2^53 and the decimal
// exponent is within +-22, otherwise within a few ulp.
bool ParseDouble(const char* s, const char** end, double& out);
// atof semantics: 0.0 when nothing parses.
double ParseLeadingDouble(const char* s);
// atoi semantics, but out-of-range clamps instead of being undefined.
int ParseLeadingInt(const char* s);
// _strtoui64(s, NULL, 10) semantics; overflow saturates to UINT64_MAX.
uint64_t ParseLeadingUInt64(const char* s);
// strtoul(s, NULL, 16) over the 4 hex digits of a JSON \u escape.
uint32_t ParseHexPrefix(const char* s);
// Identical to snprintf("%.*f", prec, v) for finite |v| < 2^63, prec 0..6,
// ties included; |v| >= 2^63 saturates the integer part.
std::string FormatFixed(double v, int prec);

// Parses a JSON value nested inside an already-parsed tree (a field holding
// a JSON-encoded string). nullptr on anything that doesn't parse.
std::unique_ptr<SimpleJson::JsonValue> ParseJson(const std::string& text);
} // namespace AImgDecoderInternal

#endif // AIMG_DECODER_INTERNAL_H
