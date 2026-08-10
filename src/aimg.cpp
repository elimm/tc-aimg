#include "contentplugin.h"
#include "metadata_parser.h"
#include "aimg_decoder.h"
#include "aimg_derived.h"
#include <windows.h>
#include <string>
#include <mutex>
#include <algorithm>
#include <cassert>
#include <deque>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Caching Mechanism for High-Speed Total Commander File Scanning
// ---------------------------------------------------------------------------
//
// A small (4-entry) LRU rather than a single slot: Total Commander queries
// every field for the *same* file one after another (one ContentGetValueW
// call per column/tooltip line) before moving to the next file in the list,
// so even one slot already turns an O(fields) re-parse per file into one
// parse + many cheap cache hits for a single pane. But TC's default UI is
// dual-pane (and can have multiple tabs per pane), and each pane/tab scrolls
// and queries fields independently; with only one slot, alternating field
// queries between two visible directories would thrash the single entry and
// force a full re-parse on every call. A handful of slots absorbs that
// without unbounded growth. The mutex is still held for the full
// parse+decode of a miss (disk read, zlib inflate, JSON parse), so misses on
// distinct files remain serialized rather than parsed in parallel -- fine
// for this plugin's access pattern, since TC drives it from field-query
// callbacks rather than a background scan pool.
struct CacheEntry {
    std::wstring path;
    AImgInfo info;
    FILETIME lastWriteTime{};
    ULARGE_INTEGER fileSize{};
    // Tick count (GetTickCount64) at which lastWriteTime/fileSize were last
    // refreshed via GetFileAttributesExW. See kStatDebounceMs below.
    ULONGLONG lastStatTick = 0;
    // At most one ft_fulltextw continuation can be in flight per thread at a
    // time (TC drives one field-query sequence to completion before starting
    // another), so a single per-thread cursor is enough: fieldIndex identifies
    // which field it belongs to, offset is how far it's read. A query for a
    // *different* field than the stored one means the prior sequence was
    // abandoned mid-string (TC stopped calling before reaching the end); the
    // new field simply overwrites the stale entry instead of resuming from it.
    struct FulltextCursor {
        int fieldIndex = -1;
        size_t offset = 0;
    };
    std::unordered_map<uint64_t /*threadId*/, FulltextCursor> fulltextCursors;
};

static const size_t kCacheCapacity = 4;

// Total Commander queries every field of a file (up to g_FieldCount calls)
// one after another before moving to the next file, so re-statting on every
// single field query turns one syscall per file into g_FieldCount syscalls
// per file. Trusting a just-read stamp for a short window collapses that
// back to ~one stat per file/row for the common case, at the cost of not
// noticing an external modification that happens to land inside the window
// -- an acceptable tradeoff for a plugin driven by interactive column/tooltip
// queries, not a background integrity scanner.
static const ULONGLONG kStatDebounceMs = 200;

static std::mutex g_CacheMutex;
static std::deque<CacheEntry> g_Cache; // front = most recently used

// Cheap existence+staleness stamp via a single attribute-query syscall (no
// file handle opened), so a cache hit costs one API call instead of a full
// re-parse even when a file might have changed since it was cached.
static bool GetFileStamp(const std::wstring& filePath, FILETIME& outTime, ULARGE_INTEGER& outSize) {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &attr)) return false;
    outTime = attr.ftLastWriteTime;
    outSize.LowPart = attr.nFileSizeLow;
    outSize.HighPart = attr.nFileSizeHigh;
    return true;
}

// Caller must hold g_CacheMutex. Leaves the requested path's entry at
// g_Cache.front() with valid info; callers read it directly while still
// holding the lock, avoiding a full AImgInfo copy (~15 std::wstring members)
// on every single-field query.
static void EnsureCached(const std::wstring& filePath) {
    ULONGLONG now = GetTickCount64();

    for (auto it = g_Cache.begin(); it != g_Cache.end(); ++it) {
        if (_wcsicmp(it->path.c_str(), filePath.c_str()) != 0) continue; // Windows paths are case-insensitive

        // Within the debounce window, trust the stamp already stored on this
        // entry instead of re-statting -- this is the fast path that turns
        // TC's per-field burst of queries into a single syscall per row.
        bool stale;
        if (now - it->lastStatTick < kStatDebounceMs) {
            stale = false;
        } else {
            FILETIME writeTime{};
            ULARGE_INTEGER size{};
            bool haveStamp = GetFileStamp(filePath, writeTime, size);
            // A path match is only a real hit if the file hasn't changed on
            // disk since it was cached (e.g. re-saved by another tool between
            // two field queries for the same row); otherwise fall through and
            // re-parse.
            stale = !haveStamp ||
                    CompareFileTime(&it->lastWriteTime, &writeTime) != 0 ||
                    it->fileSize.QuadPart != size.QuadPart;
            it->lastStatTick = now;
            if (haveStamp) {
                it->lastWriteTime = writeTime;
                it->fileSize = size;
            }
        }

        if (!stale) {
            if (it != g_Cache.begin()) {
                CacheEntry hit = std::move(*it);
                g_Cache.erase(it);
                g_Cache.push_front(std::move(hit));
            }
            return;
        }
        g_Cache.erase(it);
        break;
    }

    FILETIME writeTime{};
    ULARGE_INTEGER size{};
    GetFileStamp(filePath, writeTime, size);
    ULONGLONG statTick = GetTickCount64();

    AImgInfo info;
    try {
        RawImageMetadata rawMeta;
        if (MetadataParser::ExtractMetadata(filePath, rawMeta)) {
            info = AImgDecoder::Decode(rawMeta);
        }
    } catch (...) {
        // Never let an exception escape across the WDX __stdcall boundary into
        // Total Commander's process; fall through and cache an empty result.
        info = AImgInfo();
    }

    CacheEntry entry;
    entry.path = filePath;
    entry.info = std::move(info);
    entry.lastWriteTime = writeTime;
    entry.fileSize = size;
    entry.lastStatTick = statTick;
    g_Cache.push_front(std::move(entry));
    if (g_Cache.size() > kCacheCapacity) g_Cache.pop_back();
}


// Writes one chunk of a ft_fulltextw field, honoring TC's continuation
// protocol: repeated ContentGetValueW calls for the same field must each
// advance a cursor and return ft_fulltextw while more text remains, then
// return ft_stringw (no fulltext flag) on the final chunk so TC knows to
// stop calling. Without a per-field cursor, every call would restart from
// offset 0 and TC would loop, concatenating the same leading chunk over and
// over until it hit its own safety cap -- this was the ~5MB duplication bug
// seen on ComfyUI PNGs before this cursor was added.
static int WriteFulltextChunk(CacheEntry& entry, int fieldIndex, const std::wstring& text, void* FieldValue, int maxlen) {
    uint64_t threadId = GetCurrentThreadId();

    // A stored cursor for a *different* field means that prior continuation
    // sequence was abandoned mid-string (TC stopped calling before reaching
    // the end); starting fresh for this field is correct either way.
    CacheEntry::FulltextCursor& cursor = entry.fulltextCursors[threadId];
    if (cursor.fieldIndex != fieldIndex) {
        cursor.fieldIndex = fieldIndex;
        cursor.offset = 0;
    }

    if (maxlen <= 0) return ft_fieldempty;
    size_t maxChars = static_cast<size_t>(maxlen) / sizeof(WCHAR);
    if (maxChars == 0) return ft_fieldempty;

    size_t toCopy = std::min<size_t>(text.size() - cursor.offset, maxChars - 1);
    memcpy(FieldValue, text.c_str() + cursor.offset, toCopy * sizeof(WCHAR));
    ((WCHAR*)FieldValue)[toCopy] = L'\0';
    cursor.offset += toCopy;

    if (cursor.offset >= text.size()) {
        entry.fulltextCursors.erase(threadId); // done; next query for any field starts fresh
        return ft_stringw;
    }
    return ft_fulltextw;
}

// Shared body for every plain ft_stringw field: empty source -> ft_fieldempty,
// otherwise truncate-and-NUL-terminate into FieldValue. Factored out so the
// truncate/NUL-terminate contract that ContentGetValue's ANSI reconstruction
// depends on is enforced in one place instead of copy-pasted per field.
static int WriteStringField(const std::wstring& value, void* FieldValue, int maxlen) {
    if (value.empty()) return ft_fieldempty;
    wcsncpy_s((WCHAR*)FieldValue, maxlen / sizeof(WCHAR), value.c_str(), _TRUNCATE);
    return ft_stringw;
}

// Shared body for every fixed-size numeric field (ft_numeric_32/64/ft_float):
// absent -> ft_fieldempty, otherwise a raw write of the typed value into
// FieldValue (numeric fields have no truncation/maxlen concern, unlike
// strings). Factored out to remove the copy-pasted has/write/return triple
// previously repeated per numeric field.
template <typename T>
static int WriteNumericField(bool has, T value, int ftType, void* FieldValue) {
    if (!has) return ft_fieldempty;
    *(T*)FieldValue = value;
    return ftType;
}

// ---------------------------------------------------------------------------
// Field definitions
// ---------------------------------------------------------------------------
//
// One row per field: name/type/units (what ContentGetSupportedFieldW reports)
// AND the value-extraction logic (what ContentGetValueW returns) live
// together here, indexed exactly the way Total Commander addresses a field --
// by FieldIndex into this single array. Previously these were two
// hand-synced pieces (this table for metadata, a separate switch(FieldIndex)
// in ContentGetValueW for values); a new field appended to one without a
// matching entry in the other silently fell through to ft_nosuchfield/
// ft_fieldempty instead of a build error. Extractors are captureless lambdas
// (decay to plain function pointers, no std::function overhead) so adding a
// field is one self-contained row instead of two edits in two places.
using FieldExtractor = int (*)(const AImgInfo&, CacheEntry&, int fieldIndex, void* FieldValue, int maxlen);

struct FieldDef {
    const wchar_t* name;
    int type;
    const wchar_t* units;
    FieldExtractor extract;
    int flags = 0;
};

static const FieldDef g_Fields[] = {
    { L"Generator", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.generator, fv, maxlen);
    } }, // 0
    { L"Prompt", ft_fulltextw, L"", [](const AImgInfo& info, CacheEntry& ce, int fi, void* fv, int maxlen) {
        if (info.prompt.empty()) return ft_fieldempty;
        return WriteFulltextChunk(ce, fi, info.prompt, fv, maxlen);
    }, contflags_fieldhint }, // 1
    { L"Prompt (Short)", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.prompt, fv, maxlen);
    } }, // 2
    { L"Negative Prompt", ft_fulltextw, L"", [](const AImgInfo& info, CacheEntry& ce, int fi, void* fv, int maxlen) {
        if (info.negative_prompt.empty()) return ft_fieldempty;
        return WriteFulltextChunk(ce, fi, info.negative_prompt, fv, maxlen);
    }, contflags_fieldhint }, // 3
    { L"Negative Prompt (Short)", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.negative_prompt, fv, maxlen);
    } }, // 4
    { L"Model", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.model, fv, maxlen);
    } }, // 5
    { L"Model Hash", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.model_hash, fv, maxlen);
    } }, // 6
    { L"Seed", ft_numeric_64, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<__int64>(info.has_seed, info.seed, ft_numeric_64, fv);
    } }, // 7
    { L"CFG Scale", ft_float, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<double>(info.has_cfg, info.cfg_scale, ft_float, fv);
    } }, // 8
    { L"Steps", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<int>(info.has_steps, info.steps, ft_numeric_32, fv);
    } }, // 9
    { L"Sampler", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.sampler, fv, maxlen);
    } }, // 10
    { L"Scheduler", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.scheduler, fv, maxlen);
    } }, // 11
    { L"Clip Skip", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<int>(info.has_clip_skip, info.clip_skip, ft_numeric_32, fv);
    } }, // 12
    { L"Size", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.size, fv, maxlen);
    } }, // 13
    { L"Aspect Ratio", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(ComputeAspectRatio(info.size), fv, maxlen);
    } }, // 14
    { L"Megapixels", ft_float, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        double mp = 0.0;
        if (!ComputeMegapixels(info.size, mp)) return ft_fieldempty;
        *(double*)fv = mp;
        return ft_float;
    } }, // 15
    { L"Denoising Strength", ft_float, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<double>(info.has_denoising_strength, info.denoising_strength, ft_float, fv);
    } }, // 16
    { L"Hires Upscale", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.hires_upscale, fv, maxlen);
    } }, // 17
    { L"Hires Upscaler", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.hires_upscaler, fv, maxlen);
    } }, // 18
    { L"Hires Steps", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<int>(info.has_hires_steps, info.hires_steps, ft_numeric_32, fv);
    } }, // 19
    { L"VAE", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.vae, fv, maxlen);
    } }, // 20
    { L"LoRA", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.lora, fv, maxlen);
    } }, // 21
    { L"LoRA Count", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        int count = CountLoraEntries(info.lora);
        return WriteNumericField<int>(count != 0, count, ft_numeric_32, fv);
    } }, // 22
    { L"Full Parameters", ft_fulltextw, L"", [](const AImgInfo& info, CacheEntry& ce, int fi, void* fv, int maxlen) {
        if (info.full_parameters.empty()) return ft_fieldempty;
        return WriteFulltextChunk(ce, fi, info.full_parameters, fv, maxlen);
    }, contflags_fieldhint }, // 23
    // Extractor is only reachable via ContentGetValueW's own FieldIndex == 24
    // special case below (it must run BEFORE the has_metadata guard, since
    // this field's whole point is to report has_metadata itself, including
    // the false case) -- present here anyway so this row is still the
    // complete, self-contained definition of the field.
    { L"Has AI Metadata", ft_boolean, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        *(int*)fv = info.has_metadata ? 1 : 0;
        return ft_boolean;
    } }, // 24
};

static const int g_FieldCount = sizeof(g_Fields) / sizeof(g_Fields[0]);

int __stdcall ContentGetSupportedFieldW(int FieldIndex, WCHAR* FieldName, WCHAR* Units, int maxlen) {
    if (FieldIndex < 0 || FieldIndex >= g_FieldCount) return ft_nosuchfield;

    wcsncpy_s(FieldName, maxlen, g_Fields[FieldIndex].name, _TRUNCATE);
    wcsncpy_s(Units, maxlen, g_Fields[FieldIndex].units, _TRUNCATE);

    return g_Fields[FieldIndex].type | g_Fields[FieldIndex].flags;
}

int __stdcall ContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen) {
    if (FieldIndex < 0 || FieldIndex >= g_FieldCount) return ft_nosuchfield;

    WideCharToMultiByte(CP_ACP, 0, g_Fields[FieldIndex].name, -1, FieldName, maxlen, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_Fields[FieldIndex].units, -1, Units, maxlen, NULL, NULL);

    int type = g_Fields[FieldIndex].type;
    int flags = g_Fields[FieldIndex].flags;
    if (type == ft_stringw) return ft_string | flags;
    if (type == ft_fulltextw) return ft_fulltext | flags;
    return type | flags;
}

int __stdcall ContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    UNREFERENCED_PARAMETER(UnitIndex); // all fields are single-unit
    UNREFERENCED_PARAMETER(flags);     // no flag-dependent formatting (e.g. ft_datetime) needed
    if (!FileName || FieldIndex < 0 || FieldIndex >= g_FieldCount) return ft_nosuchfield;
    if (maxlen <= 0) return ft_fieldempty; // guard against a corrupt/negative size before any buffer write

    std::lock_guard<std::mutex> lock(g_CacheMutex);
    EnsureCached(FileName);
    CacheEntry& cacheEntry = g_Cache.front();
    const AImgInfo& info = cacheEntry.info;

    // Handled before the has_metadata guard below: this field's whole point
    // is to report has_metadata itself, including the false case.
    if (FieldIndex == 24) {
        return g_Fields[FieldIndex].extract(info, cacheEntry, FieldIndex, FieldValue, maxlen);
    }
    if (!info.has_metadata) return ft_fieldempty;

    return g_Fields[FieldIndex].extract(info, cacheEntry, FieldIndex, FieldValue, maxlen);
}

int __stdcall ContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    if (!FileName) return ft_nosuchfield;
    int reqLen = MultiByteToWideChar(CP_ACP, 0, FileName, -1, NULL, 0);
    if (reqLen <= 0) return ft_fileerror;

    std::wstring wFileName(reqLen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, FileName, -1, &wFileName[0], reqLen);

    int res = ContentGetValueW(&wFileName[0], FieldIndex, UnitIndex, FieldValue, maxlen, flags);
    if (res == ft_stringw || res == ft_fulltextw) {
        // Convert string back to ANSI for old ContentGetValue caller. This is
        // an inherently lossy legacy path for non-ANSI-codepage text (e.g.
        // Cyrillic prompts on a Western codepage); WC_NO_BEST_FIT_CHARS makes
        // unrepresentable characters map to a plain '?' instead of a
        // deceptive "best fit" substitution (e.g. some accented letters
        // silently turning into different plain-ASCII letters).
        //
        // std::wstring((WCHAR*)FieldValue) below reads until the first NUL,
        // which is safe only because every ContentGetValueW string case
        // NUL-terminates within bounds: the wcsncpy_s(..., _TRUNCATE) cases
        // either copy the full source or truncate-and-terminate, and
        // WriteFulltextChunk's manual memcpy always writes its own
        // terminator within maxlen. If a future ft_stringw/ft_fulltextw case
        // ever fills FieldValue by some other means, it must preserve that
        // same guarantee or this reconstruction will over-read.
        std::wstring wstr((WCHAR*)FieldValue);
        assert(wcsnlen(wstr.c_str(), maxlen / sizeof(WCHAR)) < (size_t)(maxlen / sizeof(WCHAR)));
        BOOL usedDefault = FALSE;
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wstr.c_str(), -1, (char*)FieldValue, maxlen, NULL, &usedDefault);
        UNREFERENCED_PARAMETER(usedDefault); // lossy-conversion signal not currently surfaced to any caller
        return (res == ft_stringw) ? ft_string : ft_fulltext;
    }
    return res;
}

int __stdcall ContentGetDetectString(char* DetectString, int maxlen) {
    const char* str = "EXT=\"PNG\" | EXT=\"JPG\" | EXT=\"JPEG\" | EXT=\"WEBP\" | EXT=\"AVIF\" | EXT=\"TIFF\" | EXT=\"TIF\"";
    strncpy_s(DetectString, maxlen, str, _TRUNCATE);
    return 0;
}

void __stdcall ContentSetDefaultParams(ContentDefaultParamStruct* dps) {
    UNREFERENCED_PARAMETER(dps); // no persisted default settings to initialize from
}

void __stdcall ContentSetDefaultParamsW(ContentDefaultParamStructW* dps) {
    UNREFERENCED_PARAMETER(dps);
}

void __stdcall ContentSendStateInformation(int state, char* path) {
    UNREFERENCED_PARAMETER(state); // no per-directory cache invalidation needed: every g_Cache entry is staleness-checked by file stamp on each access
    UNREFERENCED_PARAMETER(path);
}

void __stdcall ContentSendStateInformationW(int state, WCHAR* path) {
    UNREFERENCED_PARAMETER(state);
    UNREFERENCED_PARAMETER(path);
}
