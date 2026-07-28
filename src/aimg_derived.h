#ifndef AIMG_DERIVED_H
#define AIMG_DERIVED_H

// Pure helpers for WDX fields that are computed on the fly from an already-
// decoded AImgInfo (Size, LoRA) rather than parsed from raw metadata. Kept
// header-only and dependency-free (just <string>) so tests/test_parser.cpp
// can exercise them directly without linking aimg.cpp or loading the built
// DLL.

#include <string>
#include <algorithm>
#include <cmath>

// Parses the "Size" field's "WxH" shape (as written by every decoder in
// aimg_decoder.cpp) into positive width/height. Returns false on empty,
// malformed, or zero/negative dimensions.
inline bool ParseSize(const std::wstring& size, long& w, long& h) {
    size_t xPos = size.find(L'x');
    if (xPos == std::wstring::npos) return false;
    try {
        size_t wEnd = 0, hEnd = 0;
        w = std::stol(size.substr(0, xPos), &wEnd);
        h = std::stol(size.substr(xPos + 1), &hEnd);
        if (wEnd != xPos) return false; // trailing junk before 'x'
        if (xPos + 1 + hEnd != size.size()) return false; // trailing junk after height
    } catch (...) {
        return false;
    }
    return w > 0 && h > 0;
}

inline long Gcd(long a, long b) {
    while (b != 0) {
        long t = b;
        b = a % b;
        a = t;
    }
    return a;
}

// Returns "" if `size` doesn't parse as a positive "WxH".
inline std::wstring ComputeAspectRatio(const std::wstring& size) {
    long w = 0, h = 0;
    if (!ParseSize(size, w, h)) return L"";
    long g = Gcd(w, h);
    return std::to_wstring(w / g) + L":" + std::to_wstring(h / g);
}

// Returns false if `size` doesn't parse as a positive "WxH"; otherwise fills
// `outMp` with megapixels rounded to 2 decimal places.
inline bool ComputeMegapixels(const std::wstring& size, double& outMp) {
    long w = 0, h = 0;
    if (!ParseSize(size, w, h)) return false;
    outMp = std::round((static_cast<double>(w) * h / 1000000.0) * 100.0) / 100.0;
    return true;
}

// `lora` is the already-normalized ", "-joined list AImgDecoder::Decode
// produces (see NormalizeLoraField), so every separator is a literal comma.
// Returns 0 for an empty list.
inline int CountLoraEntries(const std::wstring& lora) {
    if (lora.empty()) return 0;
    return static_cast<int>(std::count(lora.begin(), lora.end(), L',')) + 1;
}

#endif // AIMG_DERIVED_H
