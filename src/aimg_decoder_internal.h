#ifndef AIMG_DECODER_INTERNAL_H
#define AIMG_DECODER_INTERNAL_H

// Implementation header shared by aimg_decoder.cpp and comfyui_decoder.cpp
// only; aimg.cpp must not include it. SimpleJson::JsonParser deliberately
// stays in aimg_decoder.cpp -- comfyui_decoder.cpp never uses it, so putting
// it here would make both files pay for it.

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
} // namespace AImgDecoderInternal

#endif // AIMG_DECODER_INTERNAL_H
