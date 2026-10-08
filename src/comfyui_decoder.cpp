#include "aimg_decoder.h"
#include "aimg_decoder_internal.h"
#include "aimg_abort.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <emmintrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <cwchar>
#include <windows.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <memory>

// Its own translation unit: the types here (UiLinkIndex/SubgraphRegistry/
// BoundaryOutcome/WidgetFieldMap) are touched by nothing else. The shared
// surface is deliberately thin -- see aimg_decoder_internal.h -- and
// everything else keeps internal linkage.
using AImgDecoderInternal::FormatCompactNumber;
using AImgDecoderInternal::FormatFixed;
using AImgDecoderInternal::ParseJson;

// Narrower than LooksLikeGenerationText, whose token-count rule is too
// strict for a real prompt (a single short tag is legitimate): this rejects
// only text starting with '{'/'['. Some rich-prompt-editor node packs
// serialize their editor state as JSON straight into a "text" widget, which
// would otherwise be accepted as a literal prompt. Applied only where a
// resolved value is about to become the final prompt, so a companion A1111
// block can still supply the real one via MergeGaps.
static bool LooksLikeSerializedTextBlob(const std::string& s) {
    size_t firstNonSpace = s.find_first_not_of(" \t\r\n");
    return firstNonSpace != std::string::npos && (s[firstNonSpace] == '{' || s[firstNonSpace] == '[');
}

// ---------------------------------------------------------------------------
// ComfyUI Decoder (Supports API Graph format and UI Workflow format)
// ---------------------------------------------------------------------------

namespace {

using SimpleJson::JsonType;
using SimpleJson::JsonValue;
using SimpleJson::ClampDoubleToInt64;

// A raw (int) cast of an out-of-range or NaN double is UB; -1 is the
// "unknown slot" value every consumer already rejects.
int JsonSlotIndex(const JsonValue* v) {
    if (!v || v->type != JsonType::Number) return -1;
    return (v->numVal >= 0 && v->numVal <= INT_MAX) ? (int)v->numVal : -1;
}

// Node/link ids are a JSON number or string depending on export version;
// normalize to the string form every lookup here keys on.
std::string JsonIdToString(const JsonValue* idv) {
    if (!idv) return "";
    if (idv->type == JsonType::Number) return std::to_string(ClampDoubleToInt64(idv->numVal));
    if (idv->type == JsonType::String) return idv->strVal;
    return "";
}

// class_type matching where custom packs vary a stock name's capitalization
// (e.g. "UnetLoaderGGUF" vs. stock "UNETLoader") as well as affixing it.
bool ContainsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    // Called several times per node across hundreds of nodes: a per-character
    // comparator avoids the two heap-allocating lowercased copies the naive
    // form makes on every call, which measured ~10% slower.
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != haystack.end();
}

// Parses A1111-style "<lora:name:weight>" tags out of arbitrary text into
// the "name: weight" display shape every other generator's LoRA field uses.
void ExtractLoraTags(const std::string& text, std::vector<std::string>& out) {
    size_t pos = 0;
    while ((pos = text.find("<lora:", pos)) != std::string::npos) {
        size_t end = text.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = text.substr(pos + 6, end - (pos + 6));
        size_t colon = tag.find(':');
        std::string name = colon == std::string::npos ? tag : tag.substr(0, colon);
        std::string weight = colon == std::string::npos ? "1" : tag.substr(colon + 1);
        if (!name.empty()) out.push_back(name + ": " + weight);
        pos = end + 1;
    }
}

std::string TrimWs(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Loader nodes that hold the positive/negative text, checkpoint name and empty
// latent size themselves. Substring, not ==: real files carry decorated names.
bool IsLoaderHeldPromptClass(const std::string& c) {
    return c.find("Efficient Loader") != std::string::npos || c.find("Eff. Loader SDXL") != std::string::npos ||
           c.find("easy fullLoader") != std::string::npos;
}

// Length of the model-file extension s ends in, 0 when none. One list serves
// both the "is this a model file" test and the extension strip. The test
// guards the UI-format LoRA collector's positional fallback (a node whose type
// merely CONTAINS "Lora" can hold a filename-prefix template) and loader-held
// ckpt_name reads, which can hold a cosmetic placeholder.
size_t ModelFileExtensionLength(const std::string& s) {
    static const char* const kExtensions[] = {".safetensors", ".ckpt", ".gguf", ".pt", ".pth", ".bin", ".sft"};
    for (const char* ext : kExtensions) {
        size_t extLen = strlen(ext);
        if (s.size() < extLen) continue;
        if (std::equal(s.end() - extLen, s.end(), ext, ext + extLen,
                        [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); })) {
            return extLen;
        }
    }
    return 0;
}

bool LooksLikeModelFileName(const std::string& s) {
    return ModelFileExtensionLength(s) != 0;
}

std::string StripModelFileExtension(const std::string& s) {
    return s.substr(0, s.size() - ModelFileExtensionLength(s));
}

// A graph link is a 2-element array [node_id, output_slot].
std::string GetLinkNodeId(const JsonValue* v) {
    if (!v || v->type != JsonType::Array || v->arrVal.empty()) return "";
    return JsonIdToString(v->arrVal[0].get());
}

// Python's round() for a non-negative x below 2^53 (ties to even). Integer
// truncation rather than std::floor, an out-of-line CRT call under /fp:precise.
double RoundHalfEven(double x) {
    int64_t i = (int64_t)x;
    double diff = x - (double)i;
    if (diff > 0.5 || (diff == 0.5 && (i & 1))) ++i;
    return (double)i;
}

// The core ResolutionSelector node is a pure formula over three literals, so
// the size an EmptyLatentImage receives from it can be recomputed. Any
// non-literal or unknown input returns false (blank beats a wrong guess).
bool ResolutionSelectorSize(const JsonValue* node, int64_t& w, int64_t& h) {
    const JsonValue* in = node ? node->getObj("inputs") : nullptr;
    if (!in) return false;
    const JsonValue* ar = in->find("aspect_ratio");
    const JsonValue* mp = in->find("megapixels");
    const JsonValue* mu = in->find("multiple");
    if (!ar || ar->type != JsonType::String || !mp || mp->type != JsonType::Number) return false;
    static const char* const kRatios[] = {"1:1", "2:3", "3:2", "3:4", "4:3", "9:16", "16:9", "21:9"};
    static const int kW[] = {1, 2, 3, 3, 4, 9, 16, 21};
    static const int kH[] = {1, 3, 2, 4, 3, 16, 9, 9};
    std::string tok = ar->strVal.substr(0, ar->strVal.find(' '));
    int ri = -1;
    for (int i = 0; i < 8; ++i) if (tok == kRatios[i]) { ri = i; break; }
    if (ri < 0) return false;
    double mult = 8.0; // the value hardcoded before the input existed
    if (mu) {
        if (mu->type != JsonType::Number) return false;
        mult = mu->numVal;
    }
    double megapixels = mp->numVal;
    if (!(megapixels > 0.0) || !(mult >= 1.0) || megapixels > 1e6 || mult > 1e6) return false;
    // SSE2 sqrtsd is the correctly rounded IEEE square root, bit-identical to
    // Python's math.sqrt, without the CRT's out-of-line sqrt.
    double scale = _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(),
                                             _mm_set_sd(megapixels * 1024.0 * 1024.0 / (double)(kW[ri] * kH[ri]))));
    w = ClampDoubleToInt64(RoundHalfEven(kW[ri] * scale / mult) * mult);
    h = ClampDoubleToInt64(RoundHalfEven(kH[ri] * scale / mult) * mult);
    return w > 0 && h > 0;
}

// Follows an API-format field that may be a literal OR a link through a
// bounded chain of primitive nodes to the underlying literal string. Many
// graphs route a CLIPTextEncode's "text" (or a latent node's width/height)
// through a separate primitive node, where a plain getStr()/getNum() sees
// only the link array and comes back empty.
double ResolveNumberField(const JsonValue* nodesObj, const JsonValue* field, bool& found, const char* preferredKey = nullptr, int depth = 0);

// A join of literal separators only (",", ", ") is what a join node yields when
// every real part was unresolvable; showing that is a visible wrong answer, so
// it counts as empty. Bytes >= 0x80 count as text so CJK/UTF-8 prompts survive.
std::string DropIfNoAlnum(std::string s) {
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80) return s;
    }
    return std::string();
}

// Reached from both ResolveTextField (a plain text link) and ResolveClipText
// (a sampler's conditioning link pointing straight at the node).
std::string ResolveClipTextEncodeSdxl(const JsonValue* nodesObj, const JsonValue* refInputs, int depth);
std::string ResolveOrexClipTextEncode(const JsonValue* nodesObj, const JsonValue* refInputs, int depth);

std::string ResolveTextField(const JsonValue* nodesObj, const JsonValue* field, const char* preferredKey = nullptr, int depth = 0) {
    // Call budget per resolution tree, on top of the depth limit: a diamond-
    // shaped fan-in of join nodes re-resolves a shared ancestor once per path
    // and can go exponential long before any path gets deep. Function-local
    // thread_local for the same C++17-avoidance reason as AbortFlagSlot().
    static thread_local int steps = 0;
    if (depth == 0) steps = 0;
    if (++steps > 4096) return "";
    if (!field || depth > 12) return "";
    if (field->type == JsonType::String) return field->strVal;
    if (field->type == JsonType::Number) {
        // A numeric literal feeding a text field: stringify, don't lose it.
        double n = field->numVal;
        int64_t asInt = ClampDoubleToInt64(n);
        if (n == (double)asInt) return std::to_string(asInt);
        return FormatFixed(n, 6); // matches std::to_string(double) without printf
    }
    if (field->type != JsonType::Array) return "";
    std::string nodeId = GetLinkNodeId(field);
    if (nodeId.empty()) return "";
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return "";
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs) return "";

    // Prompt-enhancement workflows route "text" through stock control-flow
    // nodes before the literal. Both graph formats serialize the same node's
    // Python INPUT_TYPES, so the socket names here are identical to
    // ResolveUiTextThroughLink's for the same node types.
    std::string classType = it->second->getStr("class_type");
    if (classType == "ComfySwitchNode") {
        bool switchFound = false;
        double switchVal = 0.0;
        if (const JsonValue* sw = refInputs->find("switch")) {
            if (sw->type == JsonType::Bool) { switchVal = sw->boolVal ? 1.0 : 0.0; switchFound = true; }
            else { switchVal = ResolveNumberField(nodesObj, sw, switchFound); }
        }
        // Prefer the branch the condition selects, but fall back to both if
        // it resolves empty -- it may flow through a node whose output is
        // computed at runtime and never stored, i.e. unrecoverable.
        if (switchFound) {
            const char* primary = switchVal != 0.0 ? "on_true" : "on_false";
            if (const JsonValue* v = refInputs->find(primary)) {
                std::string r = ResolveTextField(nodesObj, v, preferredKey, depth + 1);
                if (!r.empty()) return r;
            }
        }
        if (const JsonValue* v = refInputs->find("on_true")) {
            std::string r = ResolveTextField(nodesObj, v, preferredKey, depth + 1);
            if (!r.empty()) return r;
        }
        if (const JsonValue* v = refInputs->find("on_false")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "StringConcatenate") {
        const JsonValue* aField = refInputs->find("string_a");
        const JsonValue* bField = refInputs->find("string_b");
        std::string a = aField ? ResolveTextField(nodesObj, aField, preferredKey, depth + 1) : "";
        std::string b = bField ? ResolveTextField(nodesObj, bField, preferredKey, depth + 1) : "";
        if (a.empty()) return b;
        if (b.empty()) return a;
        std::string sep = refInputs->getStr("delimiter");
        return DropIfNoAlnum(a + sep + b);
    }
    if (classType == "Text Concatenate") {
        // API-format twin of the UI branch in ResolveUiTextThroughLink. An
        // absent delimiter joins with "" rather than an invented default.
        std::string sep = refInputs->getStr("delimiter");
        std::string joined;
        static const char* const kParts[] = {"text_a", "text_b", "text_c", "text_d"};
        for (const char* key : kParts) {
            const JsonValue* v = refInputs->find(key);
            if (!v) continue;
            std::string part = ResolveTextField(nodesObj, v, preferredKey, depth + 1);
            if (part.empty()) continue;
            if (!joined.empty()) joined += sep;
            joined += part;
        }
        return DropIfNoAlnum(std::move(joined));
    }
    if (classType == "Reroute" || classType == "PreviewAny") {
        if (const JsonValue* v = refInputs->find("source")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "RegexReplace") {
        // A regex find/replace utility node commonly sits between a prompt's
        // source and its CLIPTextEncode. Its literal input is named "string",
        // which the stock "text"/"value" fallback below misses, resolving the
        // whole chain to "". The substitution itself is not applied: the
        // pre-replace text is a cosmetic approximation, still far better than
        // an empty or wrongly-attributed result.
        if (const JsonValue* v = refInputs->find("string")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "String Literal") {
        // Its text lives in "string", which the generic fallback never reads.
        if (const JsonValue* v = refInputs->find("string")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "Flux_Finish_StylesStyler") {
        // A style-preset node fed by a "text_positive"/"text_negative" pair
        // rather than the generic keys below; "text_positive" is the one
        // wired into a CLIPTextEncode in every real sample seen.
        std::string positive = refInputs->getStr("text_positive");
        if (!positive.empty()) return positive;
        return refInputs->getStr("text_negative");
    }
    if (classType == "ImpactWildcardProcessor") {
        // A wildcard-prompt node keeps the AUTHORED template (unresolved
        // "{a|b|c}" alternatives) under "wildcard_text" and the expanded text
        // actually used under "populated_text". The stock fallback below
        // finds neither key.
        std::string populated = refInputs->getStr("populated_text");
        if (!populated.empty()) return populated;
        return refInputs->getStr("wildcard_text");
    }
    if (classType == "StringFunction|pysssss") {
        // "append" joins text_a..c (", " only when tidy_tags is "yes");
        // "replace" substitutes text_b with text_c in text_a. Its "result"
        // field is never read: it is a cached display value from the last run.
        std::string a = ResolveTextField(nodesObj, refInputs->find("text_a"), preferredKey, depth + 1);
        std::string b = ResolveTextField(nodesObj, refInputs->find("text_b"), preferredKey, depth + 1);
        if (refInputs->getStr("action") == "replace") {
            std::string c = ResolveTextField(nodesObj, refInputs->find("text_c"), preferredKey, depth + 1);
            if (b.empty()) return a;
            std::string result;
            size_t pos = 0;
            for (;;) {
                size_t found = a.find(b, pos);
                if (found == std::string::npos) { result.append(a, pos, std::string::npos); break; }
                result.append(a, pos, found - pos);
                result += c;
                pos = found + b.size();
            }
            return result;
        }
        std::string c = ResolveTextField(nodesObj, refInputs->find("text_c"), preferredKey, depth + 1);
        std::string sep = refInputs->getStr("tidy_tags") == "yes" ? ", " : "";
        std::string joined;
        const std::string* parts[] = {&a, &b, &c};
        for (const std::string* part : parts) {
            if (part->empty()) continue;
            if (!joined.empty()) joined += sep;
            joined += *part;
        }
        return DropIfNoAlnum(std::move(joined));
    }
    if (classType == "Any Switch (rgthree)") {
        // The node has no selected-index widget: the first any_NN input (key
        // order) that resolves to something wins, as the node itself behaves.
        for (const auto& kv : refInputs->objVal) {
            if (kv.first.rfind("any_", 0) != 0) continue;
            std::string r = ResolveTextField(nodesObj, kv.second.get(), preferredKey, depth + 1);
            if (!r.empty()) return r;
        }
        return "";
    }
    if (classType == "easy textIndexSwitch") {
        // Only the branch its "index" selects: an unselected branch is a wrong
        // answer, not a fallback.
        bool found = false;
        double idxNum = ResolveNumberField(nodesObj, refInputs->find("index"), found);
        if (!found) return "";
        std::string key = "text" + std::to_string((int)ClampDoubleToInt64(idxNum));
        return ResolveTextField(nodesObj, refInputs->find(key), preferredKey, depth + 1);
    }
    if (classType == "easy promptConcat") {
        // "prompt2" is an optional input commonly left unwired.
        std::string p1 = ResolveTextField(nodesObj, refInputs->find("prompt1"), preferredKey, depth + 1);
        std::string p2 = ResolveTextField(nodesObj, refInputs->find("prompt2"), preferredKey, depth + 1);
        if (p1.empty()) return p2;
        if (p2.empty()) return p1;
        return DropIfNoAlnum(p1 + refInputs->getStr("separator") + p2);
    }
    if (classType == "easy positive") {
        if (const JsonValue* v = refInputs->find("positive")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "DF_Text_Box") {
        if (const JsonValue* v = refInputs->find("Text")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "PromptComposerGrouping") {
        // Inactive only on a literal "active": false (a linked value counts as
        // active). Wraps the text in "(text:weight)" unless weight is 1.
        const JsonValue* activeField = refInputs->find("active");
        if (activeField && activeField->type == JsonType::Bool && !activeField->boolVal) return "";
        std::string text = ResolveTextField(nodesObj, refInputs->find("text_in"), preferredKey, depth + 1);
        if (text.empty()) return "";
        bool weightFound = false;
        double weight = ResolveNumberField(nodesObj, refInputs->find("weight"), weightFound);
        if (!weightFound || weight == 1.0) return text;
        return "(" + text + ":" + FormatCompactNumber(weight) + ")";
    }
    if (classType == "easy cleanGpuUsed") {
        // A GPU-cleanup passthrough whose generic "anything" input can carry
        // a text chain like a Reroute.
        if (const JsonValue* v = refInputs->find("anything")) {
            return ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        }
        return "";
    }
    if (classType == "Lora Loader (LoraManager)" || classType == "TriggerWord Toggle (LoraManager)") {
        // Their "text" holds computed LoRA tags/trigger words, not prompt
        // text; the generic fallback would inject "<lora:...>" into the prompt.
        return "";
    }
    if (classType == "CLIPTextEncodeSDXL") {
        // No plain "text" -- see ResolveClipTextEncodeSdxl.
        return ResolveClipTextEncodeSdxl(nodesObj, refInputs, depth);
    }
    if (classType == "CR Prompt List") {
        // Mirrors the node: lines[start_index : start_index + max_rows], slot
        // 0 = prepend + line + append, slot 1 = the bare line. With more than
        // one row selected there is no way to know which one this image used.
        int slot = 0;
        if (field->arrVal.size() > 1 && field->arrVal[1] && field->arrVal[1]->type == JsonType::Number) {
            slot = (int)ClampDoubleToInt64(field->arrVal[1]->numVal);
        }
        std::string multiline = ResolveTextField(nodesObj, refInputs->find("multiline_text"), preferredKey, depth + 1);
        std::vector<std::string> lines;
        size_t linePos = 0;
        while (linePos <= multiline.size()) {
            size_t nl = multiline.find('\n', linePos);
            std::string line = (nl == std::string::npos) ? multiline.substr(linePos) : multiline.substr(linePos, nl - linePos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (nl == std::string::npos) break;
            linePos = nl + 1;
        }
        if (lines.empty()) return "";
        bool foundIdx = false, foundRows = false;
        double idxNum = ResolveNumberField(nodesObj, refInputs->find("start_index"), foundIdx);
        double rowsNum = ResolveNumberField(nodesObj, refInputs->find("max_rows"), foundRows);
        int startIndex = foundIdx ? (int)ClampDoubleToInt64(idxNum) : 0;
        int maxRows = foundRows ? (int)ClampDoubleToInt64(rowsNum) : 9999;
        if (startIndex < 0) startIndex = 0;
        if (startIndex > (int)lines.size() - 1) startIndex = (int)lines.size() - 1;
        int end = startIndex + maxRows;
        if (end > (int)lines.size()) end = (int)lines.size();
        if (end - startIndex != 1) return "";
        const std::string& line = lines[startIndex];
        if (slot == 1) return line;
        if (slot == 0) {
            std::string prepend = ResolveTextField(nodesObj, refInputs->find("prepend_text"), preferredKey, depth + 1);
            std::string append = ResolveTextField(nodesObj, refInputs->find("append_text"), preferredKey, depth + 1);
            return prepend + line + append;
        }
        return "";
    }
    if (classType == "PixaromaPrompt") {
        // Own text, order and separator live in one JSON-encoded "PromptState"
        // string. An unparseable one degrades to an empty own side, since the
        // linked "text_in" may still carry the prompt.
        std::string mine, order = "mine", sep = ", ";
        auto state = ParseJson(refInputs->getStr("PromptState"));
        if (state && state->type == JsonType::Object) {
            mine = state->getStr("text");
            std::string o = state->getStr("order");
            if (!o.empty()) order = o;
            if (const JsonValue* sepField = state->find("sep")) {
                if (sepField->type == JsonType::String) sep = sepField->strVal;
            }
        }
        mine = TrimWs(mine);
        std::string other;
        if (const JsonValue* v = refInputs->find("text_in")) {
            other = TrimWs(ResolveTextField(nodesObj, v, preferredKey, depth + 1));
        }
        if (other.empty()) return mine;
        if (mine.empty()) return other;
        return order == "wired" ? (other + sep + mine) : (mine + sep + other);
    }
    if (classType == "orex Cip Text Encode") {
        return ResolveOrexClipTextEncode(nodesObj, refInputs, depth);
    }
    if (classType == "orex Lora Loader") {
        // Only output slot 2 is text: the trigger words of every slot that is
        // on and has "tw_on" set, joined with ", ".
        int slot = -1;
        if (field->arrVal.size() > 1 && field->arrVal[1] && field->arrVal[1]->type == JsonType::Number) {
            slot = (int)ClampDoubleToInt64(field->arrVal[1]->numVal);
        }
        if (slot != 2) return "";
        std::string joined;
        for (const auto& kv : refInputs->objVal) {
            if (kv.first.rfind("lora_", 0) != 0) continue;
            const JsonValue* entry = kv.second.get();
            if (!entry || entry->type != JsonType::Object) continue;
            const JsonValue* onField = entry->find("on");
            if (onField && onField->type == JsonType::Bool && !onField->boolVal) continue;
            const JsonValue* twField = entry->find("tw_on");
            if (!twField || twField->type != JsonType::Bool || !twField->boolVal) continue;
            std::string tw = TrimWs(entry->getStr("trigger_words"));
            // The node's own placeholders ("\xE2\x8F\xB3" is U+23F3 as UTF-8,
            // since sources are compiled without /utf-8).
            if (tw.empty() || tw == "No words (click to add)" || tw == "\xE2\x8F\xB3 Loading...") continue;
            if (!joined.empty()) joined += ", ";
            joined += tw;
        }
        return joined;
    }

    // A "Selector"-style helper reuses the outer field's own name for its
    // literal rather than a generic "text"/"value" key.
    if (preferredKey) {
        if (const JsonValue* v = refInputs->find(preferredKey)) {
            std::string r = ResolveTextField(nodesObj, v, preferredKey, depth + 1);
            if (!r.empty()) return r;
        }
    }
    if (const JsonValue* v = refInputs->find("text")) {
        std::string r = ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        if (!r.empty()) return r;
    }
    if (const JsonValue* v = refInputs->find("value")) {
        std::string r = ResolveTextField(nodesObj, v, preferredKey, depth + 1);
        if (!r.empty()) return r;
    }
    return "";
}

// Numeric counterpart to ResolveTextField. Unlike text nodes, numeric
// literal sources use inconsistent field names ("seed", "int", "value"), so
// this prefers the conventional "value" then falls back to the first
// Number-typed input on the node.
double ResolveNumberField(const JsonValue* nodesObj, const JsonValue* field, bool& found, const char* preferredKey, int depth) {
    found = false;
    if (!field || depth > 4) return 0.0;
    if (field->type == JsonType::Number) { found = true; return field->numVal; }
    if (field->type != JsonType::Array) return 0.0;
    std::string nodeId = GetLinkNodeId(field);
    if (nodeId.empty()) return 0.0;
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return 0.0;
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs) return 0.0;
    // A helper node commonly reuses the outer field's own key for its
    // literal. Prefer it over a numeric sibling ("increment" next to "seed")
    // that the generic fallback would grab first on alphabetical order.
    if (preferredKey) {
        if (const JsonValue* v = refInputs->find(preferredKey)) {
            double r = ResolveNumberField(nodesObj, v, found, preferredKey, depth + 1);
            if (found) return r;
        }
    }
    if (const JsonValue* v = refInputs->find("value")) {
        double r = ResolveNumberField(nodesObj, v, found, preferredKey, depth + 1);
        if (found) return r;
    }
    for (const auto& kv : refInputs->objVal) {
        if (kv.second && kv.second->type == JsonType::Number) { found = true; return kv.second->numVal; }
    }
    return 0.0;
}

// CLIPTextEncodeSDXL splits the prompt into "text_g"/"text_l", usually equal.
// Shown once when equal or one side is empty, "g, l" when they differ.
std::string ResolveClipTextEncodeSdxl(const JsonValue* nodesObj, const JsonValue* refInputs, int depth) {
    std::string g = ResolveTextField(nodesObj, refInputs->find("text_g"), nullptr, depth + 1);
    std::string l = ResolveTextField(nodesObj, refInputs->find("text_l"), nullptr, depth + 1);
    if (g == l || g.empty()) return l;
    if (l.empty()) return g;
    return g + ", " + l;
}

// Mirrors the node: non-empty stripped "stringN" inputs in numeric order,
// then "text", joined with ", ". An empty "text" is skipped rather than
// leaving the node's own trailing separator.
std::string ResolveOrexClipTextEncode(const JsonValue* nodesObj, const JsonValue* refInputs, int depth) {
    // Highest N among keys that are "string" + digits only, then walk 1..N so
    // numeric order falls out without collecting and sorting.
    int maxN = 0;
    for (const auto& kv : refInputs->objVal) {
        const std::string& key = kv.first;
        if (key.size() < 7 || key.size() > 10 || key.compare(0, 6, "string") != 0 ||
            key.find_first_not_of("0123456789", 6) != std::string::npos) continue;
        int n = AImgDecoderInternal::ParseLeadingInt(key.c_str() + 6);
        if (n > maxN) maxN = n;
    }
    std::string joined;
    // 1..maxN are the numbered strings; the final pass (i == maxN + 1) is the
    // node's own "text", which the real node appends last.
    for (int i = 1; i <= maxN + 1; i++) {
        const JsonValue* f = i <= maxN ? refInputs->find(("string" + std::to_string(i)).c_str()) : refInputs->find("text");
        if (!f) continue;
        std::string val = TrimWs(ResolveTextField(nodesObj, f, nullptr, depth + 1));
        if (val.empty()) continue;
        if (!joined.empty()) joined += ", ";
        joined += val;
    }
    return joined;
}

// The inputs of the loader-class node that the sampler's `key` link lands on
// (any output slot), or null.
const JsonValue* LoaderInputsLinkedFrom(const JsonValue* nodesObj, const JsonValue* samplerInputs, const char* key) {
    const JsonValue* f = samplerInputs ? samplerInputs->find(key) : nullptr;
    std::string id = f ? GetLinkNodeId(f) : "";
    auto it = id.empty() ? nodesObj->objVal.end() : nodesObj->objVal.find(id);
    if (it == nodesObj->objVal.end() || !it->second || !IsLoaderHeldPromptClass(it->second->getStr("class_type"))) return nullptr;
    return it->second->getObj("inputs");
}

// True when a sampler's conditioning link ends at a ConditioningZeroOut, directly
// or through the same single-input passthrough nodes ResolveClipText follows.
bool IsZeroedConditioning(const JsonValue* nodesObj, std::string id) {
    for (int hop = 0; hop < 8 && !id.empty(); hop++) {
        auto it = nodesObj->objVal.find(id);
        if (it == nodesObj->objVal.end() || !it->second) return false;
        const std::string ct = it->second->getStr("class_type");
        if (ct == "ConditioningZeroOut") return true;
        const char* key = (ct == "FluxGuidance" || ct == "RBG_Smart_Seed_Variance" || ct == "SeedVarianceEnhancer") ? "conditioning"
                        : ct == "easy cleanGpuUsed" ? "anything" : nullptr;
        const JsonValue* in = it->second->getObj("inputs");
        const JsonValue* v = (key && in) ? in->find(key) : nullptr;
        if (!v) return false;
        id = GetLinkNodeId(v);
    }
    return false;
}

// Resolves the exact node a KSampler's "positive"/"negative" link points at,
// which beats the "first/second text-encode found" guess once a graph has
// more than two (regional prompting, IP-adapters). Some custom nodes expose
// "prompt" and "negative_prompt" on the SAME node instead of a shared
// "text"; isPositive picks between them. depth bounds the passthrough follows.
std::string ResolveClipText(const JsonValue* nodesObj, const std::string& nodeId, bool isPositive, int depth = 0) {
    if (nodeId.empty() || depth > 4) return "";
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return "";
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs) return "";
    std::string classType = it->second->getStr("class_type");
    // "wildcard_text" is the authored template with unexpanded {a|b}
    // alternatives; "populated_text" is what this generation actually used.
    if (classType == "ImpactWildcardEncode") {
        if (const JsonValue* v = refInputs->find("populated_text")) {
            std::string r = ResolveTextField(nodesObj, v);
            if (!r.empty()) return r;
        }
        if (const JsonValue* v = refInputs->find("wildcard_text")) return ResolveTextField(nodesObj, v);
        return "";
    }
    // Both the positive and negative instances of this node name their text
    // field "positive", so isPositive is deliberately ignored.
    if (classType.rfind("WeiLinPromptUI", 0) == 0) {
        if (const JsonValue* v = refInputs->find("positive")) return ResolveTextField(nodesObj, v);
    }
    // FluxGuidance only wraps the conditioning it was given, so follow it one
    // hop -- but never into ConditioningZeroOut: a zeroed negative must
    // resolve to empty, not to the text it was computed from.
    if (classType == "FluxGuidance") {
        if (const JsonValue* v = refInputs->find("conditioning")) {
            std::string targetId = GetLinkNodeId(v);
            if (!targetId.empty()) {
                auto targetIt = nodesObj->objVal.find(targetId);
                if (targetIt != nodesObj->objVal.end() && targetIt->second &&
                    targetIt->second->getStr("class_type") != "ConditioningZeroOut") {
                    return ResolveClipText(nodesObj, targetId, isPositive, depth + 1);
                }
            }
        }
        return "";
    }
    // Same conditioning-wrapper shape as FluxGuidance.
    if (classType == "RBG_Smart_Seed_Variance" || classType == "SeedVarianceEnhancer") {
        if (const JsonValue* v = refInputs->find("conditioning")) {
            std::string targetId = GetLinkNodeId(v);
            if (!targetId.empty()) {
                auto targetIt = nodesObj->objVal.find(targetId);
                if (targetIt != nodesObj->objVal.end() && targetIt->second &&
                    targetIt->second->getStr("class_type") != "ConditioningZeroOut") {
                    return ResolveClipText(nodesObj, targetId, isPositive, depth + 1);
                }
            }
        }
        return "";
    }
    // Same shape again, through the generic "anything" input.
    if (classType == "easy cleanGpuUsed") {
        if (const JsonValue* v = refInputs->find("anything")) {
            std::string targetId = GetLinkNodeId(v);
            if (!targetId.empty()) {
                auto targetIt = nodesObj->objVal.find(targetId);
                if (targetIt != nodesObj->objVal.end() && targetIt->second &&
                    targetIt->second->getStr("class_type") != "ConditioningZeroOut") {
                    return ResolveClipText(nodesObj, targetId, isPositive, depth + 1);
                }
            }
        }
        return "";
    }
    // A tag-concatenating node with three STRING inputs and no "text" field
    // of its own. Real files carry their own trailing punctuation, so join
    // with nothing rather than guessing a delimiter.
    if (classType.find("Get Booru Tag") != std::string::npos) {
        std::string joined;
        for (const char* key : {"text_a", "text_b", "text_c"}) {
            if (const JsonValue* v = refInputs->find(key)) joined += ResolveTextField(nodesObj, v);
        }
        if (!joined.empty()) return joined;
    }
    if (classType == "CLIPTextEncodeSDXL") {
        return ResolveClipTextEncodeSdxl(nodesObj, refInputs, depth);
    }
    if (IsLoaderHeldPromptClass(classType)) {
        // The loader holds the positive/negative itself. A link is tried as
        // plain text first, then as conditioning one hop further, for targets
        // only ResolveClipText's own branches can read.
        const char* key = isPositive ? "positive" : "negative";
        if (const JsonValue* v = refInputs->find(key)) {
            if (v->type == JsonType::String) return v->strVal;
            std::string resolved = ResolveTextField(nodesObj, v);
            if (!resolved.empty()) return resolved;
            std::string targetId = GetLinkNodeId(v);
            if (!targetId.empty()) return ResolveClipText(nodesObj, targetId, isPositive, depth + 1);
        }
        return "";
    }
    if (classType == "orex Cip Text Encode") {
        return ResolveOrexClipTextEncode(nodesObj, refInputs, depth);
    }
    if (const JsonValue* v = refInputs->find("text")) {
        return ResolveTextField(nodesObj, v);
    }
    const char* fallbackKey = isPositive ? "prompt" : "negative_prompt";
    if (const JsonValue* v = refInputs->find(fallbackKey)) {
        return ResolveTextField(nodesObj, v);
    }
    return "";
}

// The first (skip == 0) or (skip+1)-th STRING in a node's widgets_values.
// Stock nodes hold the prompt at index 0, but some prepend a metadata object,
// so the first string element is right either way. skip == 1 serves a node
// exposing two CONDITIONING outputs from two text fields, where both roles
// would otherwise resolve to the identical first string.
std::string FirstWidgetString(const JsonValue* node, int skip = 0) {
    const JsonValue* wv = node ? node->find("widgets_values") : nullptr;
    if (!wv || wv->type != JsonType::Array) return "";
    for (const auto& v : wv->arrVal) {
        // A rich-prompt-editor node serializes its editor state as JSON into
        // its own widgets_values. STOP here rather than scanning on: that
        // blob IS this node's prompt state, so a string following it (a
        // session id, say) is unrelated settings data, not a second guess.
        // Different from a non-string entry, which just isn't the text and is
        // fine to skip past.
        if (v && v->type == JsonType::String && !v->strVal.empty() && LooksLikeSerializedTextBlob(v->strVal)) return "";
        if (v && v->type == JsonType::String && !v->strVal.empty()) {
            if (skip > 0) { skip--; continue; }
            return v->strVal;
        }
    }
    return "";
}

// A widget-backed input's value looked up BY NAME, for nodes carrying more
// than one text field where FirstWidgetString's positional guess can't tell
// them apart. widgets_values holds an entry only for widget-backed inputs
// (an "inputs" entry with a "widget" key), in declaration order, so the Nth
// such input maps onto widgets_values[N].
std::string NamedWidgetString(const JsonValue* node, const std::string& widgetName) {
    if (!node || node->type != JsonType::Object) return "";
    const JsonValue* inputsField = node->find("inputs");
    const JsonValue* wvField = node->find("widgets_values");
    if (!inputsField || inputsField->type != JsonType::Array) return "";
    if (!wvField || wvField->type != JsonType::Array) return "";
    const auto& wArr = wvField->arrVal;
    size_t widgetIndex = 0;
    for (const auto& inp : inputsField->arrVal) {
        if (!inp || inp->type != JsonType::Object || !inp->find("widget")) continue;
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

// UI-format node/link index, the counterpart to ResolveClipText: here
// "inputs" is an array of {name, type, link} objects pointing into a separate
// "links" array, so a KSampler's positive/negative CONDITIONING can only be
// resolved through an up-front index. Without it, a graph with more than one
// text-encode node falls back to the "first found = positive" guess and can
// silently swap prompt and negative_prompt.
//
// Node and link ids are unique only WITHIN one scope -- each subgraph
// definition renumbers its own from scratch, so two definitions in one
// workflow can reuse the same small ids. Every lookup is therefore keyed by
// "scopeId\x1f<id>", scopeId being "" for the top-level graph.
//
// BoundarySlot describes one promoted-widget boundary link: widgetIndex
// indexes the instantiating node's widgets_values[], and inputName is that
// boundary input's own name from the subgraph's "inputs[]" at the link's RAW
// origin_slot. The name is what finds the matching input on the instance
// node, which lists only a subset of the boundary inputs and not in boundary
// order -- so that lookup must go by name, never positionally.
struct BoundarySlot {
    int widgetIndex = -1;
    std::string inputName;
};

struct UiLinkIndex {
    // std::map for the same reason as JsonValue::objVal: small N, and one
    // container backend rather than both tree and hash template machinery.
    std::map<std::string, const JsonValue*> nodesById;
    std::map<std::string, std::string> linkOrigin;
    // A link originating at the subgraph's boundary-input sentinel maps here
    // instead of linkOrigin. Without it, a promoted widget resolves to the
    // value frozen on the inner node when its widget became a socket, not
    // the one actually used.
    std::map<std::string, BoundarySlot> boundarySlot;
};

// reserve+append rather than operator+ chaining: one allocation instead of
// two, called once per node and link of every UI-format graph.
std::string ScopedKey(const std::string& scopeId, const std::string& id) {
    std::string key;
    key.reserve(scopeId.size() + 1 + id.size());
    key.append(scopeId);
    key.push_back('\x1f');
    key.append(id);
    return key;
}

// A subgraph definition plus, heuristically, its instantiating node. A
// definition can be instantiated more than once, but the serialized JSON
// gives no way to tell which instance an internal node copy belongs to, so
// the FIRST top-level instance found wins. Correct for nearly every real
// workflow; two instances of one subgraph could misattribute values.
struct SubgraphInfo {
    const JsonValue* def = nullptr;
    const JsonValue* instanceNode = nullptr;
};
using SubgraphRegistry = std::map<std::string, SubgraphInfo>;

// A link id to its origin node, or nullptr if either is unknown.
const JsonValue* ResolveLinkNode(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index) {
    auto it = index.linkOrigin.find(ScopedKey(scopeId, std::to_string(linkId)));
    if (it == index.linkOrigin.end()) return nullptr;
    auto nodeIt = index.nodesById.find(it->second);
    if (nodeIt == index.nodesById.end()) return nullptr;
    return nodeIt->second;
}

// NotBoundary: not a promoted-widget crossing; the caller falls back as
// usual. Resolved: a real current value was found (or legitimately isn't
// there). Unresolvable: this IS a promoted widget whose real value cannot be
// recovered from the file -- callers MUST leave the field empty rather than
// fall back to any stale local copy.
enum class BoundaryOutcome { NotBoundary, Resolved, Unresolvable };

// A replacement wire is trusted only when it lands on a plain
// literal-carrying primitive node; anything else -- a computed chain, a
// Reroute, an unknown custom node -- means the value is unrecoverable. Both
// wired-widget paths below follow their wire exactly one hop and consult
// this same short list.
bool LiteralNodeValue(const JsonValue* origin, const JsonValue*& outValue) {
    outValue = nullptr;
    std::string originType = origin ? origin->getStr("type") : "";
    static const char* const kLiteralTypes[] = {
        "PrimitiveString", "PrimitiveStringMultiline", "Text Multiline",
        "PrimitiveBoolean", "PrimitiveFloat", "PrimitiveInt", "PrimitiveInteger"
    };
    bool isLiteral = false;
    for (const char* t : kLiteralTypes) { if (originType == t) { isLiteral = true; break; } }
    if (!isLiteral) return false;
    const JsonValue* wvField = origin->find("widgets_values");
    if (!wvField || wvField->type != JsonType::Array || wvField->arrVal.empty() || !wvField->arrVal[0]) {
        return false;
    }
    outValue = wvField->arrVal[0].get();
    return true;
}

// Resolves a subgraph-boundary crossing to the real, current value. The
// instance node's widgets_values is trustworthy only while the instance's own
// matching input -- found BY NAME via BoundarySlot::inputName, never
// positionally -- is unwired. A wired instance input is just as stale as the
// inner node's, frozen when ITS widget became a socket, and the real value
// comes from whatever feeds that wire in the outer graph: followed one hop
// (always in scope "", since instanceNode only ever comes from the top-level
// nodes[]) and accepted only for a plain literal carrier. Anything else -- a
// computed chain, a nested instance, an unknown node -- is Unresolvable.
// Deliberately NOT routed through ResolveUiTextThroughLink: that returns a
// std::string, and this also serves numbers and booleans. Blank is the safe
// direction for a case this narrow.
BoundaryOutcome ResolveBoundaryValue(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                      const SubgraphRegistry& registry, const JsonValue*& outValue) {
    outValue = nullptr;
    auto slotIt = index.boundarySlot.find(ScopedKey(scopeId, std::to_string(linkId)));
    if (slotIt == index.boundarySlot.end()) return BoundaryOutcome::NotBoundary;
    auto regIt = registry.find(scopeId);
    if (regIt == registry.end() || !regIt->second.instanceNode) return BoundaryOutcome::NotBoundary;
    const JsonValue* inst = regIt->second.instanceNode;
    const BoundarySlot& slot = slotIt->second;

    const JsonValue* instLinkField = nullptr;
    if (!slot.inputName.empty()) {
        if (const JsonValue* instInputs = inst->find("inputs")) {
            if (instInputs->type == JsonType::Array) {
                for (const auto& inp : instInputs->arrVal) {
                    if (!inp || inp->type != JsonType::Object) continue;
                    if (inp->getStr("name") != slot.inputName) continue;
                    instLinkField = inp->find("link");
                    break;
                }
            }
        }
    }
    if (instLinkField && instLinkField->type == JsonType::Number) {
        // The instance's own input is wired, so its widgets_values copy is
        // stale too. One hop, literal carriers only.
        const JsonValue* origin = ResolveLinkNode("", ClampDoubleToInt64(instLinkField->numVal), index);
        return LiteralNodeValue(origin, outValue) ? BoundaryOutcome::Resolved : BoundaryOutcome::Unresolvable;
    }

    // Instance input absent or unwired: its widgets_values is current.
    const JsonValue* wvField = inst->find("widgets_values");
    if (!wvField || wvField->type != JsonType::Array) return BoundaryOutcome::Resolved;
    const auto& wArr = wvField->arrVal;
    int idx = slot.widgetIndex;
    if (idx < 0 || idx >= (int)wArr.size() || !wArr[idx]) return BoundaryOutcome::Resolved;
    outValue = wArr[idx].get();
    return BoundaryOutcome::Resolved;
}

// The stale-wired-widget rule, which is NOT specific to subgraph boundaries:
// a "widget" key on an input means that widget became a socket, so its local
// widgets_values entry is a frozen leftover regardless of scope. An ordinary
// in-scope wired widget (a loader in the top-level graph wired to a computed
// node) falls through ResolveBoundaryValue as NotBoundary, and callers then
// read that stale value as current. This tries the boundary path first,
// preserving its by-name instance lookup when the link IS a crossing, and
// otherwise follows the in-scope link one hop, trusting only the same
// literal carriers; anything else is Unresolvable, never a silent
// NotBoundary.
//
// Deliberately NOT used by FirstPromotedWidgetValue: a wired TEXT widget can
// still be recovered through ResolveUiTextThroughLink's full chain
// traversal, which this narrow one-hop rule would pre-empt with a blank.
BoundaryOutcome ResolveWiredWidgetValue(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                         const SubgraphRegistry& registry, const JsonValue*& outValue) {
    BoundaryOutcome outcome = ResolveBoundaryValue(scopeId, linkId, index, registry, outValue);
    if (outcome != BoundaryOutcome::NotBoundary) return outcome;
    const JsonValue* origin = ResolveLinkNode(scopeId, linkId, index);
    if (!origin) return BoundaryOutcome::Unresolvable;
    return LiteralNodeValue(origin, outValue) ? BoundaryOutcome::Resolved : BoundaryOutcome::Unresolvable;
}

// Scans a node's OWN inputs[] for a widget-backed entry whose link crosses a
// subgraph boundary, and reports the outcome for whichever crossing matches
// the wanted JSON type. Distinct from TryBoundaryString/TryBoundaryNumber
// below, which address an input BY NAME: here the caller has only the node
// and wants whichever promoted widget matches a type. Unresolvable
// propagates immediately -- the caller must not fall back to a local read.
BoundaryOutcome FirstPromotedWidgetValue(const JsonValue* node, const std::string& scopeId,
                                          const UiLinkIndex& index, const SubgraphRegistry& registry,
                                          JsonType wanted, const JsonValue*& outValue) {
    outValue = nullptr;
    if (!node || node->type != JsonType::Object) return BoundaryOutcome::NotBoundary;
    const JsonValue* inputsField = node->find("inputs");
    if (!inputsField || inputsField->type != JsonType::Array) return BoundaryOutcome::NotBoundary;
    for (const auto& inp : inputsField->arrVal) {
        if (!inp || inp->type != JsonType::Object) continue;
        if (!inp->find("widget")) continue;
        const JsonValue* linkField = inp->find("link");
        if (!linkField || linkField->type != JsonType::Number) continue;
        const JsonValue* v = nullptr;
        BoundaryOutcome outcome = ResolveBoundaryValue(scopeId, ClampDoubleToInt64(linkField->numVal), index, registry, v);
        if (outcome == BoundaryOutcome::NotBoundary) continue;
        if (outcome == BoundaryOutcome::Unresolvable) return BoundaryOutcome::Unresolvable;
        if (v && v->type == wanted) { outValue = v; return BoundaryOutcome::Resolved; }
    }
    return BoundaryOutcome::NotBoundary;
}

void CollectUiNodesAndLinks(const JsonValue* scope, const std::string& scopeId, UiLinkIndex& index) {
    if (!scope || scope->type != JsonType::Object) return;
    if (const JsonValue* nodesField = scope->find("nodes")) {
        if (nodesField->type == JsonType::Array) {
            for (const auto& n : nodesField->arrVal) {
                if (!n || n->type != JsonType::Object) continue;
                const JsonValue* idField = n->find("id");
                if (!idField) continue;
                std::string idStr = JsonIdToString(idField);
                if (!idStr.empty()) index.nodesById[ScopedKey(scopeId, idStr)] = n.get();
            }
        }
    }
    // The boundary-input sentinel id: links from there are promoted-widget
    // crossings, not ordinary node-to-node links. Absent at top level.
    std::string boundaryInputId;
    if (const auto* inputNode = scope->getObj("inputNode")) {
        if (const JsonValue* idField = inputNode->find("id")) boundaryInputId = JsonIdToString(idField);
    }
    // Collected here, resolved to a widgets_values index in a second pass
    // below once every link in the scope is known.
    struct BoundaryLink { std::string linkKey; int originSlot; std::string targetId; int targetSlot; };
    std::vector<BoundaryLink> boundaryLinks;
    const JsonValue* linksField = scope->find("links");
    if (linksField && linksField->type == JsonType::Array) {
        for (const auto& l : linksField->arrVal) {
            if (!l) continue;
            int64_t linkId = 0;
            std::string originId, targetId;
            int originSlot = -1, targetSlot = -1;
            if (l->type == JsonType::Array) {
                // Classic tuple form: [link_id, origin_id, origin_slot,
                // target_id, target_slot, type].
                if (l->arrVal.size() < 2 || !l->arrVal[0] || l->arrVal[0]->type != JsonType::Number || !l->arrVal[1]) continue;
                linkId = ClampDoubleToInt64(l->arrVal[0]->numVal);
                originId = JsonIdToString(l->arrVal[1].get());
                if (l->arrVal.size() > 2) originSlot = JsonSlotIndex(l->arrVal[2].get());
                if (l->arrVal.size() > 3 && l->arrVal[3]) targetId = JsonIdToString(l->arrVal[3].get());
                if (l->arrVal.size() > 4) targetSlot = JsonSlotIndex(l->arrVal[4].get());
            } else if (l->type == JsonType::Object) {
                // Subgraph-capable versions store each link as an object
                // instead. Unhandled, every link in such a workflow is
                // dropped and no link resolution fires at all.
                const JsonValue* idv = l->find("id");
                const JsonValue* originv = l->find("origin_id");
                if (!idv || idv->type != JsonType::Number || !originv) continue;
                linkId = ClampDoubleToInt64(idv->numVal);
                originId = JsonIdToString(originv);
                originSlot = JsonSlotIndex(l->find("origin_slot"));
                if (const JsonValue* targetIdField = l->find("target_id")) targetId = JsonIdToString(targetIdField);
                targetSlot = JsonSlotIndex(l->find("target_slot"));
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
        // A boundary "inputs[]" mixes socket-only entries (IMAGE/MODEL/
        // CLIP/...) with promoted widgets (STRING/INT/...), and ONLY the
        // latter get a widgets_values entry -- so origin_slot is not a
        // widgets_values index. With a socket-only IMAGE at input 0 and a
        // promoted "prompt" at input 1, the prompt sits at widgets_values[0].
        // Indexing by raw origin_slot grabs the wrong slot, fails the
        // caller's type check, and falls back to the inner node's stale
        // value. So: decide widget-backed-ness per distinct origin_slot from
        // whether that slot's target has a "widget" key, then number the
        // widget-backed slots sequentially, skipping socket-only ones.
        std::map<int, bool> slotIsWidget;
        for (const auto& bl : boundaryLinks) {
            if (slotIsWidget.count(bl.originSlot)) continue;
            bool isWidget = false;
            if (!bl.targetId.empty() && bl.targetSlot >= 0) {
                auto nodeIt = index.nodesById.find(ScopedKey(scopeId, bl.targetId));
                const JsonValue* targetInputs = nodeIt != index.nodesById.end() ? nodeIt->second->find("inputs") : nullptr;
                if (targetInputs && targetInputs->type == JsonType::Array) {
                    const auto& inputs = targetInputs->arrVal;
                    if (bl.targetSlot < (int)inputs.size() && inputs[bl.targetSlot] &&
                        inputs[bl.targetSlot]->type == JsonType::Object) {
                        isWidget = inputs[bl.targetSlot]->find("widget") != nullptr;
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
        // The name comes from the definition's own "inputs[]" at the RAW
        // origin_slot, not the remapped widget index -- two different things.
        const JsonValue* boundaryInputsField = scope->find("inputs");
        for (const auto& bl : boundaryLinks) {
            auto it = slotToWidgetIndex.find(bl.originSlot);
            if (it == slotToWidgetIndex.end()) continue;
            BoundarySlot slot;
            slot.widgetIndex = it->second;
            if (boundaryInputsField && boundaryInputsField->type == JsonType::Array &&
                bl.originSlot >= 0 && bl.originSlot < (int)boundaryInputsField->arrVal.size() &&
                boundaryInputsField->arrVal[bl.originSlot]) {
                slot.inputName = boundaryInputsField->arrVal[bl.originSlot]->getStr("name");
            }
            index.boundarySlot[bl.linkKey] = slot;
        }
    }
}

// A named input's "link" id. False when the input is absent or unwired, the
// common case for a widget never converted to a socket. outIsWidget reports
// whether the matched input carries a "widget" key -- what makes the node's
// own widgets_values entry for it stale (see ResolveWiredWidgetValue).
bool GetNodeInputLink(const JsonValue* node, const std::string& name, int64_t& outLinkId, bool* outIsWidget = nullptr) {
    if (!node || node->type != JsonType::Object) return false;
    const JsonValue* inputsField = node->find("inputs");
    if (!inputsField || inputsField->type != JsonType::Array) return false;
    for (const auto& inp : inputsField->arrVal) {
        if (!inp || inp->type != JsonType::Object) continue;
        if (inp->getStr("name") != name) continue;
        const JsonValue* linkField = inp->find("link");
        if (!linkField || linkField->type != JsonType::Number) return false;
        outLinkId = ClampDoubleToInt64(linkField->numVal);
        if (outIsWidget) *outIsWidget = (inp->find("widget") != nullptr);
        return true;
    }
    return false;
}

std::string ResolveUiTextThroughLink(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                      const SubgraphRegistry& registry, int depth);

// A boolean-valued link to its literal, following PrimitiveBoolean nodes.
// -1 when undeterminable, so callers can fall back to a local default.
int ResolveUiBoolLink(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                       const SubgraphRegistry& registry, int depth) {
    if (depth > 6) return -1;
    // The link itself can be a boundary crossing, with no intermediate proxy
    // node. Unresolvable must return -1 directly, never fall through to a
    // local widgets_values read.
    const JsonValue* boundaryVal = nullptr;
    BoundaryOutcome outcome = ResolveBoundaryValue(scopeId, linkId, index, registry, boundaryVal);
    if (outcome == BoundaryOutcome::Unresolvable) return -1;
    if (outcome == BoundaryOutcome::Resolved && boundaryVal && boundaryVal->type == JsonType::Bool) {
        return boundaryVal->boolVal ? 1 : 0;
    }
    const JsonValue* node = ResolveLinkNode(scopeId, linkId, index);
    if (!node) return -1;
    // More commonly the link lands on a proxy node whose own "value" widget
    // is what was promoted, making its local widgets_values[0] stale.
    const JsonValue* promotedVal = nullptr;
    BoundaryOutcome promotedOutcome = FirstPromotedWidgetValue(node, scopeId, index, registry, JsonType::Bool, promotedVal);
    if (promotedOutcome == BoundaryOutcome::Unresolvable) return -1;
    if (promotedOutcome == BoundaryOutcome::Resolved && promotedVal) {
        return promotedVal->boolVal ? 1 : 0;
    }
    const JsonValue* wvField = node->find("widgets_values");
    if (!wvField || wvField->type != JsonType::Array) return -1;
    const auto& w = wvField->arrVal;
    if (!w.empty() && w[0] && w[0]->type == JsonType::Bool) return w[0]->boolVal ? 1 : 0;
    return -1;
}

// Resolves a pass-through / prompt-routing node's text from the node itself
// rather than a link to it, so ResolveUiLinkText -- which already holds the
// origin node -- can share the same safe dispatch instead of falling to
// FirstWidgetString(). That matters because blindly taking a join node's
// first string widget grabs its own delimiter (", ") rather than the text it
// passes through. Returns "" for any type not recognized here.
std::string ResolveNodeTextByType(const JsonValue* node, const std::string& scopeId, const UiLinkIndex& index,
                                   const SubgraphRegistry& registry, int depth) {
    if (!node) return "";
    std::string typeStr = node->getStr("type");

    if (typeStr == "ComfySwitchNode") {
        int switchVal = -1;
        int64_t switchLink = 0;
        if (GetNodeInputLink(node, "switch", switchLink)) switchVal = ResolveUiBoolLink(scopeId, switchLink, index, registry, depth + 1);
        if (switchVal == -1) {
            const JsonValue* wvField = node->find("widgets_values");
            if (wvField && wvField->type == JsonType::Array) {
                const auto& w = wvField->arrVal;
                if (!w.empty() && w[0] && w[0]->type == JsonType::Bool) switchVal = w[0]->boolVal ? 1 : 0;
            }
        }
        int64_t branchLink = 0;
        // Prefer the branch the condition selects, then try both: the
        // selected one may flow through a node whose output is computed at
        // runtime and never stored, i.e. unrecoverable.
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
        if (const JsonValue* wvField = node->find("widgets_values")) {
            if (wvField->type == JsonType::Array) {
                const auto& w = wvField->arrVal;
                if (w.size() > 2 && w[2] && w[2]->type == JsonType::String) sep = w[2]->strVal;
            }
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
        const JsonValue* inputsField = node->find("inputs");
        if (inputsField && inputsField->type == JsonType::Array && !inputsField->arrVal.empty()) {
            const auto& inp0 = inputsField->arrVal[0];
            if (inp0 && inp0->type == JsonType::Object) {
                const JsonValue* linkField = inp0->find("link");
                if (linkField && linkField->type == JsonType::Number) {
                    return ResolveUiTextThroughLink(scopeId, ClampDoubleToInt64(linkField->numVal), index, registry, depth + 1);
                }
            }
        }
        return "";
    }

    if (typeStr == "RegexReplace") {
        // As in ResolveTextField: the literal input is named "string", and
        // the pre-replace text passes through unmodified.
        int64_t srcLink;
        if (GetNodeInputLink(node, "string", srcLink)) {
            return ResolveUiTextThroughLink(scopeId, srcLink, index, registry, depth + 1);
        }
        return "";
    }

    if (typeStr == "Flux_Finish_StylesStyler") {
        std::string positive = NamedWidgetString(node, "text_positive");
        if (!positive.empty()) return positive;
        return NamedWidgetString(node, "text_negative");
    }

    if (typeStr == "ImpactWildcardProcessor") {
        // As in ResolveTextField: "populated_text" is what was actually
        // used, "wildcard_text" the authored template.
        std::string populated = NamedWidgetString(node, "populated_text");
        if (!populated.empty()) return populated;
        return NamedWidgetString(node, "wildcard_text");
    }

    if (typeStr == "Text Concatenate") {
        // "Text Concatenate": four STRING inputs joined with a delimiter.
        // Unlike StringConcatenate, EVERY text_* input can be a socket, so
        // widgets_values may hold only [delimiter, clean_whitespace] with no
        // text at all -- FirstWidgetString() would return the delimiter.
        std::string sep;
        if (const JsonValue* wvField = node->find("widgets_values")) {
            if (wvField->type == JsonType::Array) {
                const auto& w = wvField->arrVal;
                if (!w.empty() && w[0] && w[0]->type == JsonType::String) sep = w[0]->strVal;
            }
        }
        std::string joined;
        for (const char* inName : {"text_a", "text_b", "text_c", "text_d"}) {
            int64_t partLink;
            if (!GetNodeInputLink(node, inName, partLink)) continue;
            std::string part = ResolveUiTextThroughLink(scopeId, partLink, index, registry, depth + 1);
            if (part.empty()) continue;
            if (!joined.empty()) joined += sep;
            joined += part;
        }
        return joined;
    }

    // Only node types whose entire purpose is to carry a fixed literal are
    // trusted here. FirstWidgetString() on ANY node is unsafe: a node whose
    // real output is computed at runtime carries no literal text, but its
    // widgets_values still holds string settings (an "on"/"off" toggle), and
    // taking the first produces a confidently wrong one-word "prompt".
    if (typeStr == "PrimitiveString" || typeStr == "PrimitiveStringMultiline" || typeStr == "Text Multiline") {
        // This node's own literal widget may itself be promoted to the
        // enclosing subgraph's boundary, making its local widgets_values
        // stale. ONLY a boundary crossing is followed: an ordinary linked
        // widget is left alone, since this node type's contract is to carry
        // a literal, not route one. Unresolvable must return "" outright, not
        // fall back to FirstWidgetString() and surface the stale value.
        const JsonValue* boundaryVal = nullptr;
        BoundaryOutcome outcome = FirstPromotedWidgetValue(node, scopeId, index, registry, JsonType::String, boundaryVal);
        if (outcome == BoundaryOutcome::Unresolvable) return "";
        if (outcome == BoundaryOutcome::Resolved && boundaryVal) return boundaryVal->strVal;
        return FirstWidgetString(node);
    }
    return "";
}

// Tells "this IS a join/passthrough node whose inputs resolved empty" (keep
// the blank) apart from "this is a node type not recognized at all" (fall
// back to FirstWidgetString, the ordinary literal case). Without the
// distinction, a join node whose upstream text couldn't be resolved falls
// through and yields its own delimiter or toggle widget.
bool IsKnownPassthroughNodeType(const std::string& typeStr) {
    return typeStr == "ComfySwitchNode" || typeStr == "StringConcatenate" || typeStr == "Text Concatenate" ||
           typeStr == "Reroute" || typeStr == "PreviewAny" || typeStr == "RegexReplace";
}

// Follows a "text" input's link through pass-through nodes to the literal
// feeding it; bounded depth guards against cycles in malformed graphs. Checks
// for a subgraph-boundary crossing FIRST: a promoted text widget's real value
// lives on the instantiating node, not on the node the link nominally points
// at, and Unresolvable returns "" rather than resolving that nominal node.
std::string ResolveUiTextThroughLink(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                                      const SubgraphRegistry& registry, int depth) {
    if (depth > 6) return "";
    const JsonValue* boundaryVal = nullptr;
    BoundaryOutcome outcome = ResolveBoundaryValue(scopeId, linkId, index, registry, boundaryVal);
    if (outcome == BoundaryOutcome::Unresolvable) return "";
    if (outcome == BoundaryOutcome::Resolved) {
        return (boundaryVal && boundaryVal->type == JsonType::String) ? boundaryVal->strVal : "";
    }
    const JsonValue* node = ResolveLinkNode(scopeId, linkId, index);
    return ResolveNodeTextByType(node, scopeId, index, registry, depth);
}

// A KSampler's "positive"/"negative" CONDITIONING link to the origin node's
// text. That node's own "text" (or "prompt"/"negative_prompt" on a dual-role
// node) may itself be linked, so this resolves through
// ResolveUiTextThroughLink rather than taking FirstWidgetString(), which
// would prefer a stale widget over the real value whenever one is linked.
std::string ResolveUiLinkText(const std::string& scopeId, int64_t linkId, const UiLinkIndex& index,
                               const SubgraphRegistry& registry, bool wantNegative, int skip = 0) {
    const JsonValue* node = ResolveLinkNode(scopeId, linkId, index);
    if (!node) return "";
    int64_t fieldLink;
    bool namedInputWired = false;
    if (GetNodeInputLink(node, "text", fieldLink)) {
        namedInputWired = true;
        std::string t = ResolveUiTextThroughLink(scopeId, fieldLink, index, registry, 0);
        if (!t.empty()) return t;
    }
    const char* namedField = wantNegative ? "negative_prompt" : "prompt";
    if (GetNodeInputLink(node, namedField, fieldLink)) {
        namedInputWired = true;
        std::string t = ResolveUiTextThroughLink(scopeId, fieldLink, index, registry, 0);
        if (!t.empty()) return t;
    }
    // The origin may be a join/passthrough node wired straight into the
    // sampler with no CLIPTextEncode in between -- use the same safe dispatch
    // before any FirstWidgetString() fallback.
    std::string passthrough = ResolveNodeTextByType(node, scopeId, index, registry, 0);
    if (!passthrough.empty()) return passthrough;
    if (IsKnownPassthroughNodeType(node->getStr("type"))) return "";
    // The chain traversal above IS the recovery for a wired text input, so
    // when it comes back empty the recovery has FAILED and the ordinary
    // stale-wired-widget rule applies again: that input's own widget value is
    // a frozen leftover, and surfacing it would show the authoring-time
    // template instead of an honest blank.
    if (namedInputWired) return "";
    return FirstWidgetString(node, skip);
}

// Positional offsets into widgets_values for node types carrying generation
// parameters at fixed indices. -1 means "no widget for this field"; minSize
// is the shortest widgets_values that may be trusted.
struct WidgetFieldMap {
    int minSize;
    int seedIdx, stepsIdx, cfgIdx, denoiseIdx, samplerIdx, schedulerIdx;
};

// A flat array scanned linearly, not a std::map: seven rows of compile-time
// data reached from one call site. A string-keyed tree would instantiate a
// whole specialization, heap-allocate every key at static-init time and carry
// a thread-safe-init guard, all to search seven short strings.
//
// The per-row widget-order comments are the only defence against a silently
// wrong index; do not drop them.
struct WidgetFieldMapRow {
    const char* name;
    WidgetFieldMap map;
};

static const WidgetFieldMapRow kWidgetFieldMaps[] = {
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

static const WidgetFieldMap* FindWidgetFieldMap(const std::string& typeStr) {
    for (const auto& row : kWidgetFieldMaps) {
        if (typeStr == row.name) return &row.map;
    }
    return nullptr;
}

// Resolves a node's NAMED input through the boundary / wired-widget
// machinery before any positional widgets_values fallback. A sampler's
// seed/steps/cfg widget can be promoted to a boundary or simply wired to a
// computed node in the same scope, exactly as a "text" input can; either way
// its local widget value is a frozen leftover. outUnresolvable means the
// wiring exists but the value is unrecoverable -- the caller must then skip
// its positional fallback FOR THAT FIELD, which is different from this
// returning false for any other reason.
bool TryBoundaryNumber(const JsonValue* node, const char* inputName, const std::string& scopeId,
                       const UiLinkIndex& index, const SubgraphRegistry& registry, double& outNum,
                       bool& outUnresolvable) {
    outUnresolvable = false;
    int64_t linkId;
    bool isWidget = false;
    if (!GetNodeInputLink(node, inputName, linkId, &isWidget)) return false;
    const JsonValue* v = nullptr;
    // A genuine socket (no "widget" key) has no widgets_values entry to go
    // stale, so it stays on the boundary-only path.
    BoundaryOutcome outcome = isWidget ? ResolveWiredWidgetValue(scopeId, linkId, index, registry, v)
                                        : ResolveBoundaryValue(scopeId, linkId, index, registry, v);
    if (outcome == BoundaryOutcome::Unresolvable) { outUnresolvable = true; return false; }
    if (outcome != BoundaryOutcome::Resolved || !v || v->type != JsonType::Number) return false;
    outNum = v->numVal;
    return true;
}

bool TryBoundaryString(const JsonValue* node, const char* inputName, const std::string& scopeId,
                        const UiLinkIndex& index, const SubgraphRegistry& registry, std::string& outStr,
                        bool& outUnresolvable) {
    outUnresolvable = false;
    int64_t linkId;
    bool isWidget = false;
    if (!GetNodeInputLink(node, inputName, linkId, &isWidget)) return false;
    const JsonValue* v = nullptr;
    BoundaryOutcome outcome = isWidget ? ResolveWiredWidgetValue(scopeId, linkId, index, registry, v)
                                        : ResolveBoundaryValue(scopeId, linkId, index, registry, v);
    if (outcome == BoundaryOutcome::Unresolvable) { outUnresolvable = true; return false; }
    if (outcome != BoundaryOutcome::Resolved || !v || v->type != JsonType::String) return false;
    outStr = v->strVal;
    return true;
}

void ApplyWidgetFieldMap(const JsonValue* node, const WidgetFieldMap& m, const std::vector<std::unique_ptr<JsonValue>>& wArr,
                          const std::string& scopeId, const UiLinkIndex& index, const SubgraphRegistry& registry,
                          AImgInfo& info, std::string& samplerName, std::string& schedulerName) {
    double num;
    bool unresolvable = false;
    // Tracked per field so the positional fallback below is skipped for just
    // that one: a field driven by an outer computed link must stay EMPTY,
    // never fall back to a stale local widget.
    bool seedUnresolvable = false, stepsUnresolvable = false, cfgUnresolvable = false, denoiseUnresolvable = false;
    bool samplerUnresolvable = false, schedulerUnresolvable = false;

    if (!info.has_seed) {
        if (TryBoundaryNumber(node, "seed", scopeId, index, registry, num, unresolvable)) {
            info.seed = ClampDoubleToInt64(num); info.has_seed = true;
        } else {
            seedUnresolvable = unresolvable;
            if (TryBoundaryNumber(node, "noise_seed", scopeId, index, registry, num, unresolvable)) {
                info.seed = ClampDoubleToInt64(num); info.has_seed = true;
            } else if (unresolvable) {
                seedUnresolvable = true;
            }
        }
    }
    if (!info.has_steps && TryBoundaryNumber(node, "steps", scopeId, index, registry, num, unresolvable)) {
        info.steps = (int32_t)ClampDoubleToInt64(num); info.has_steps = true;
    } else if (!info.has_steps) {
        stepsUnresolvable = unresolvable;
    }
    if (!info.has_cfg && TryBoundaryNumber(node, "cfg", scopeId, index, registry, num, unresolvable)) {
        info.cfg_scale = num; info.has_cfg = true;
    } else if (!info.has_cfg) {
        cfgUnresolvable = unresolvable;
    }
    if (!info.has_denoising_strength && TryBoundaryNumber(node, "denoise", scopeId, index, registry, num, unresolvable)) {
        info.denoising_strength = num; info.has_denoising_strength = true;
    } else if (!info.has_denoising_strength) {
        denoiseUnresolvable = unresolvable;
    }
    std::string str;
    if (samplerName.empty()) {
        if (TryBoundaryString(node, "sampler_name", scopeId, index, registry, str, unresolvable)) samplerName = str;
        else samplerUnresolvable = unresolvable;
    }
    if (schedulerName.empty()) {
        if (TryBoundaryString(node, "scheduler", scopeId, index, registry, str, unresolvable)) schedulerName = str;
        else schedulerUnresolvable = unresolvable;
    }

    if ((int)wArr.size() < m.minSize) return;
    auto numAt = [&](int idx) -> const JsonValue* {
        if (idx < 0 || idx >= (int)wArr.size() || !wArr[idx]) return nullptr;
        return wArr[idx]->type == JsonType::Number ? wArr[idx].get() : nullptr;
    };
    auto strAt = [&](int idx) -> const JsonValue* {
        if (idx < 0 || idx >= (int)wArr.size() || !wArr[idx]) return nullptr;
        return wArr[idx]->type == JsonType::String ? wArr[idx].get() : nullptr;
    };
    if (!info.has_seed && !seedUnresolvable) { if (const auto* v = numAt(m.seedIdx)) { info.seed = ClampDoubleToInt64(v->numVal); info.has_seed = true; } }
    if (!info.has_steps && !stepsUnresolvable) { if (const auto* v = numAt(m.stepsIdx)) { info.steps = (int32_t)ClampDoubleToInt64(v->numVal); info.has_steps = true; } }
    if (!info.has_cfg && !cfgUnresolvable) { if (const auto* v = numAt(m.cfgIdx)) { info.cfg_scale = v->numVal; info.has_cfg = true; } }
    if (!info.has_denoising_strength && !denoiseUnresolvable) { if (const auto* v = numAt(m.denoiseIdx)) { info.denoising_strength = v->numVal; info.has_denoising_strength = true; } }
    if (samplerName.empty() && !samplerUnresolvable) { if (const auto* v = strAt(m.samplerIdx)) samplerName = v->strVal; }
    if (schedulerName.empty() && !schedulerUnresolvable) { if (const auto* v = strAt(m.schedulerIdx)) schedulerName = v->strVal; }
}

// Shared state across the API-format and UI-format traversals, so each pass
// only fills gaps the other left, without threading a half-dozen out-params
// through TraverseUiNodes.
struct ComfyUiExtraction {
    std::string posPromptText, negPromptText;
    std::string modelName, vaeName, samplerName, schedulerName;
    std::string uiResolvedPos, uiResolvedNeg;
    // Any wired "positive" input seen, resolved or not; feeds the
    // unresolvable-positive guard in DecodeComfyUI.
    bool uiPosLinkSeen = false;
    // A fallback only: the API-format graph is authoritative when present.
    std::vector<std::string> loras;
};

// "mode" is an editor run-state flag: 0 = enabled (default when absent),
// 2 = muted, 4 = bypassed. Unlike the API format, which already excludes
// disabled nodes at queue time, the UI format keeps everything the editor
// last had open -- a prior edit step, an alternate draft branch, an
// experiment helper -- and none of it may contribute prompt or seed text.
bool IsNodeDisabled(const JsonValue* node) {
    if (!node || node->type != JsonType::Object) return false;
    const JsonValue* m = node->find("mode");
    if (!m || m->type != JsonType::Number) return false;
    return m->numVal == 2 || m->numVal == 4;
}

// Traversal of one "nodes" array -- the top-level one, or a subgraph
// definition's own, since a subgraph moves the real nodes out of the top
// level and leaves only an opaque instance placeholder behind.
void TraverseUiNodes(const std::vector<std::unique_ptr<JsonValue>>& nodesArray, const std::string& scopeId,
                      const UiLinkIndex& linkIndex, const SubgraphRegistry& registry, AImgInfo& info, ComfyUiExtraction& out) {
    for (const auto& item : nodesArray) {
        if (AbortRequested()) break; // ContentStopGetValueW was called for this file; stop walking and let the caller discard the result
        if (!item || item->type != JsonType::Object) continue;
        if (IsNodeDisabled(item.get())) continue;
        std::string typeStr = item->getStr("type");

        // "inputs" is an array of {name, type, link} here, so a wired
        // positive/negative source resolves the same way for any node type.
        const JsonValue* itemInputs = item->find("inputs");
        if (itemInputs && itemInputs->type == JsonType::Array) {
            int64_t posLinkId = 0, negLinkId = 0;
            bool hasPosLink = false, hasNegLink = false;
            for (const auto& inp : itemInputs->arrVal) {
                if (!inp || inp->type != JsonType::Object) continue;
                std::string inName = inp->getStr("name");
                if (inName != "positive" && inName != "negative") continue;
                const JsonValue* linkField = inp->find("link");
                if (!linkField || linkField->type != JsonType::Number) continue;
                int64_t linkId = ClampDoubleToInt64(linkField->numVal);
                if (inName == "positive") { posLinkId = linkId; hasPosLink = true; }
                else { negLinkId = linkId; hasNegLink = true; }
            }
            if (hasPosLink) {
                out.uiPosLinkSeen = true;
                std::string resolved = ResolveUiLinkText(scopeId, posLinkId, linkIndex, registry, /*wantNegative=*/false);
                if (!resolved.empty() && out.uiResolvedPos.empty()) out.uiResolvedPos = resolved;
            }
            if (hasNegLink) {
                // Both roles linking into the SAME origin node means one
                // node with two CONDITIONING outputs; skip past the string
                // already used for positive.
                bool sameOriginAsPositive = hasPosLink && ResolveLinkNode(scopeId, posLinkId, linkIndex) == ResolveLinkNode(scopeId, negLinkId, linkIndex);
                std::string resolved = ResolveUiLinkText(scopeId, negLinkId, linkIndex, registry, /*wantNegative=*/true, sameOriginAsPositive ? 1 : 0);
                if (!resolved.empty() && out.uiResolvedNeg.empty()) out.uiResolvedNeg = resolved;
            }
        }

        // getObj() does not verify the CHILD's type, and widgets_values is
        // an Array -- read it via objVal plus an explicit Array check.
        const JsonValue* itemWv = item->find("widgets_values");
        if (!itemWv || itemWv->type != JsonType::Array) continue;
        const auto& wArr = itemWv->arrVal;

        // Exact-name lookup dispatched AHEAD of every substring branch
        // below, the same rule the LoRA dispatch chain follows: a node with
        // dedicated handling must never be reclaimed by a broader match. No
        // name in kWidgetFieldMaps contains any of those substrings today,
        // which is exactly why the order is safe to fix now -- before an
        // upstream rename makes a branch silently unreachable.
        if (const WidgetFieldMap* fieldMap = FindWidgetFieldMap(typeStr)) {
            ApplyWidgetFieldMap(item.get(), *fieldMap, wArr, scopeId, linkIndex, registry, info, out.samplerName, out.schedulerName);
        } else if (typeStr == "CLIPTextEncode" || typeStr.find("TextEncode") != std::string::npos) {
            std::string text;
            int64_t textLink;
            bool positiveInputWired = false;
            // A "text" widget converted to a socket leaves widgets_values[0]
            // a stale leftover, so follow the link first.
            if (GetNodeInputLink(item.get(), "text", textLink)) {
                positiveInputWired = true;
                text = ResolveUiTextThroughLink(scopeId, textLink, linkIndex, registry, 0);
            }
            // Nodes with separate "prompt"/"negative_prompt" fields instead
            // of a shared "text". Either may be linked, so resolve through
            // the link BEFORE any local widget fallback below.
            std::string negFromNamedField;
            bool negativeInputWired = false;
            int64_t negLink;
            if (GetNodeInputLink(item.get(), "negative_prompt", negLink)) {
                negativeInputWired = true;
                negFromNamedField = ResolveUiTextThroughLink(scopeId, negLink, linkIndex, registry, 0);
            }
            if (text.empty()) {
                int64_t promptLink;
                if (GetNodeInputLink(item.get(), "prompt", promptLink)) {
                    positiveInputWired = true;
                    text = ResolveUiTextThroughLink(scopeId, promptLink, linkIndex, registry, 0);
                }
            }
            // The traversal above IS the recovery for a wired input, so an
            // empty result means recovery FAILED and the stale-wired-widget
            // rule applies: skip both positive-side local fallbacks rather
            // than surface the authoring-time template.
            if (text.empty() && !positiveInputWired && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String &&
                !LooksLikeSerializedTextBlob(wArr[0]->strVal)) {
                text = wArr[0]->strVal;
            }
            // Same rule, independently, for the "negative_prompt" input.
            if (negFromNamedField.empty() && !negativeInputWired) negFromNamedField = NamedWidgetString(item.get(), "negative_prompt");
            if (text.empty() && !positiveInputWired) text = NamedWidgetString(item.get(), "prompt");
            if (!text.empty()) {
                if (out.posPromptText.empty()) out.posPromptText = text;
                else if (out.negPromptText.empty()) out.negPromptText = text;
            }
            if (!negFromNamedField.empty() && out.negPromptText.empty()) out.negPromptText = negFromNamedField;
        } else if (typeStr == "Load Checkpoint" || ContainsCI(typeStr, "CheckpointLoader") ||
                   ContainsCI(typeStr, "UNETLoader") || ContainsCI(typeStr, "DiffusionModelLoader")) {
            std::string name;
            if (out.modelName.empty()) {
                bool u1 = false, u2 = false, u3 = false;
                if (TryBoundaryString(item.get(), "ckpt_name", scopeId, linkIndex, registry, name, u1) ||
                    TryBoundaryString(item.get(), "unet_name", scopeId, linkIndex, registry, name, u2) ||
                    TryBoundaryString(item.get(), "model_name", scopeId, linkIndex, registry, name, u3)) {
                    out.modelName = name;
                } else if (!(u1 || u2 || u3) && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
                    // Positional fallback only when no named field was an
                    // unrecoverable crossing -- see TryBoundaryString.
                    out.modelName = wArr[0]->strVal;
                }
            }
        } else if (typeStr == "VAELoader") {
            std::string name;
            if (out.vaeName.empty()) {
                bool unresolvable = false;
                if (TryBoundaryString(item.get(), "vae_name", scopeId, linkIndex, registry, name, unresolvable)) {
                    out.vaeName = name;
                } else if (!unresolvable && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String) {
                    out.vaeName = wArr[0]->strVal;
                }
            }
        } else if (ContainsCI(typeStr, "Lora")) {
            // A promoted "lora_name" must be checked before any local widget
            // read. When the instance's own input is itself wired, even its
            // copy is unreliable and the real name appears nowhere in the
            // file -- collect NOTHING rather than a plausible wrong guess.
            std::string name;
            bool unresolvable = false;
            if (TryBoundaryString(item.get(), "lora_name", scopeId, linkIndex, registry, name, unresolvable)) {
                out.loras.push_back(name);
            } else if (!unresolvable) {
                // Loaders with a single widget-backed "lora_name".
                name = NamedWidgetString(item.get(), "lora_name");
                // The positional fallback is safe only when that widget
                // looks like a model filename: "Lora" as a SUBSTRING also
                // matches non-loader nodes whose first widget is a
                // filename-prefix template, collected as a bogus entry.
                if (name.empty() && !wArr.empty() && wArr[0] && wArr[0]->type == JsonType::String &&
                    LooksLikeModelFileName(wArr[0]->strVal)) {
                    name = wArr[0]->strVal;
                }
                if (!name.empty()) {
                    out.loras.push_back(name);
                } else {
                    // The multi-slot "Power Lora Loader" shape: the same
                    // per-slot {"on", "lora", "strength"} object the API
                    // format exposes, serialized inside widgets_values.
                    for (const auto& w : wArr) {
                        if (!w || w->type != JsonType::Object || !w->find("lora")) continue;
                        const JsonValue* onField = w->find("on");
                        if (onField && onField->type == JsonType::Bool && !onField->boolVal) continue;
                        std::string loraName = w->getStr("lora");
                        if (loraName.empty()) continue;
                        out.loras.push_back(loraName + ": " + FormatCompactNumber(w->getNum("strength")));
                    }
                }
            }
            // Unresolvable: every stored copy of the name is stale.
        }
    }
}

// ---------------------------------------------------------------------------
// Output-driven scope for API-format graphs. Nothing strips a node the
// frontend never wired to an output: a leftover CLIPTextEncode, an unused
// upscale pass, or a whole second pipeline queued into the same file. The
// functions below restrict extraction to what fed the saved image.
// ---------------------------------------------------------------------------

// A link only counts when its target node exists: a malformed [123] must not
// be walked as a live edge.
bool IsLinkInput(const JsonValue* nodesObj, const JsonValue* v) {
    if (!v || v->type != JsonType::Array || v->arrVal.empty()) return false;
    std::string id = GetLinkNodeId(v);
    return !id.empty() && nodesObj->objVal.count(id) != 0;
}

// Every node reachable from startId over link inputs, excluding startId.
// Only used for membership and size, so traversal order does not matter.
std::set<std::string> ComputeAncestors(const JsonValue* nodesObj, const std::string& startId) {
    std::set<std::string> visited;
    std::vector<std::string> stack{startId};
    visited.insert(startId);
    while (!stack.empty()) {
        if (AbortRequested()) break;
        std::string id = stack.back();
        stack.pop_back();
        auto it = nodesObj->objVal.find(id);
        if (it == nodesObj->objVal.end() || !it->second) continue;
        const auto* inputs = it->second->getObj("inputs");
        if (!inputs) continue;
        for (const auto& kv : inputs->objVal) {
            if (!IsLinkInput(nodesObj, kv.second.get())) continue;
            std::string linkedId = GetLinkNodeId(kv.second.get());
            if (visited.insert(linkedId).second) stack.push_back(linkedId);
        }
    }
    visited.erase(startId);
    return visited;
}

// Shared by the API-format loop and the scope precompute so the two cannot
// drift. "Guider" classes match too: on split custom-sampler pipelines that
// is where cfg/positive/negative live, though a guider is never a pass.
bool IsSamplerFamilyClassType(const std::string& classType) {
    return classType == "KSampler" || classType == "KSamplerAdvanced" || classType == "KSamplerSelect" ||
           classType == "CFGGuider" || classType == "RandomNoise" || classType == "UltimateSDUpscale" ||
           classType.find("Sampler") != std::string::npos || classType.find("Scheduler") != std::string::npos ||
           ContainsCI(classType, "Guider");
}

// One full sampling pass: passes the class gate, carries a positive or guider
// link (bare RandomNoise/KSamplerSelect/scheduler helpers do not), and is not
// itself a guider -- a guider is wired into a pass, never one on its own.
bool IsSamplingCandidate(const JsonValue* node, const std::string& classType) {
    if (!IsSamplerFamilyClassType(classType)) return false;
    if (ContainsCI(classType, "Guider")) return false;
    const auto* inputs = node->getObj("inputs");
    if (!inputs) return false;
    return inputs->find("positive") != nullptr || inputs->find("guider") != nullptr;
}

// Output nodes: Save-class nodes, or Preview/Comparer ones when there are no
// Save nodes at all. A single scan either way, so the common no-output case
// stays cheap.
std::vector<std::string> FindOutputRoots(const JsonValue* nodesObj) {
    std::vector<std::string> saveRoots, previewRoots;
    for (const auto& pair : nodesObj->objVal) {
        if (AbortRequested()) break;
        const auto* node = pair.second.get();
        if (!node || node->type != JsonType::Object) continue;
        std::string classType = node->getStr("class_type");
        if (ContainsCI(classType, "Save")) saveRoots.push_back(pair.first);
        else if (ContainsCI(classType, "Preview") || ContainsCI(classType, "Comparer")) previewRoots.push_back(pair.first);
    }
    return !saveRoots.empty() ? saveRoots : previewRoots;
}

// Lets the scope walk tell this output's sampling passes from an unrelated
// pipeline's.
std::set<std::string> FindSamplingCandidates(const JsonValue* nodesObj) {
    std::set<std::string> candidates;
    for (const auto& pair : nodesObj->objVal) {
        if (AbortRequested()) break;
        const auto* node = pair.second.get();
        if (!node || node->type != JsonType::Object) continue;
        std::string classType = node->getStr("class_type");
        if (IsSamplingCandidate(node, classType)) candidates.insert(pair.first);
    }
    return candidates;
}

// With several outputs (a base-pass preview next to the real upscaled save),
// the one with the largest ancestor set pulls in the whole pipeline. Ties
// keep the first in map order.
std::string ChooseRootWithLargestAncestorSet(const JsonValue* nodesObj, const std::vector<std::string>& roots,
                                              std::set<std::string>& outAncestors) {
    std::string best;
    size_t bestSize = 0;
    for (const auto& r : roots) {
        if (AbortRequested()) break;
        std::set<std::string> anc = ComputeAncestors(nodesObj, r);
        if (best.empty() || anc.size() > bestSize) {
            bestSize = anc.size();
            outAncestors = std::move(anc);
            best = r;
        }
    }
    return best;
}

// A sampler whose latent input traces back through a VAEEncode to another
// sampling pass re-encodes that pass's pixels: a refine pass, even at denoise
// 1.0 -- unless it has a prompt of its own, which makes it a separate
// generation over the earlier image.
bool IsPixelRefinePass(const JsonValue* nodesObj, const std::string& id, const std::set<std::string>& candidates) {
    static const char* const kLatentInputNames[] = {"latent_image", "samples", "latent"};
    auto it = nodesObj->objVal.find(id);
    if (it == nodesObj->objVal.end() || !it->second) return false;
    const auto* inputs = it->second->getObj("inputs");
    if (!inputs) return false;
    std::set<std::string> visited;
    std::vector<std::string> stack;
    for (const char* name : kLatentInputNames) {
        const auto* v = inputs->find(name);
        if (!IsLinkInput(nodesObj, v)) continue;
        std::string t = GetLinkNodeId(v);
        if (visited.insert(t).second) stack.push_back(t);
    }
    while (!stack.empty()) {
        if (AbortRequested()) break;
        std::string cur = stack.back();
        stack.pop_back();
        if (candidates.count(cur)) continue; // direct latent chaining, not a pixel round-trip
        auto nit = nodesObj->objVal.find(cur);
        if (nit == nodesObj->objVal.end() || !nit->second) continue;
        if (ContainsCI(nit->second->getStr("class_type"), "VAEEncode")) {
            std::set<std::string> anc = ComputeAncestors(nodesObj, cur);
            bool upstreamPass = false;
            for (const auto& a : anc)
                if (candidates.count(a)) { upstreamPass = true; break; }
            if (!upstreamPass) continue;
            // Own prompt: readable text from an encoder the upstream pass
            // does not share.
            const auto* pin = inputs;
            const auto* pv = pin->find("positive");
            if (!pv) {
                if (const auto* gv = pin->find("guider")) {
                    auto git = nodesObj->objVal.find(GetLinkNodeId(gv));
                    const auto* gin = (git != nodesObj->objVal.end() && git->second) ? git->second->getObj("inputs") : nullptr;
                    if (gin) { pv = gin->find("positive"); if (!pv) pv = gin->find("conditioning"); }
                }
            }
            std::string posId = IsLinkInput(nodesObj, pv) ? GetLinkNodeId(pv) : std::string();
            if (!posId.empty() && !anc.count(posId) && !ResolveClipText(nodesObj, posId, true).empty()) return false;
            return true;
            // img2img from LoadImage and the like: not a refine pass
        }
        const auto* in = nit->second->getObj("inputs");
        if (!in) continue;
        for (const auto& kv : in->objVal) {
            if (!IsLinkInput(nodesObj, kv.second.get())) continue;
            std::string t = GetLinkNodeId(kv.second.get());
            if (visited.insert(t).second) stack.push_back(t);
        }
    }
    return false;
}

// The primary pass behind rootChoice: deterministic BFS over every link (it
// must walk through an upscale pass's "image" input to the base pass). First
// candidate with denoise absent or >= 0.5 that is not a pixel refine pass;
// failing that, the first passing the denoise test, else the first found.
std::string FindPrimarySamplingPass(const JsonValue* nodesObj, const std::string& rootChoice,
                                     const std::set<std::string>& candidates) {
    std::set<std::string> visited{rootChoice};
    std::vector<std::string> queue{rootChoice};
    std::string firstCandidate, firstDenoiseOk;
    for (size_t head = 0; head < queue.size(); head++) {
        if (AbortRequested()) break;
        const std::string& id = queue[head];
        if (candidates.count(id)) {
            if (firstCandidate.empty()) firstCandidate = id;
            bool qualifies = true;
            auto it = nodesObj->objVal.find(id);
            const auto* inputs = (it != nodesObj->objVal.end() && it->second) ? it->second->getObj("inputs") : nullptr;
            if (inputs) {
                if (const auto* dv = inputs->find("denoise")) {
                    bool found = false;
                    double n = ResolveNumberField(nodesObj, dv, found);
                    if (found && n < 0.5) qualifies = false;
                }
            }
            if (qualifies) {
                if (!IsPixelRefinePass(nodesObj, id, candidates)) return id;
                if (firstDenoiseOk.empty()) firstDenoiseOk = id;
            }
        }
        auto it = nodesObj->objVal.find(id);
        if (it == nodesObj->objVal.end() || !it->second) continue;
        const auto* inputs = it->second->getObj("inputs");
        if (!inputs) continue;
        for (const auto& kv : inputs->objVal) {
            if (!IsLinkInput(nodesObj, kv.second.get())) continue;
            std::string linkedId = GetLinkNodeId(kv.second.get());
            if (visited.insert(linkedId).second) queue.push_back(linkedId);
        }
    }
    return firstDenoiseOk.empty() ? firstCandidate : firstDenoiseOk;
}

// The primary pass plus every sampling candidate chained to it purely through
// latent links (a two-stage latent upscale). Stops at a VAEEncode: past a
// pixel round-trip is a different generation reusing this one's pixels.
std::set<std::string> ComputeSamplingGroup(const JsonValue* nodesObj, const std::string& primary,
                                            const std::set<std::string>& candidates) {
    static const char* const kLatentInputNames[] = {"latent_image", "samples", "latent"};
    std::set<std::string> group{primary};
    std::set<std::string> visited{primary};
    std::vector<std::string> queue{primary};
    for (size_t head = 0; head < queue.size(); head++) {
        if (AbortRequested()) break;
        const std::string& id = queue[head];
        auto it = nodesObj->objVal.find(id);
        if (it == nodesObj->objVal.end() || !it->second) continue;
        const auto* inputs = it->second->getObj("inputs");
        if (!inputs) continue;
        for (const char* name : kLatentInputNames) {
            const auto* v = inputs->find(name);
            if (!IsLinkInput(nodesObj, v)) continue;
            std::string targetId = GetLinkNodeId(v);
            if (targetId.empty() || !visited.insert(targetId).second) continue;
            auto targetIt = nodesObj->objVal.find(targetId);
            if (targetIt == nodesObj->objVal.end() || !targetIt->second) continue;
            if (candidates.count(targetId)) group.insert(targetId);
            if (ContainsCI(targetIt->second->getStr("class_type"), "VAEEncode")) continue;
            queue.push_back(targetId);
        }
    }
    return group;
}

// Every ancestor of the group over all links, except a sampling candidate
// outside the group (another pipeline's sampler) and pixel-space inputs
// ("image*", "pixels", "images"), through which unrelated post-processing
// and pipelines commonly hang off this one's output.
std::set<std::string> ComputeExtractionScope(const JsonValue* nodesObj, const std::set<std::string>& group,
                                              const std::set<std::string>& candidates) {
    std::set<std::string> scope = group;
    std::vector<std::string> queue(group.begin(), group.end());
    for (size_t head = 0; head < queue.size(); head++) {
        if (AbortRequested()) break;
        const std::string& id = queue[head];
        auto it = nodesObj->objVal.find(id);
        if (it == nodesObj->objVal.end() || !it->second) continue;
        const auto* inputs = it->second->getObj("inputs");
        if (!inputs) continue;
        for (const auto& kv : inputs->objVal) {
            const std::string& name = kv.first;
            if (name.rfind("image", 0) == 0 || name == "pixels" || name == "images") continue;
            if (!IsLinkInput(nodesObj, kv.second.get())) continue;
            std::string targetId = GetLinkNodeId(kv.second.get());
            if (targetId.empty() || scope.count(targetId)) continue;
            if (candidates.count(targetId) && !group.count(targetId)) continue;
            scope.insert(targetId);
            queue.push_back(targetId);
        }
    }
    return scope;
}

// Follows the primary sampler's "model" link (through LoRA stacks, patchers and
// samplers-of-model nodes alike, whatever their class names) to the node that
// has no model input of its own and reads the checkpoint name there. A custom
// sampler reaches its model through a guider instead. The terminal's name is
// accepted only when it looks like a model file, so a loader of some other
// kind (an upscale model, a placeholder string) is never reported as the model.
std::string ModelFromPrimaryChain(const JsonValue* nodesObj, const JsonValue* primaryInputs) {
    if (!primaryInputs) return "";
    std::string cur = GetLinkNodeId(primaryInputs->find("model"));
    if (cur.empty()) {
        std::string guiderId = GetLinkNodeId(primaryInputs->find("guider"));
        auto git = guiderId.empty() ? nodesObj->objVal.end() : nodesObj->objVal.find(guiderId);
        if (git != nodesObj->objVal.end() && git->second && git->second->type == JsonType::Object) {
            if (const JsonValue* gin = git->second->getObj("inputs")) cur = GetLinkNodeId(gin->find("model"));
        }
    }
    // The hop bound alone also ends a cycle.
    for (int hop = 0; hop < 32 && !cur.empty(); hop++) {
        auto it = nodesObj->objVal.find(cur);
        if (it == nodesObj->objVal.end() || !it->second || it->second->type != JsonType::Object) return "";
        const JsonValue* in = it->second->getObj("inputs");
        if (!in) return "";
        std::string next = GetLinkNodeId(in->find("model"));
        if (!next.empty()) { cur = std::move(next); continue; }
        static const char* const kKeys[] = {"ckpt_name", "unet_name", "model_name", "gguf_name"};
        for (const char* key : kKeys) {
            const JsonValue* v = in->find(key);
            if (!v) continue;
            std::string resolved = ResolveTextField(nodesObj, v, key);
            return LooksLikeModelFileName(resolved) ? resolved : std::string();
        }
        return "";
    }
    return "";
}

// active == false means no scoping: every node is processed.
struct ApiScopeInfo {
    bool active = false;
    std::set<std::string> scope;
    std::string rootChoice;
    std::string primary;
    std::set<std::string> rootAncestors;
    std::set<std::string> primaryAncestors;
};

ApiScopeInfo ComputeApiScope(const JsonValue* nodesObj) {
    ApiScopeInfo result;
    // UI format is untouched: TraverseUiNodes already excludes disabled nodes.
    if (const auto* nodesArr = nodesObj->find("nodes")) {
        if (nodesArr->type == JsonType::Array) return result;
    }
    std::vector<std::string> roots = FindOutputRoots(nodesObj);
    if (roots.empty()) return result; // no output node - no scoping at all

    std::set<std::string> rootAncestors;
    std::string rootChoice = ChooseRootWithLargestAncestorSet(nodesObj, roots, rootAncestors);
    std::set<std::string> candidates = FindSamplingCandidates(nodesObj);
    std::string primary = FindPrimarySamplingPass(nodesObj, rootChoice, candidates);
    if (primary.empty()) return result; // an output node exists but nothing recognizable feeds it - no scoping

    std::set<std::string> group = ComputeSamplingGroup(nodesObj, primary, candidates);

    result.active = true;
    result.scope = ComputeExtractionScope(nodesObj, group, candidates);
    result.rootChoice = std::move(rootChoice);
    result.primary = primary;
    result.rootAncestors = std::move(rootAncestors);
    result.primaryAncestors = ComputeAncestors(nodesObj, primary);
    return result;
}

} // namespace

bool AImgDecoder::DecodeComfyUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info, bool* outNegativeZeroed) {
    if (!root || root->type != SimpleJson::JsonType::Object) return false;

    // Every other decoder gates on a distinctive key before touching the
    // caller's `info`. This one structurally cannot: "is this a ComfyUI
    // graph" is only answerable by walking it. DecodeCore retries this once
    // per JSON candidate ("prompt" AND "workflow" are both tried), so a
    // failed attempt's partial writes would leak into the next attempt and
    // into every later generator -- and since the write guards here are
    // "if (!info.has_*)", a leaked flag would actively SUPPRESS the value a
    // subsequent successful attempt would have written. Decode into a local
    // and commit only once the graph is confirmed.
    AImgInfo local;

    const SimpleJson::JsonValue* nodesObj = root;
    const auto* promptField = root->find("prompt");
    const auto* workflowField = root->find("workflow");
    if (promptField && promptField->type == SimpleJson::JsonType::Object) {
        nodesObj = promptField;
    } else if (workflowField && workflowField->type == SimpleJson::JsonType::Object) {
        nodesObj = workflowField;
    }

    // At least one node must actually look like a graph node before this
    // JSON is claimed as ComfyUI -- otherwise any unrelated metadata object
    // would match here first, since this is the first decoder tried, and the
    // others would never run. Folded into the extraction loop rather than a
    // separate pass: every extraction branch already requires class_type plus
    // inputs, so detection is guaranteed true before info is touched.
    bool looksLikeComfyGraph = false;

    std::string widthHeight;
    std::vector<std::string> loras;
    // First sampler-family node reached wins. The map is keyed by node-id
    // STRINGS, so "first" is LEXICOGRAPHIC ("10" before "2"), not authoring
    // order or id magnitude -- with two samplers (base and refiner) the
    // choice therefore depends on how the author numbered their nodes. The
    // serialized graph carries no reliable base-vs-refiner signal, so this
    // stays documented incidental behaviour rather than a guessed heuristic.
    std::string positiveNodeId, negativeNodeId;
    std::string guessPosNodeId; // which node the encounter-order Prompt guess came from
    // The node that supplied positiveNodeId has no "negative" input at all
    // (a BasicGuider), as opposed to one whose negative link did not resolve.
    bool posSourceLacksNegative = false;
    ComfyUiExtraction ui;

    // Computed once up front, so the loop's per-node check is one lookup.
    ApiScopeInfo apiScope = ComputeApiScope(nodesObj);

    // 1. API Graph Format Traversal
    for (const auto& pair : nodesObj->objVal) {
        if (AbortRequested()) break; // ContentStopGetValueW was called for this file; stop walking and let the caller discard the result

        const auto* node = pair.second.get();
        if (!node || node->type != SimpleJson::JsonType::Object) continue;

        std::string classType = node->getStr("class_type");
        const auto* inputs = node->getObj("inputs");
        if (!inputs) continue;
        if (!classType.empty()) looksLikeComfyGraph = true;

        // Detection (looksLikeComfyGraph above) is deliberately unscoped.
        if (apiScope.active && !apiScope.scope.count(pair.first)) continue;

        // "Custom sampler" graphs split one KSampler into single-purpose
        // nodes wired together -- RandomNoise (seed), CFGGuider (cfg +
        // positive/negative), KSamplerSelect (sampler), a scheduler
        // (steps/denoise) -- so dispatch on key presence rather than a
        // literal node name. "UltimateSDUpscale" is the same shape but its
        // class_type contains neither "Sampler" nor "Scheduler", and a
        // workflow using it for the whole generation would otherwise leave
        // every one of these fields empty.
        if (IsSamplerFamilyClassType(classType)) {
            // Usually literal numbers, but some graphs route them through a
            // helper node via a link array. A plain getInt64/getNum on that
            // array silently reads 0 while still marking the field found,
            // locking in a wrong value MergeGaps can never correct.
            bool foundNum;
            if (const auto* v = inputs->find("seed")) {
                double n = ResolveNumberField(nodesObj, v, foundNum, "seed");
                if (foundNum) { local.seed = ClampDoubleToInt64(n); local.has_seed = true; }
            } else if (const auto* v2 = inputs->find("noise_seed")) {
                double n = ResolveNumberField(nodesObj, v2, foundNum, "noise_seed");
                if (foundNum) { local.seed = ClampDoubleToInt64(n); local.has_seed = true; }
            }
            // Two-stage samplers (a base pass plus a refine pass in one
            // node) prefix every field "stage1_"/"stage2_", so the lookups
            // above find nothing despite the node matching as sampler-family.
            // Use stage1_* for the primary fields and surface stage2_steps
            // through hires_steps, as A1111's hires-fix pass already maps.
            if (const auto* v = inputs->find("steps")) {
                double n = ResolveNumberField(nodesObj, v, foundNum, "steps");
                if (foundNum) { local.steps = (int32_t)ClampDoubleToInt64(n); local.has_steps = true; }
            } else if (const auto* v1 = inputs->find("stage1_steps")) {
                double n = ResolveNumberField(nodesObj, v1, foundNum, "stage1_steps");
                if (foundNum) { local.steps = (int32_t)ClampDoubleToInt64(n); local.has_steps = true; }
            }
            if (const auto* v = inputs->find("stage2_steps")) {
                double n = ResolveNumberField(nodesObj, v, foundNum, "stage2_steps");
                if (foundNum) { local.hires_steps = (int32_t)ClampDoubleToInt64(n); local.has_hires_steps = true; }
            }
            if (const auto* v = inputs->find("cfg")) {
                double n = ResolveNumberField(nodesObj, v, foundNum, "cfg");
                if (foundNum) { local.cfg_scale = n; local.has_cfg = true; }
            } else if (const auto* v1 = inputs->find("stage1_cfg")) {
                double n = ResolveNumberField(nodesObj, v1, foundNum, "stage1_cfg");
                if (foundNum) { local.cfg_scale = n; local.has_cfg = true; }
            }
            // First-wins: the map is keyed by node-id string, so a
            // lexicographically later node resolving to "" would otherwise
            // clobber a good value an earlier one wrote -- the opposite of
            // every other guard in this decoder.
            if (ui.samplerName.empty()) {
                if (const auto* v = inputs->find("sampler_name")) {
                    std::string resolved = ResolveTextField(nodesObj, v, "sampler_name");
                    if (!resolved.empty()) ui.samplerName = std::move(resolved);
                } else if (const auto* v1 = inputs->find("stage1_sampler_name")) {
                    std::string resolved = ResolveTextField(nodesObj, v1, "stage1_sampler_name");
                    if (!resolved.empty()) ui.samplerName = std::move(resolved);
                }
            }
            if (ui.schedulerName.empty()) {
                if (const auto* v = inputs->find("scheduler")) {
                    std::string resolved = ResolveTextField(nodesObj, v, "scheduler");
                    if (!resolved.empty()) ui.schedulerName = std::move(resolved);
                } else if (const auto* v1 = inputs->find("stage1_scheduler")) {
                    std::string resolved = ResolveTextField(nodesObj, v1, "stage1_scheduler");
                    if (!resolved.empty()) ui.schedulerName = std::move(resolved);
                }
            }
            if (const auto* v = inputs->find("denoise")) {
                double n = ResolveNumberField(nodesObj, v, foundNum, "denoise");
                if (foundNum) { local.denoising_strength = n; local.has_denoising_strength = true; }
            } else if (const auto* v1 = inputs->find("stage1_denoise")) {
                double n = ResolveNumberField(nodesObj, v1, foundNum, "stage1_denoise");
                if (foundNum) { local.denoising_strength = n; local.has_denoising_strength = true; }
            }
            if (positiveNodeId.empty()) {
                if (const auto* v = inputs->find("positive")) positiveNodeId = GetLinkNodeId(v);
                else if (classType == "BasicGuider") {
                    // BasicGuider's only conditioning input is named
                    // "conditioning"; it has no negative side.
                    if (const auto* v2 = inputs->find("conditioning")) positiveNodeId = GetLinkNodeId(v2);
                }
                if (!positiveNodeId.empty()) posSourceLacksNegative = (inputs->find("negative") == nullptr);
            }
            if (negativeNodeId.empty()) {
                if (const auto* v = inputs->find("negative")) negativeNodeId = GetLinkNodeId(v);
            }
        }
        // Loader-held prompts ("Efficient Loader" and kin) carry the
        // positive/negative links on the LOADER, not the sampler: its
        // samplers take one bundled pipe input with no positive/negative
        // fields, so the gate above never sees them.
        else if (IsLoaderHeldPromptClass(classType)) {
            if (positiveNodeId.empty()) {
                if (const auto* v = inputs->find("positive")) positiveNodeId = GetLinkNodeId(v);
            }
            if (negativeNodeId.empty()) {
                if (const auto* v = inputs->find("negative")) negativeNodeId = GetLinkNodeId(v);
            }
        }

        const bool posWasEmpty = ui.posPromptText.empty();
        if (classType == "CLIPTextEncode" || classType == "BNK_CLIPTextEncodeAdvanced" ||
            classType.find("Prompt") != std::string::npos || classType.find("TextEncode") != std::string::npos) {
            if (classType == "PromptToolkit_ShowText" && !inputs->getStr("displayed_text").empty()) {
                // A display node whose "text" input links back into the
                // encoder that produced it. Links are addressed by node id,
                // not output slot, so following it re-enters that encoder and
                // finds no literal (only a wildcard-pool DSL). The node's own
                // "displayed_text" already holds the expanded text.
                std::string text = inputs->getStr("displayed_text");
                if (ui.posPromptText.empty()) ui.posPromptText = text;
                else if (ui.negPromptText.empty()) ui.negPromptText = text;
            } else if (const auto* textField = inputs->find("text")) {
                std::string text = ResolveTextField(nodesObj, textField);
                if (!text.empty() && !LooksLikeSerializedTextBlob(text)) {
                    if (ui.posPromptText.empty()) ui.posPromptText = text;
                    else if (ui.negPromptText.empty()) ui.negPromptText = text;
                }
            } else if (classType.rfind("WeiLinPromptUI", 0) == 0 && inputs->find("positive")) {
                // Both role instances name their text "positive": fill pos,
                // then neg, in encounter order like "text" above.
                std::string text = ResolveTextField(nodesObj, inputs->find("positive"));
                if (!text.empty() && !LooksLikeSerializedTextBlob(text)) {
                    if (ui.posPromptText.empty()) ui.posPromptText = text;
                    else if (ui.negPromptText.empty()) ui.negPromptText = text;
                }
            } else {
                // Separate "prompt"/"negative_prompt" on one node: read both
                // by name, since the encounter-order guess above would only
                // ever see one of them.
                if (ui.posPromptText.empty()) {
                    if (const auto* v = inputs->find("prompt")) {
                        std::string t = ResolveTextField(nodesObj, v);
                        if (!t.empty()) ui.posPromptText = t;
                    }
                }
                if (ui.negPromptText.empty()) {
                    if (const auto* v = inputs->find("negative_prompt")) {
                        std::string t = ResolveTextField(nodesObj, v);
                        if (!t.empty()) ui.negPromptText = t;
                    }
                }
            }
        }
        if (posWasEmpty && !ui.posPromptText.empty()) guessPosNodeId = pair.first;

        // Packs wrap the stock loader in a prefixed variant keeping the same
        // inputs, so match on substring rather than an exact list. Case-
        // insensitive because GGUF variants don't preserve "UNETLoader"'s
        // casing; matching "GGUF" alone also covers reversed names like
        // "LoaderGGUF", which contain "UNETLoader" nowhere. The name fields
        // go through ResolveTextField, not getStr(): a graph may route the
        // literal through a selector helper, and getStr() on a link array
        // silently returns "".
        if (classType == "Load Checkpoint" || ContainsCI(classType, "CheckpointLoader") ||
            ContainsCI(classType, "DiffusionModelLoader") || ContainsCI(classType, "UNETLoader") ||
            ContainsCI(classType, "GGUF")) {
            // First-wins, and only a non-empty value: a later loader with an
            // unreadable name must never blank out a good one.
            if (ui.modelName.empty()) {
                std::string resolved;
                if (const auto* ckptV = inputs->find("ckpt_name")) resolved = ResolveTextField(nodesObj, ckptV, "ckpt_name");
                else if (const auto* unetV = inputs->find("unet_name")) resolved = ResolveTextField(nodesObj, unetV, "unet_name");
                else if (const auto* modelV = inputs->find("model_name")) resolved = ResolveTextField(nodesObj, modelV, "model_name");
                else if (const auto* ggufV = inputs->find("gguf_name")) resolved = ResolveTextField(nodesObj, ggufV, "gguf_name");
                if (!resolved.empty()) ui.modelName = std::move(resolved);
            }
        }

        // A loader-held ckpt_name can be a cosmetic placeholder when the
        // checkpoint is supplied externally; only a real model filename counts.
        if (ui.modelName.empty() && IsLoaderHeldPromptClass(classType)) {
            if (const auto* ckptV = inputs->find("ckpt_name")) {
                std::string resolved = ResolveTextField(nodesObj, ckptV, "ckpt_name");
                if (!resolved.empty() && LooksLikeModelFileName(resolved)) ui.modelName = std::move(resolved);
            }
        }

        // Substring, not ==: custom packs ship their own VAE loaders with the
        // same vae_name input.
        if (ContainsCI(classType, "VAELoader")) {
            if (ui.vaeName.empty()) {
                if (const auto* v = inputs->find("vae_name")) {
                    std::string resolved = ResolveTextField(nodesObj, v, "vae_name");
                    if (!resolved.empty()) ui.vaeName = std::move(resolved);
                }
            }
        } else if (classType.find("Efficient Loader") != std::string::npos && ui.vaeName.empty()) {
            // This loader's "vae_name" is a real literal, but its
            // "ckpt_name" can hold a cosmetic placeholder when the checkpoint
            // is routed in externally -- so take only the VAE, and only
            // behind a real dedicated VAELoader.
            if (const auto* v = inputs->find("vae_name")) {
                std::string resolved = ResolveTextField(nodesObj, v, "vae_name");
                if (!resolved.empty()) ui.vaeName = std::move(resolved);
            }
        }

        // Case-insensitive for the same reason as the checkpoint matching
        // above: packs rename class_type freely and vary its casing.
        bool classTypeHasLora = ContainsCI(classType, "Lora");

        // Exact class_type matches dispatch FIRST, ahead of the substring
        // heuristics below, so a node with a dedicated branch can never be
        // reclaimed by a looser match once some pack grows a "lora_name"
        // input or a "Lora" substring.
        //
        // LoraTagLoader has no lora_name or slot fields: it reads its list
        // out of A1111-style "<lora:name:weight>" tags in its own "text".
        if (classType == "LoraTagLoader") {
            if (const auto* v = inputs->find("text")) {
                std::string text = ResolveTextField(nodesObj, v);
                ExtractLoraTags(text, loras);
            }
        }
        else if (classType == "Lora Loader (LoraManager)") {
            // A multi-select loader storing its slots under a plain "loras"
            // key shaped {"__value__": [{name, strength, active}, ...]} --
            // invisible to every "lora_N" shape above. Its "text" widget also
            // carries a tag for EVERY entry regardless of "active", so
            // reading that instead would overcount: use the structured array
            // and keep only entries with an explicit boolean active:true.
            // "strength" is inconsistently typed within one real file, so
            // accept a Number or a numeric String rather than silently reading 0.
            const JsonValue* valueArr = nullptr;
            if (const auto* lorasField = inputs->find("loras")) {
                if (lorasField->type == JsonType::Object) valueArr = lorasField->find("__value__");
            }
            if (valueArr && valueArr->type == JsonType::Array) {
                for (const auto& entry : valueArr->arrVal) {
                    if (!entry || entry->type != JsonType::Object) continue;
                    const JsonValue* activeField = entry->find("active");
                    if (!activeField || activeField->type != JsonType::Bool || !activeField->boolVal) continue;
                    std::string name = entry->getStr("name");
                    if (name.empty()) continue;
                    const JsonValue* strengthField = entry->find("strength");
                    double strength = 1.0;
                    if (strengthField) {
                        if (strengthField->type == JsonType::Number) strength = strengthField->numVal;
                        else if (strengthField->type == JsonType::String && !strengthField->strVal.empty()) strength = AImgDecoderInternal::ParseLeadingDouble(strengthField->strVal.c_str());
                    }
                    loras.push_back(name + ": " + FormatCompactNumber(strength));
                }
            }
        }
        else if (classType == "PromptToolkit_SuperEncode") {
            // A node bundling numbered LoRA slots onto the prompt encoder
            // itself, so its class_type contains no "Lora" and the substring
            // match never sees it. Unlike every other multi-slot shape, every
            // field here -- the enabled flag and the strength included -- is
            // serialized as a STRING, not a JSON bool or number.
            for (const auto& kv : inputs->objVal) {
                if (kv.first.rfind("lora_", 0) != 0 || kv.first.rfind("lora_name", 0) == 0) continue;
                if (!kv.second || kv.second->type != JsonType::String) continue;
                const std::string& name = kv.second->strVal;
                if (name.empty() || name == "None") continue;
                std::string suffix = kv.first.substr(5); // after "lora_"
                const auto* useField = inputs->find("use_lora_" + suffix);
                if (useField && useField->type == JsonType::String && useField->strVal == "false") continue;
                const auto* strengthField = inputs->find("strength_" + suffix);
                std::string strengthStr = (strengthField && strengthField->type == JsonType::String) ? strengthField->strVal : "1";
                loras.push_back(name + ": " + strengthStr);
            }
        }
        else if (classType.rfind("WeiLinPromptUI", 0) == 0) {
            // The LoRA-stack variant serializes its whole stack as one JSON
            // array string under "lora_str"; parse it directly.
            const auto* lsField = inputs->find("lora_str");
            if (lsField && lsField->type == JsonType::String && !lsField->strVal.empty()) {
                auto parsed = ParseJson(lsField->strVal);
                if (parsed && parsed->type == JsonType::Array) {
                    for (const auto& entry : parsed->arrVal) {
                        if (!entry || entry->type != JsonType::Object) continue;
                        const JsonValue* hiddenField = entry->find("hidden");
                        if (hiddenField && hiddenField->type == JsonType::Bool && hiddenField->boolVal) continue;
                        std::string name = entry->getStr("name");
                        if (name.empty()) continue;
                        double weight = 1.0;
                        if (const JsonValue* w = entry->find("weight")) {
                            if (w->type == JsonType::Number) weight = w->numVal;
                        }
                        loras.push_back(name + ": " + FormatCompactNumber(weight));
                    }
                }
            }
        }
        // Packs rename the stock loader while keeping "lora_name", so match
        // on substring rather than an exact list.
        else if (classTypeHasLora && inputs->find("lora_name")) {
            loras.push_back(inputs->getStr("lora_name"));
        } else if (classTypeHasLora) {
            // The "Power Lora Loader" shape: numbered slots, each an object
            // {"on", "lora", "strength"}, visited in order by std::map's own
            // alphabetical iteration. ONLY an explicit "on": false excludes a
            // slot -- absent means enabled, as everywhere else here.
            for (const auto& kv : inputs->objVal) {
                if (kv.first.rfind("lora_", 0) != 0 || !kv.second) continue;
                if (kv.second->type == JsonType::Object) {
                    const auto& slot = *kv.second;
                    const JsonValue* onField = slot.find("on");
                    if (onField && onField->type == JsonType::Bool && !onField->boolVal) continue;
                    std::string name = slot.getStr("lora");
                    if (name.empty()) continue;
                    loras.push_back(name + ": " + FormatCompactNumber(slot.getNum("strength")));
                } else if (kv.second->type == JsonType::String && kv.first.rfind("lora_name_", 0) == 0) {
                    // A "LoRA Stacker" shape: "lora_name_N" plus sibling
                    // "lora_wt_N" (the combined weight; the separate model/
                    // clip strengths can diverge, but then no single weight
                    // exists anyway). Checked BEFORE the plain "lora_N" shape
                    // below, which also matches "lora_name_N" but derives the
                    // suffix "name_N", matching no real strength field and
                    // silently defaulting every weight to 1.0.
                    const std::string& name = kv.second->strVal;
                    if (name.empty() || name == "None") continue;
                    std::string suffix = kv.first.substr(10); // after "lora_name_"
                    const auto* weightField = inputs->find("lora_wt_" + suffix);
                    double strength = (weightField && weightField->type == JsonType::Number) ? weightField->numVal : 1.0;
                    loras.push_back(name + ": " + FormatCompactNumber(strength));
                } else if (kv.second->type == JsonType::String && kv.first.rfind("lora_", 0) == 0) {
                    // Other multi-slot loaders use flat "lora_N"/"strength_N"
                    // siblings instead of a per-slot object, with "None" as
                    // the unused-slot sentinel.
                    const std::string& name = kv.second->strVal;
                    // A serialized LoRA stack under a "lora_" key is not one
                    // slot's name.
                    if (name.empty() || name == "None" || LooksLikeSerializedTextBlob(name)) continue;
                    std::string suffix = kv.first.substr(5); // after "lora_"
                    const auto* strengthField = inputs->find("strength_" + suffix);
                    double strength = (strengthField && strengthField->type == JsonType::Number) ? strengthField->numVal : 1.0;
                    loras.push_back(name + ": " + FormatCompactNumber(strength));
                }
            }
        }

        if (classType == "EmptyLatentImage" || classType == "EmptySD3LatentImage" ||
            classType.find("LatentImage") != std::string::npos || classType.find("Latent Image") != std::string::npos) {
            const auto* widthField = inputs->find("width");
            const auto* heightField = inputs->find("height");
            // A resolution node's width/height are its outputs 0/1.
            auto linkSlot = [](const JsonValue* v) -> double {
                return (v && v->type == JsonType::Array && v->arrVal.size() >= 2 && v->arrVal[1] &&
                        v->arrVal[1]->type == JsonType::Number) ? v->arrVal[1]->numVal : -1.0;
            };
            const JsonValue* sizeNode = nullptr;
            if (linkSlot(widthField) == 0.0 && linkSlot(heightField) == 1.0) {
                std::string wId = GetLinkNodeId(widthField);
                auto rit = nodesObj->objVal.find(wId);
                if (wId == GetLinkNodeId(heightField) && rit != nodesObj->objVal.end() && rit->second) {
                    sizeNode = rit->second.get();
                }
            }
            int64_t sw = 0, sh = 0;
            bool haveSize = false;
            if (sizeNode && sizeNode->getStr("class_type") == "ResolutionSelector") {
                haveSize = ResolutionSelectorSize(sizeNode, sw, sh);
            }
            if (sizeNode && !haveSize) {
                // Slots 0/1 are required: image-derived resize nodes also carry
                // width/height widgets, but output the real size on other slots
                // (e.g. 2), so their widgets are not the size.
                if (const auto* nodeInputs = sizeNode->find("inputs")) {
                    const auto* nw = nodeInputs->find("width");
                    const auto* nh = nodeInputs->find("height");
                    if (nw && nh && nw->type == JsonType::Number && nh->type == JsonType::Number) {
                        sw = SimpleJson::ClampDoubleToInt64(nw->numVal);
                        sh = SimpleJson::ClampDoubleToInt64(nh->numVal);
                        haveSize = sw > 0 && sh > 0;
                    }
                }
            }
            if (haveSize) {
                widthHeight = std::to_string(sw) + "x" + std::to_string(sh);
            } else if (widthField && heightField) {
                // Some graphs route width/height through a PrimitiveInt node
                // rather than embedding the numbers.
                std::string wStr = ResolveTextField(nodesObj, widthField);
                std::string hStr = ResolveTextField(nodesObj, heightField);
                if (!wStr.empty() && !hStr.empty()) widthHeight = wStr + "x" + hStr;
            } else if (const auto* dimField = inputs->find("dimensions")) {
                // One variant packs width/height into a single free-form
                // "dimensions" string, e.g. " 832 x 1216  (portrait)".
                std::string dims = ResolveTextField(nodesObj, dimField);
                size_t wStart = dims.find_first_of("0123456789");
                if (wStart != std::string::npos) {
                    size_t wEnd = dims.find_first_not_of("0123456789", wStart);
                    std::string wStr = dims.substr(wStart, wEnd - wStart);
                    size_t xPos = dims.find('x', wEnd);
                    if (xPos != std::string::npos) {
                        size_t hStart = dims.find_first_of("0123456789", xPos);
                        if (hStart != std::string::npos) {
                            size_t hEnd = dims.find_first_not_of("0123456789", hStart);
                            std::string hStr = dims.substr(hStart, hEnd - hStart);
                            if (!wStr.empty() && !hStr.empty()) widthHeight = wStr + "x" + hStr;
                        }
                    }
                }
            }
        }
    }

    if (!looksLikeComfyGraph) {
        const auto* topNodesCheck = nodesObj->find("nodes");
        if (topNodesCheck && topNodesCheck->type == SimpleJson::JsonType::Array) {
            for (const auto& item : topNodesCheck->arrVal) {
                if (item && item->type == SimpleJson::JsonType::Object && !item->getStr("type").empty()) {
                    looksLikeComfyGraph = true;
                    break;
                }
            }
        }
    }
    if (!looksLikeComfyGraph) return false;

    local.has_metadata = true;
    local.generator = L"ComfyUI";

    // The exact wired nodes beat the "first/second found" guess above, which
    // is unreliable once a graph has more than two text-encode nodes.
    // With no sampler in scope, a saver-like node wiring both prompts in as
    // links names the roles itself; the encounter-order guess swaps them
    // whenever the negative's node id sorts first. Only links count: string
    // literals there are placeholders.
    if (positiveNodeId.empty() && negativeNodeId.empty()) {
        for (const auto& pair : nodesObj->objVal) {
            if (AbortRequested()) break;
            if (apiScope.active && !apiScope.scope.count(pair.first)) continue;
            if (!pair.second || pair.second->type != JsonType::Object) continue;
            const JsonValue* in = pair.second->getObj("inputs");
            if (!in) continue;
            const JsonValue* posLink = in->find("positive");
            const JsonValue* negLink = in->find("negative");
            if (GetLinkNodeId(posLink).empty() || GetLinkNodeId(negLink).empty()) continue;
            // ResolveTextField follows the link through an encoder's own "text"
            // as well as a plain string node's "value".
            std::string pos = ResolveTextField(nodesObj, posLink);
            std::string neg = ResolveTextField(nodesObj, negLink);
            if (!pos.empty()) {
                ui.posPromptText = std::move(pos);
                ui.negPromptText = std::move(neg);
            }
            break;
        }
    }

    std::string resolvedPos = ResolveClipText(nodesObj, positiveNodeId, /*isPositive=*/true);
    std::string resolvedNeg = ResolveClipText(nodesObj, negativeNodeId, /*isPositive=*/false);
    if (!resolvedPos.empty()) ui.posPromptText = resolvedPos;
    if (!resolvedNeg.empty()) ui.negPromptText = resolvedNeg;
    // A deliberately zeroed negative stays blank: the encounter-order guess
    // would otherwise refill it with the text the zeroed conditioning came from.
    // So does a negative the positive's source has no input for (BasicGuider);
    // one that merely failed to resolve may still be companion-filled.
    const bool negAbsent = apiScope.active && !positiveNodeId.empty() && negativeNodeId.empty() && posSourceLacksNegative;
    const bool negZeroed = resolvedNeg.empty() && (IsZeroedConditioning(nodesObj, negativeNodeId) || negAbsent);
    if (negZeroed) ui.negPromptText.clear();
    // A positive computed at run time resolves empty, and the encounter-order
    // guess may then hold the NEGATIVE text. Leave it blank instead, so a
    // companion text block can fill it.
    if (resolvedPos.empty() && !positiveNodeId.empty() && !resolvedNeg.empty() && ui.posPromptText == resolvedNeg) {
        ui.posPromptText.clear();
    }
    // Same hazard when the negative resolves empty by design (a zeroed-out
    // negative): keep the guess only if it sits on the positive's own chain.
    if (resolvedPos.empty() && !positiveNodeId.empty() && !ui.posPromptText.empty() && !guessPosNodeId.empty() &&
        guessPosNodeId != positiveNodeId && !ComputeAncestors(nodesObj, positiveNodeId).count(guessPosNodeId)) {
        ui.posPromptText.clear();
    }

    // Every definition (both the array and legacy object-map forms) plus its
    // instantiating node, collected BEFORE any traversal so promoted-widget
    // resolution always has somewhere to find the real current value.
    std::vector<const SimpleJson::JsonValue*> subgraphDefs;
    if (const auto* defs = nodesObj->getObj("definitions")) {
        if (const auto* sgVal = defs->find("subgraphs")) {
            if (sgVal->type == SimpleJson::JsonType::Array) {
                for (const auto& sg : sgVal->arrVal) if (sg) subgraphDefs.push_back(sg.get());
            } else if (sgVal && sgVal->type == SimpleJson::JsonType::Object) {
                for (const auto& kv : sgVal->objVal) if (kv.second) subgraphDefs.push_back(kv.second.get());
            }
        }
    }

    SubgraphRegistry registry;
    for (const auto* sg : subgraphDefs) {
        if (!sg || sg->type != SimpleJson::JsonType::Object) continue;
        const auto* idField = sg->find("id");
        if (!idField) continue;
        std::string sgId = JsonIdToString(idField);
        if (!sgId.empty()) registry[sgId].def = sg;
    }
    const auto* topNodesField = nodesObj->find("nodes");
    if (topNodesField && topNodesField->type == SimpleJson::JsonType::Array) {
        for (const auto& n : topNodesField->arrVal) {
            if (!n || n->type != SimpleJson::JsonType::Object) continue;
            auto regIt = registry.find(n->getStr("type"));
            if (regIt != registry.end() && !regIt->second.instanceNode) regIt->second.instanceNode = n.get();
        }
    }

    // A subgraph never placed in the graph, or disabled, contributes nothing
    // to the generation: skip both its link collection and its traversal, so
    // its internal nodes can't pollute prompt/seed through the "any node
    // found anywhere" fallback below.
    auto subgraphInstanceActive = [&](const std::string& sgId) {
        auto it = registry.find(sgId);
        return it != registry.end() && it->second.instanceNode && !IsNodeDisabled(it->second.instanceNode);
    };

    // Scope "" is the top-level graph; each definition uses its own id,
    // since node and link ids are unique only within one scope.
    UiLinkIndex linkIndex;
    CollectUiNodesAndLinks(nodesObj, "", linkIndex);
    for (const auto* sg : subgraphDefs) {
        const auto* idField = sg->find("id");
        std::string sgId = idField ? JsonIdToString(idField) : "";
        if (!subgraphInstanceActive(sgId)) continue;
        CollectUiNodesAndLinks(sg, sgId, linkIndex);
    }

    // 2. UI Graph Format Traversal: the top-level "nodes" array plus each
    // subgraph definition's own, since a subgraph moves the real nodes there
    // and leaves only an opaque instance placeholder at the top level.
    if (topNodesField && topNodesField->type == SimpleJson::JsonType::Array) {
        TraverseUiNodes(topNodesField->arrVal, "", linkIndex, registry, local, ui);
    }
    for (const auto* sg : subgraphDefs) {
        const auto* sgNodesField = sg->find("nodes");
        if (!sgNodesField || sgNodesField->type != SimpleJson::JsonType::Array) continue;
        const auto* idField2 = sg->find("id");
        std::string sgId = idField2 ? JsonIdToString(idField2) : "";
        if (!subgraphInstanceActive(sgId)) continue;
        TraverseUiNodes(sgNodesField->arrVal, sgId, linkIndex, registry, local, ui);
    }

    // Link-resolved text wins over the traversal's positional guess.
    if (!ui.uiResolvedPos.empty()) ui.posPromptText = ui.uiResolvedPos;
    if (!ui.uiResolvedNeg.empty()) ui.negPromptText = ui.uiResolvedNeg;
    // UI-format twin of the unresolvable-positive guard above.
    if (ui.uiResolvedPos.empty() && ui.uiPosLinkSeen && !ui.negPromptText.empty() && ui.posPromptText == ui.negPromptText) {
        ui.posPromptText.clear();
    }

    // Only when the API-format loop found none: it runs over the
    // authoritative execution graph whenever one is present.
    if (loras.empty() && !ui.loras.empty()) loras = ui.loras;

    // A loader alone is not success: it is easy to find even when the real
    // parameters live in unrecognized node types, and claiming success would
    // block the A1111-style fallback text some save nodes embed.
    bool foundAnything = !ui.posPromptText.empty() || !ui.negPromptText.empty() ||
                          !ui.samplerName.empty() || !ui.schedulerName.empty() ||
                          local.has_seed || local.has_steps || local.has_cfg;
    if (!foundAnything) {
        // `local` is discarded; the caller's `info` was never touched.
        return false;
    }

    // The scope can exclude every VAE loader; a blank VAE is worse than taking
    // the first one anywhere upstream of the chosen output.
    if (apiScope.active && ui.vaeName.empty()) {
        for (const auto& id : apiScope.rootAncestors) {
            if (AbortRequested()) break;
            auto vaeIt = nodesObj->objVal.find(id);
            if (vaeIt == nodesObj->objVal.end() || !vaeIt->second) continue;
            if (!ContainsCI(vaeIt->second->getStr("class_type"), "VAELoader")) continue;
            const auto* vaeInputs = vaeIt->second->getObj("inputs");
            if (!vaeInputs) continue;
            if (const auto* v = vaeInputs->find("vae_name")) {
                std::string resolved = ResolveTextField(nodesObj, v, "vae_name");
                if (!resolved.empty()) { ui.vaeName = std::move(resolved); break; }
            }
        }
    }

    // Hires upscale: product of the upscale factors layered on top of the
    // primary pass (upstream of the output, not of the primary). Left unset,
    // not "1", when none resolved -- "1" would claim a hires pass exists.

    if (apiScope.active) {
        double product = 1.0;
        bool anyResolved = false;
        for (const auto& id : apiScope.rootAncestors) {
            if (AbortRequested()) break;
            if (id == apiScope.primary || apiScope.primaryAncestors.count(id)) continue;
            auto upIt = nodesObj->objVal.find(id);
            if (upIt == nodesObj->objVal.end() || !upIt->second) continue;
            std::string upClassType = upIt->second->getStr("class_type");
            const char* fieldName = nullptr;
            if (upClassType == "IterativeLatentUpscale") fieldName = "upscale_factor";
            else if (upClassType == "UltimateSDUpscale") fieldName = "upscale_by";
            else if (upClassType == "LatentUpscaleBy") fieldName = "scale_by";
            else continue;
            const auto* upInputs = upIt->second->getObj("inputs");
            if (!upInputs) continue;
            const auto* v = upInputs->find(fieldName);
            if (!v) continue;
            bool found = false;
            double n = ResolveNumberField(nodesObj, v, found, fieldName);
            if (found) { product *= n; anyResolved = true; }
        }
        if (anyResolved) local.hires_upscale = Utf8ToWstring(FormatCompactNumber(product));

        // Upscale models used by a pass layered on top of the primary one:
        // a literal file name or a link to a loader's model_name. Listed in
        // pipeline order (fewer ancestors = earlier), each name once.
        struct UpscalerUse { size_t depth; std::string name; };
        std::vector<UpscalerUse> upscalers;
        for (const auto& id : apiScope.rootAncestors) {
            if (AbortRequested()) break;
            if (id == apiScope.primary || apiScope.primaryAncestors.count(id)) continue;
            auto upIt = nodesObj->objVal.find(id);
            if (upIt == nodesObj->objVal.end() || !upIt->second || upIt->second->type != JsonType::Object) continue;
            const auto* upInputs = upIt->second->getObj("inputs");
            if (!upInputs) continue;
            const JsonValue* v = upInputs->find("upscale_model");
            if (!v) v = upInputs->find("upscale_model_opt");
            if (!v) continue;
            std::string name = (v->type == JsonType::String) ? v->strVal : ResolveTextField(nodesObj, v, "model_name");
            name = StripModelFileExtension(TrimWs(name));
            if (name.empty()) continue;
            upscalers.push_back({ComputeAncestors(nodesObj, id).size(), std::move(name)});
        }
        // Selection by smallest depth rather than std::sort: a handful of
        // entries, and the sort instantiation alone costs kilobytes. Ties keep
        // rootAncestors' (node id) order. A name already listed is skipped.
        std::string upscalerList;
        std::vector<std::string> listed;
        while (!upscalers.empty()) {
            size_t best = 0;
            for (size_t i = 1; i < upscalers.size(); i++) if (upscalers[i].depth < upscalers[best].depth) best = i;
            std::string name = std::move(upscalers[best].name);
            upscalers.erase(upscalers.begin() + best);
            if (std::find(listed.begin(), listed.end(), name) != listed.end()) continue;
            if (!upscalerList.empty()) upscalerList += ", ";
            upscalerList += name;
            listed.push_back(std::move(name));
        }
        if (!upscalerList.empty()) local.hires_upscaler = Utf8ToWstring(upscalerList);
    }

    // Loader-held values by role: whatever the primary sampler's link lands
    // on, never by output slot (layouts differ per pack and version). ckpt_name
    // only when it looks like a real model file.

    if (apiScope.active && !apiScope.primary.empty()) {
        auto primaryIt = nodesObj->objVal.find(apiScope.primary);
        const auto* primaryInputs = (primaryIt != nodesObj->objVal.end() && primaryIt->second) ?
            primaryIt->second->getObj("inputs") : nullptr;
        if (const JsonValue* li = LoaderInputsLinkedFrom(nodesObj, primaryInputs, "model")) {
            std::string resolved = li->find("ckpt_name") ? ResolveTextField(nodesObj, li->find("ckpt_name"), "ckpt_name") : "";
            if (LooksLikeModelFileName(resolved)) ui.modelName = std::move(resolved);
        }
        if (ui.modelName.empty()) {
            std::string chained = ModelFromPrimaryChain(nodesObj, primaryInputs);
            if (!chained.empty()) ui.modelName = std::move(chained);
        }
        // Noise providers other than RandomNoise (an AdvancedNoise fed by a
        // seed node, a pack's own noise class) are not sampler-family, so the
        // main loop never reads their seed. Matching by role - whatever the
        // primary sampler's "noise" input lands on - covers unknown noise nodes.
        if (!local.has_seed && primaryInputs) {
            const JsonValue* nf = primaryInputs->find("noise");
            std::string noiseId = nf ? GetLinkNodeId(nf) : "";
            auto nit = noiseId.empty() ? nodesObj->objVal.end() : nodesObj->objVal.find(noiseId);
            const JsonValue* nIn = (nit != nodesObj->objVal.end() && nit->second) ? nit->second->getObj("inputs") : nullptr;
            if (nIn) {
                const char* key = nIn->find("noise_seed") ? "noise_seed" : "seed";
                bool found = false;
                double n = ResolveNumberField(nodesObj, nIn->find(key), found, key);
                if (found) { local.seed = SimpleJson::ClampDoubleToInt64(n); local.has_seed = true; }
            }
        }
        if (widthHeight.empty()) {
            if (const JsonValue* li = LoaderInputsLinkedFrom(nodesObj, primaryInputs, "latent_image")) {
                bool wFound = false, hFound = false;
                double w = ResolveNumberField(nodesObj, li->find("empty_latent_width"), wFound);
                double h = ResolveNumberField(nodesObj, li->find("empty_latent_height"), hFound);
                if (wFound && hFound && w > 0 && h > 0) {
                    widthHeight = std::to_string(ClampDoubleToInt64(w)) + "x" + std::to_string(ClampDoubleToInt64(h));
                }
            }
        }
    }

    // A join of partly run-time-computed parts can resolve to nothing but
    // whitespace or separators (a lone " " literal); that is not a prompt, and
    // non-empty it would stop the companion text from filling the real one.
    ui.posPromptText = DropIfNoAlnum(std::move(ui.posPromptText));
    ui.negPromptText = DropIfNoAlnum(std::move(ui.negPromptText));
    local.prompt = Utf8ToWstring(ui.posPromptText);
    local.negative_prompt = Utf8ToWstring(ui.negPromptText);
    local.model = Utf8ToWstring(ui.modelName);
    local.vae = Utf8ToWstring(ui.vaeName);
    local.sampler = Utf8ToWstring(ui.samplerName);
    local.scheduler = Utf8ToWstring(ui.schedulerName);
    local.size = Utf8ToWstring(widthHeight);

    if (!loras.empty()) {
        std::string joined;
        for (const auto& l : loras) {
            if (!joined.empty()) joined += ", ";
            joined += l;
        }
        local.lora = Utf8ToWstring(joined);
    }

    local.full_parameters_utf8 = originalText;

    // Confirmed a ComfyUI graph: only now commit to the caller.
    if (outNegativeZeroed) *outNegativeZeroed = negZeroed;
    info = std::move(local);
    return true;
}
