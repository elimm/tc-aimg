#ifndef AIMG_DERIVED_H
#define AIMG_DERIVED_H

// Pure helpers for WDX fields derived from an already-decoded AImgInfo
// (Size, LoRA) rather than parsed from raw metadata. Header-only and
// dependency-free so they can be exercised without linking aimg.cpp.

#include <string>
#include <algorithm>
#include <cmath>

// Parses the "Size" field's "WxH" shape. False on empty, malformed, or
// zero/negative dimensions.
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

// `lora` is NormalizeLoraField's ", "-joined output, so every separator is a
// literal comma.
inline int CountLoraEntries(const std::wstring& lora) {
    if (lora.empty()) return 0;
    return static_cast<int>(std::count(lora.begin(), lora.end(), L',')) + 1;
}

#endif // AIMG_DERIVED_H
