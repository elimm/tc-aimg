#include "aimg_decoder.h"
#include "aimg_decoder_internal.h"
#include "aimg_abort.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <cwchar>
#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <memory>

// Its own translation unit: the types here (UiLinkIndex/SubgraphRegistry/
// BoundaryOutcome/WidgetFieldMap) are touched by nothing else. The shared
// surface is deliberately thin -- see aimg_decoder_internal.h -- and
// everything else keeps internal linkage.
using AImgDecoderInternal::FormatCompactNumber;

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

// Guards the UI-format LoRA collector's positional fallback: a node whose
// type merely CONTAINS "Lora" (a save node, not a loader) can still have a
// String first widget -- a filename-prefix template, not a LoRA name.
bool LooksLikeModelFileName(const std::string& s) {
    static const char* const kExtensions[] = {".safetensors", ".ckpt", ".pt", ".pth", ".bin", ".sft"};
    for (const char* ext : kExtensions) {
        size_t extLen = strlen(ext);
        if (s.size() < extLen) continue;
        if (std::equal(s.end() - extLen, s.end(), ext, ext + extLen,
                        [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); })) {
            return true;
        }
    }
    return false;
}

// A graph link is a 2-element array [node_id, output_slot].
std::string GetLinkNodeId(const JsonValue* v) {
    if (!v || v->type != JsonType::Array || v->arrVal.empty()) return "";
    return JsonIdToString(v->arrVal[0].get());
}

// Follows an API-format field that may be a literal OR a link through a
// bounded chain of primitive nodes to the underlying literal string. Many
// graphs route a CLIPTextEncode's "text" (or a latent node's width/height)
// through a separate primitive node, where a plain getStr()/getNum() sees
// only the link array and comes back empty.
double ResolveNumberField(const JsonValue* nodesObj, const JsonValue* field, bool& found, const char* preferredKey = nullptr, int depth = 0);

std::string ResolveTextField(const JsonValue* nodesObj, const JsonValue* field, const char* preferredKey = nullptr, int depth = 0) {
    if (!field || depth > 4) return "";
    if (field->type == JsonType::String) return field->strVal;
    if (field->type == JsonType::Number) {
        // A numeric literal feeding a text field: stringify, don't lose it.
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
        return a + sep + b;
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

// Resolves the exact node a KSampler's "positive"/"negative" link points at,
// which beats the "first/second text-encode found" guess once a graph has
// more than two (regional prompting, IP-adapters). Some custom nodes expose
// "prompt" and "negative_prompt" on the SAME node instead of a shared
// "text"; isPositive picks between them.
std::string ResolveClipText(const JsonValue* nodesObj, const std::string& nodeId, bool isPositive) {
    if (nodeId.empty()) return "";
    auto it = nodesObj->objVal.find(nodeId);
    if (it == nodesObj->objVal.end() || !it->second) return "";
    const auto* refInputs = it->second->getObj("inputs");
    if (!refInputs) return "";
    // A tag-concatenating node with three STRING inputs and no "text" field
    // of its own. Real files carry their own trailing punctuation, so join
    // with nothing rather than guessing a delimiter.
    if (it->second->getStr("class_type").find("Get Booru Tag") != std::string::npos) {
        std::string joined;
        for (const char* key : {"text_a", "text_b", "text_c"}) {
            if (const JsonValue* v = refInputs->find(key)) joined += ResolveTextField(nodesObj, v);
        }
        if (!joined.empty()) return joined;
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
                if (l->arrVal.size() > 2 && l->arrVal[2] && l->arrVal[2]->type == JsonType::Number) originSlot = (int)l->arrVal[2]->numVal;
                if (l->arrVal.size() > 3 && l->arrVal[3]) targetId = JsonIdToString(l->arrVal[3].get());
                if (l->arrVal.size() > 4 && l->arrVal[4] && l->arrVal[4]->type == JsonType::Number) targetSlot = (int)l->arrVal[4]->numVal;
            } else if (l->type == JsonType::Object) {
                // Subgraph-capable versions store each link as an object
                // instead. Unhandled, every link in such a workflow is
                // dropped and no link resolution fires at all.
                const JsonValue* idv = l->find("id");
                const JsonValue* originv = l->find("origin_id");
                if (!idv || idv->type != JsonType::Number || !originv) continue;
                linkId = ClampDoubleToInt64(idv->numVal);
                originId = JsonIdToString(originv);
                if (const JsonValue* originSlotField = l->find("origin_slot")) {
                    if (originSlotField->type == JsonType::Number) originSlot = (int)originSlotField->numVal;
                }
                if (const JsonValue* targetIdField = l->find("target_id")) targetId = JsonIdToString(targetIdField);
                if (const JsonValue* targetSlotField = l->find("target_slot")) {
                    if (targetSlotField->type == JsonType::Number) targetSlot = (int)targetSlotField->numVal;
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
    int mode = (int)m->numVal;
    return mode == 2 || mode == 4;
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

} // namespace

bool AImgDecoder::DecodeComfyUI(const SimpleJson::JsonValue* root, const std::string& originalText, AImgInfo& info) {
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
    ComfyUiExtraction ui;

    // 1. API Graph Format Traversal
    for (const auto& pair : nodesObj->objVal) {
        if (AbortRequested()) break; // ContentStopGetValueW was called for this file; stop walking and let the caller discard the result

        const auto* node = pair.second.get();
        if (!node || node->type != SimpleJson::JsonType::Object) continue;

        std::string classType = node->getStr("class_type");
        const auto* inputs = node->getObj("inputs");
        if (!inputs) continue;
        if (!classType.empty()) looksLikeComfyGraph = true;

        // "Custom sampler" graphs split one KSampler into single-purpose
        // nodes wired together -- RandomNoise (seed), CFGGuider (cfg +
        // positive/negative), KSamplerSelect (sampler), a scheduler
        // (steps/denoise) -- so dispatch on key presence rather than a
        // literal node name. "UltimateSDUpscale" is the same shape but its
        // class_type contains neither "Sampler" nor "Scheduler", and a
        // workflow using it for the whole generation would otherwise leave
        // every one of these fields empty.
        if (classType == "KSampler" || classType == "KSamplerAdvanced" || classType == "KSamplerSelect" ||
            classType == "CFGGuider" || classType == "RandomNoise" || classType == "UltimateSDUpscale" ||
            classType.find("Sampler") != std::string::npos || classType.find("Scheduler") != std::string::npos) {
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
            }
            if (negativeNodeId.empty()) {
                if (const auto* v = inputs->find("negative")) negativeNodeId = GetLinkNodeId(v);
            }
        }
        // The "Efficient Loader" carries the positive/negative links on the
        // LOADER, not the sampler: its samplers take one bundled pipe input
        // with no positive/negative fields, so the gate above never sees
        // them.
        else if (classType.find("Efficient Loader") != std::string::npos) {
            if (positiveNodeId.empty()) {
                if (const auto* v = inputs->find("positive")) positiveNodeId = GetLinkNodeId(v);
            }
            if (negativeNodeId.empty()) {
                if (const auto* v = inputs->find("negative")) negativeNodeId = GetLinkNodeId(v);
            }
        }

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

        if (classType == "VAELoader") {
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
            // and keep only active ones. "strength" is inconsistently typed
            // within one real file, so accept a Number or a numeric String
            // rather than silently reading 0.
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
                        else if (strengthField->type == JsonType::String && !strengthField->strVal.empty()) strength = atof(strengthField->strVal.c_str());
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
                    if (name.empty() || name == "None") continue;
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
            if (widthField && heightField) {
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
    std::string resolvedPos = ResolveClipText(nodesObj, positiveNodeId, /*isPositive=*/true);
    std::string resolvedNeg = ResolveClipText(nodesObj, negativeNodeId, /*isPositive=*/false);
    if (!resolvedPos.empty()) ui.posPromptText = resolvedPos;
    if (!resolvedNeg.empty()) ui.negPromptText = resolvedNeg;

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
    info = std::move(local);
    return true;
}
