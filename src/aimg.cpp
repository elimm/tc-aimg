#include "contentplugin.h"
#include "metadata_parser.h"
#include "aimg_decoder.h"
#include "aimg_derived.h"
#include "aimg_abort.h"
#include <windows.h>
#include <string>
#include <algorithm>
#include <cassert>
#include <deque>
#include <vector>
#include <memory>
#include <atomic>
#include <cwctype>

// ---------------------------------------------------------------------------
// LRU cache
// ---------------------------------------------------------------------------
//
// Four entries, not one: TC queries every field of a file in a burst before
// moving on, so a single slot already collapses O(fields) re-parses into one
// parse -- but its dual-pane UI scrolls two directories independently, and
// alternating queries between them would thrash a single slot into a
// re-parse per call. A handful of slots absorbs that without unbounded
// growth.
struct CacheEntry {
    std::wstring path;
    AImgInfo info;
    FILETIME lastWriteTime{};
    ULARGE_INTEGER fileSize{};
    ULONGLONG lastStatTick = 0; // when the stamp above was last refreshed; see kStatDebounceMs
    // Lazy UTF-16 form of full_parameters_utf8, for the "Full Parameters"
    // field only. Lives here rather than on AImgInfo (the decoder's output,
    // which knows nothing about display) so it is built at most once per
    // cached file and invalidation is free -- a stale entry is erased whole,
    // resetting the flag with it.
    std::wstring fullParamsW;
    bool fullParamsWBuilt = false;
};

static const size_t kCacheCapacity = 4;

// Without this, TC's per-file burst of field queries costs one stat syscall
// each. Trusting a just-read stamp for a short window collapses that to ~one
// stat per row, at the cost of missing an external modification landing
// inside the window -- fine for interactive column queries.
static const ULONGLONG kStatDebounceMs = 200;

// SRWLOCK instead of std::mutex: in the /MT build std::mutex drags in
// std::system_error/FormatMessageA (~10 KB) for what is just an exclusive lock.
struct SrwLock {
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
struct SrwGuard {
    SrwLock& m;
    explicit SrwGuard(SrwLock& lk) : m(lk) { m.lock(); }
    ~SrwGuard() { m.unlock(); }
    SrwGuard(const SrwGuard&) = delete;
    SrwGuard& operator=(const SrwGuard&) = delete;
};

static SrwLock g_CacheMutex;
static std::deque<CacheEntry> g_Cache; // front = most recently used

// Existence + staleness stamp from one attribute query, no file handle.
static bool GetFileStamp(const std::wstring& filePath, FILETIME& outTime, ULARGE_INTEGER& outSize) {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &attr)) return false;
    outTime = attr.ftLastWriteTime;
    outSize.LowPart = attr.nFileSizeLow;
    outSize.HighPart = attr.nFileSizeHigh;
    return true;
}

// Caller must hold g_CacheMutex. On a fresh hit, moves the entry to
// g_Cache.front() and returns true -- the caller then reads it in place,
// still under the lock, avoiding a ~15-wstring AImgInfo copy per query. A
// miss or a stale entry (erased here) returns false.
//
// Lookup is deliberately split from InsertFresh so the parse can run with no
// lock held: holding g_CacheMutex across a disk read + inflate + JSON parse
// serialized misses on *different* files from different panes.
static bool FindFresh(const std::wstring& filePath) {
    ULONGLONG now = GetTickCount64();

    for (auto it = g_Cache.begin(); it != g_Cache.end(); ++it) {
        if (_wcsicmp(it->path.c_str(), filePath.c_str()) != 0) continue; // Windows paths are case-insensitive

        // Inside the debounce window, trust the stored stamp instead of
        // re-statting.
        bool stale;
        if (now - it->lastStatTick < kStatDebounceMs) {
            stale = false;
        } else {
            FILETIME writeTime{};
            ULARGE_INTEGER size{};
            bool haveStamp = GetFileStamp(filePath, writeTime, size);
            // A path match is only a hit if the bytes haven't changed since
            // it was cached; otherwise fall through and re-parse.
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
            return true;
        }
        g_Cache.erase(it);
        break;
    }

    return false;
}

// Caller must hold g_CacheMutex.
static void InsertFresh(CacheEntry&& entry) {
    g_Cache.push_front(std::move(entry));
    if (g_Cache.size() > kCacheCapacity) g_Cache.pop_back();
}

// ---------------------------------------------------------------------------
// CONTENT_DELAYIFSLOW policy
// ---------------------------------------------------------------------------
//
// Per docs/contentgetvalue.htm, answering ft_delayed is what makes TC retry
// on a background thread. Below this size a PNG/JPEG parse is measured in
// tens of microseconds, so delaying would be pure overhead and flicker; the
// threshold exists for the containers and paths where slow is plausible.
static const ULONGLONG kDelaySizeThreshold = 2ULL * 1024 * 1024;

// Decided without parsing, from the already-fetched size and the drive type.
// Size is a WEAK proxy on PNG/JPEG (both seekg() past the pixel payload that
// dominates the file size) but a good one for WebP/AVIF/TIFF, which read a
// bounded buffer regardless, and for anything not on a local fixed drive,
// where the read itself stalls. Errs toward delaying a file that would have
// been fast over freezing TC's thread on one that is slow.
static bool LooksSlow(const std::wstring& filePath, const ULARGE_INTEGER& fileSize) {
    if (fileSize.QuadPart >= (ULONGLONG)kDelaySizeThreshold) return true;

    // A UNC path is never local by definition; no need to ask the OS.
    if (filePath.size() >= 2 && filePath[0] == L'\\' && filePath[1] == L'\\') return true;

    // GetDriveTypeW wants just the root ("C:\"), not the full file path.
    if (filePath.size() >= 2 && filePath[1] == L':') {
        std::wstring root = filePath.substr(0, 2) + L"\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// ContentStopGetValue(W) abort support
// ---------------------------------------------------------------------------
//
// Registry of parses running on a background thread, keyed by lowercased
// path. Guarded by its OWN mutex, never g_CacheMutex: a stop request must
// not be able to block on, or deadlock against, a parse that is itself
// waiting on the cache lock.
static SrwLock g_InFlightMutex;
// Flat vector with a linear scan, not a std::map: at most a handful of
// parses are ever in flight, and a wstring-keyed tree instantiates a whole
// map specialization to search ~2 elements.
//
// Duplicate keys are legal -- two threads parsing one path is the accepted
// cost of two-phase locking -- so there is deliberately no lookup-by-key
// helper: the registration guard must erase ITS OWN entry, and a stop
// request must signal ALL of them.
static std::vector<std::pair<std::wstring, std::shared_ptr<std::atomic<bool>>>> g_InFlight;

// Only needs to compare equal for one path spelled two ways, not to be a
// correct Unicode casefold. towlower rather than CharLowerBuffW, which lives
// in user32.dll and would break this plugin's KERNEL32-only import table.
static std::wstring ToLowerPathKey(const std::wstring& path) {
    std::wstring lower(path);
    for (wchar_t& c : lower) c = (wchar_t)towlower((wint_t)c);
    return lower;
}


// One chunk of an ft_fulltextw field. Per docs/contentgetvalue.htm the
// CALLER drives the offset: TC starts at UnitIndex 0 and repeats with it
// advanced by maxlen-1 bytes until the plugin answers ft_fieldempty. The
// plugin must therefore keep NO cursor of its own -- unitIndex IS the state.
// A plugin-side cursor restarts at 0 whenever another field is queried in
// between (TC asks for many fields per file), desynchronising from what TC
// believes it already read; that was the cause of a historical multi-MB
// duplication bug. Stateless removes the whole class of it.
static int WriteFulltextChunk(const std::wstring& text, int unitIndex, void* FieldValue, int maxlen) {
    if (text.empty()) return ft_fieldempty;
    if (unitIndex < 0) return ft_fieldempty; // defence in depth; ContentGetValueW already answers this before reaching here

    // maxlen is bytes, not characters; need one wide char plus its NUL.
    if (maxlen < (int)(sizeof(WCHAR) * 2)) return ft_fieldempty;

    // TC advances the byte offset by maxlen-1, which is odd for a wide-char
    // buffer. Rounding down overlaps the previous chunk by one code unit --
    // harmless, and the same intent as TC's own stepping: a search match must
    // never be able to fall exactly on a block boundary.
    size_t byteOffset = (size_t)unitIndex & ~(size_t)1;
    size_t charOffset = byteOffset / sizeof(WCHAR);

    // Never start a chunk in the middle of a surrogate pair.
    if (charOffset > 0 && charOffset < text.size() &&
        text[charOffset] >= 0xDC00 && text[charOffset] <= 0xDFFF) {
        charOffset--;
    }

    if (charOffset >= text.size()) return ft_fieldempty; // the documented end-of-data signal

    size_t maxChars = (size_t)maxlen / sizeof(WCHAR);
    size_t toCopy = std::min<size_t>(text.size() - charOffset, maxChars - 1);

    // Never split a surrogate pair across the end of this chunk either.
    if (toCopy > 0 && charOffset + toCopy < text.size()) {
        WCHAR last = text[charOffset + toCopy - 1];
        if (last >= 0xD800 && last <= 0xDBFF) toCopy--;
    }

    // Only when the buffer holds exactly one wide char and the guard above
    // just took it away as a lone surrogate half. Answering ft_fulltextw with
    // an empty chunk would livelock TC at this same offset forever.
    if (toCopy == 0) return ft_fieldempty;

    memcpy(FieldValue, text.c_str() + charOffset, toCopy * sizeof(WCHAR));
    ((WCHAR*)FieldValue)[toCopy] = L'\0';

    // Even the final chunk returns ft_fulltextw -- this protocol has no
    // terminator type. The NEXT call, past the end, answers ft_fieldempty.
    return ft_fulltextw;
}

// Shared by every ft_stringw field, so the NUL-termination contract that
// ContentGetValue's ANSI reconstruction relies on lives in one place.
static int WriteStringField(const std::wstring& value, void* FieldValue, int maxlen) {
    if (value.empty()) return ft_fieldempty;
    // wcsncpy_s with a zero-sized destination reaches the CRT's
    // invalid-parameter handler, which abort()s inside TC's own process.
    if (maxlen < (int)(sizeof(WCHAR) * 2)) return ft_fieldempty;
    wcsncpy_s((WCHAR*)FieldValue, maxlen / sizeof(WCHAR), value.c_str(), _TRUNCATE);
    return ft_stringw;
}

// Shared by every fixed-size numeric field; no maxlen concern, unlike
// strings.
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
// One row per field, holding BOTH the metadata ContentGetSupportedField
// reports and the extractor ContentGetValueW runs, indexed the way TC
// addresses fields. Keeping them together is the point: as two hand-synced
// lists, a field added to one and not the other fell through to
// ft_fieldempty instead of failing the build. Extractors are captureless
// lambdas, so they decay to function pointers with no std::function cost.
//
// The int parameter is TC's UnitIndex (a byte offset), meaningful only to
// the ft_fulltextw rows; every other row ignores it.
using FieldExtractor = int (*)(const AImgInfo&, CacheEntry&, int unitIndex, void* FieldValue, int maxlen);

struct FieldDef {
    const wchar_t* name;
    int type;
    const wchar_t* units;
    FieldExtractor extract;
    // True only for "Has AI Metadata", whose whole purpose is to report
    // has_metadata itself -- including the false case, which the guard every
    // other field passes through would swallow.
    bool bypassesMetadataGuard = false;
};

// Order matters beyond "TC addresses by index": per
// contentgetsupportedfield.htm, ft_fulltext fields "MUST be placed at the
// END of the field list, otherwise you will get errors in Total Commander!"
// Insert every new field BEFORE the last three rows, never after them.
static const FieldDef g_Fields[] = {
    { L"Generator", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.generator, fv, maxlen);
    } }, // 0
    { L"Prompt (Short)", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.prompt, fv, maxlen);
    } }, // 1
    { L"Negative Prompt (Short)", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.negative_prompt, fv, maxlen);
    } }, // 2
    { L"Model", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.model, fv, maxlen);
    } }, // 3
    { L"Model Hash", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.model_hash, fv, maxlen);
    } }, // 4
    { L"Seed", ft_numeric_64, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<__int64>(info.has_seed, info.seed, ft_numeric_64, fv);
    } }, // 5
    { L"CFG Scale", ft_numeric_floating, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<double>(info.has_cfg, info.cfg_scale, ft_numeric_floating, fv);
    } }, // 6
    { L"Steps", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<int>(info.has_steps, info.steps, ft_numeric_32, fv);
    } }, // 7
    { L"Sampler", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.sampler, fv, maxlen);
    } }, // 8
    { L"Scheduler", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.scheduler, fv, maxlen);
    } }, // 9
    { L"Clip Skip", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<int>(info.has_clip_skip, info.clip_skip, ft_numeric_32, fv);
    } }, // 10
    { L"Size", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.size, fv, maxlen);
    } }, // 11
    { L"Aspect Ratio", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(ComputeAspectRatio(info.size), fv, maxlen);
    } }, // 12
    { L"Megapixels", ft_numeric_floating, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        double mp = 0.0;
        if (!ComputeMegapixels(info.size, mp)) return ft_fieldempty;
        *(double*)fv = mp;
        return ft_numeric_floating;
    } }, // 13
    { L"Denoising Strength", ft_numeric_floating, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<double>(info.has_denoising_strength, info.denoising_strength, ft_numeric_floating, fv);
    } }, // 14
    { L"Hires Upscale", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.hires_upscale, fv, maxlen);
    } }, // 15
    { L"Hires Upscaler", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.hires_upscaler, fv, maxlen);
    } }, // 16
    { L"Hires Steps", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        return WriteNumericField<int>(info.has_hires_steps, info.hires_steps, ft_numeric_32, fv);
    } }, // 17
    { L"VAE", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.vae, fv, maxlen);
    } }, // 18
    { L"LoRA", ft_stringw, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int maxlen) {
        return WriteStringField(info.lora, fv, maxlen);
    } }, // 19
    { L"LoRA Count", ft_numeric_32, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        int count = CountLoraEntries(info.lora);
        return WriteNumericField<int>(count != 0, count, ft_numeric_32, fv);
    } }, // 20
    { L"Has AI Metadata", ft_boolean, L"", [](const AImgInfo& info, CacheEntry&, int, void* fv, int) {
        *(int*)fv = info.has_metadata ? 1 : 0;
        return ft_boolean;
    }, /*bypassesMetadataGuard=*/true }, // 21
    { L"Prompt", ft_fulltextw, L"", [](const AImgInfo& info, CacheEntry&, int unitIndex, void* fv, int maxlen) {
        if (info.prompt.empty()) return ft_fieldempty;
        return WriteFulltextChunk(info.prompt, unitIndex, fv, maxlen);
    } }, // 22
    { L"Negative Prompt", ft_fulltextw, L"", [](const AImgInfo& info, CacheEntry&, int unitIndex, void* fv, int maxlen) {
        if (info.negative_prompt.empty()) return ft_fieldempty;
        return WriteFulltextChunk(info.negative_prompt, unitIndex, fv, maxlen);
    } }, // 23
    { L"Full Parameters", ft_fulltextw, L"", [](const AImgInfo& info, CacheEntry& ce, int unitIndex, void* fv, int maxlen) {
        if (info.full_parameters_utf8.empty()) return ft_fieldempty;
        if (!ce.fullParamsWBuilt) {
            ce.fullParamsW = AImgDecoder::Utf8ToWstring(info.full_parameters_utf8);
            ce.fullParamsWBuilt = true;
        }
        return WriteFulltextChunk(ce.fullParamsW, unitIndex, fv, maxlen);
    } }, // 24
};

static const int g_FieldCount = sizeof(g_Fields) / sizeof(g_Fields[0]);

// ANSI-only by the ABI -- there is no ContentGetSupportedFieldW -- so this
// is the sole place field metadata is reported, in every build.
int __stdcall ContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen) {
    // Out of range is the enumeration's termination signal, not an error:
    // ft_nomorefields here, never ContentGetValue's ft_nosuchfield.
    if (FieldIndex < 0 || FieldIndex >= g_FieldCount) return ft_nomorefields;
    if (maxlen <= 0) return ft_nomorefields; // nothing valid to write into a zero/negative-size buffer

    // WideCharToMultiByte writes nothing and returns 0 on a too-small
    // buffer rather than truncating, which would leave TC reading
    // uninitialised stack bytes. Not expected here, but not assumed either.
    if (WideCharToMultiByte(CP_ACP, 0, g_Fields[FieldIndex].name, -1, FieldName, maxlen, NULL, NULL) == 0) {
        FieldName[0] = 0;
    }
    if (WideCharToMultiByte(CP_ACP, 0, g_Fields[FieldIndex].units, -1, Units, maxlen, NULL, NULL) == 0) {
        Units[0] = 0;
    }

    int type = g_Fields[FieldIndex].type;
    // Endorsed by the spec: "You can report ft_fulltext in
    // ContentGetSupportedField, and then either ft_fulltext or ft_fulltextw
    // in ContentGetValue." A BARE type -- flags belong to the separate
    // ContentGetSupportedFieldFlags, never OR'd into this return value.
    if (type == ft_stringw) return ft_string;
    if (type == ft_fulltextw) return ft_fulltext;
    return type;
}

// Shared by the cache-hit and post-parse paths so the has_metadata guard
// (and its one documented bypass) exists in exactly one place.
static int ExtractField(int FieldIndex, CacheEntry& entry, int UnitIndex, void* FieldValue, int maxlen) {
    const AImgInfo& info = entry.info;
    if (g_Fields[FieldIndex].bypassesMetadataGuard) {
        return g_Fields[FieldIndex].extract(info, entry, UnitIndex, FieldValue, maxlen);
    }
    if (!info.has_metadata) return ft_fieldempty;
    return g_Fields[FieldIndex].extract(info, entry, UnitIndex, FieldValue, maxlen);
}

// Split out of ContentGetValueW so the exported entry point can wrap the
// WHOLE call in a try/catch -- including the cache-hit path and the Full
// Parameters extractor's lazy multi-MB conversion, which can throw
// bad_alloc. The inner try further down means something different
// (substitute an empty AImgInfo and cache it, rather than abandon the
// query), so neither subsumes the other.
static int GetValueWImpl(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    if (!FileName || FieldIndex < 0 || FieldIndex >= g_FieldCount) return ft_nosuchfield;
    if (maxlen <= 0) return ft_fieldempty; // guard against a corrupt/negative size before any buffer write

    // UnitIndex == -1 is the spec's "match found, you may discard cached
    // data for this field" signal, answered with ft_fieldempty. The fulltext
    // protocol here is stateless, so there is nothing to discard -- answer
    // before any file I/O. Safe for the other fields too: none declares
    // units, so TC only ever passes 0 for them.
    if (UnitIndex < 0) return ft_fieldempty;

    std::wstring filePath(FileName);

    // A cached file is never delayed, whatever flags TC passes: answering
    // from the cache is not slow by any definition this policy cares about.
    {
        SrwGuard lock(g_CacheMutex);
        if (FindFresh(filePath)) {
            return ExtractField(FieldIndex, g_Cache.front(), UnitIndex, FieldValue, maxlen);
        }
    }

    // Miss, lock released. The stamp is captured BEFORE the parse, not
    // after: a file rewritten while the parse runs must not be recorded as
    // matching the old bytes that were actually read. A failed stat falls
    // through to the parse, which fails its own read and caches an empty
    // result.
    FILETIME writeTime{};
    ULARGE_INTEGER size{};
    bool haveStamp = GetFileStamp(filePath, writeTime, size);
    ULONGLONG statTick = GetTickCount64();

    // Only offered on a genuine miss; a cached file answered above.
    if ((flags & CONTENT_DELAYIFSLOW) && haveStamp && LooksSlow(filePath, size)) {
        return ft_delayed;
    }

    // Give a stop request arriving mid-parse something to set. The slot is
    // thread_local, so this only affects this thread's parse, and RAII below
    // unregisters it so a thrown exception can't leak the entry.
    //
    // Always emplace a NEW entry, never overwrite one for the same key: two
    // threads may legitimately parse the same path, and overwriting A's flag
    // with B's would silently make A unabortable for the rest of its run.
    auto abortFlag = std::make_shared<std::atomic<bool>>(false);
    std::wstring inFlightKey = ToLowerPathKey(filePath);
    {
        SrwGuard lock(g_InFlightMutex);
        g_InFlight.emplace_back(inFlightKey, abortFlag);
    }
    AbortFlagSlot() = abortFlag.get();
    AbortObservedSlot() = false; // reset before this thread's parse: only a poll that happens DURING the parse below may mark the result partial
    // Erases by IDENTITY, not by key: duplicate keys are legal, so erasing
    // "the first matching key" can destroy another thread's still-running
    // registration and leave that parse silently unabortable. Holds its key
    // by value for the same reason -- the local outlives nothing here.
    struct InFlightGuard {
        std::wstring key;
        std::shared_ptr<std::atomic<bool>> flag;
        ~InFlightGuard() {
            SrwGuard lock(g_InFlightMutex);
            for (auto it = g_InFlight.begin(); it != g_InFlight.end(); ++it) {
                if (it->first == key && it->second.get() == flag.get()) { g_InFlight.erase(it); break; }
            }
        }
    } inFlightGuard{inFlightKey, abortFlag};

    AImgInfo info;
    try {
        RawImageMetadata rawMeta;
        if (MetadataParser::ExtractMetadata(filePath, rawMeta)) {
            info = AImgDecoder::Decode(rawMeta);
        }
    } catch (...) {
        // Never let an exception cross the __stdcall boundary into TC's
        // process; substitute an empty result rather than a partial one.
        info = AImgInfo();
    }

    AbortFlagSlot() = nullptr; // done parsing on this thread; clear before the cache/registry housekeeping below

    // A poll inside the parse actually saw the flag, so a coarse loop check
    // bailed out mid-walk and `info` may be partial: report empty and cache
    // nothing. Deliberately NOT `abortFlag->load()` -- that answers "was a
    // stop requested", which is also true when the request lands in the
    // window between a clean finish and the guard erasing the entry. Acting
    // on it there would discard a good result and re-parse the file on every
    // visit forever.
    if (AbortObservedSlot()) {
        return ft_fieldempty;
    }

    CacheEntry entry;
    entry.path = filePath;
    entry.info = std::move(info);
    entry.lastWriteTime = writeTime;
    entry.fileSize = size;
    entry.lastStatTick = statTick;

    SrwGuard lock(g_CacheMutex);
    // Another thread may have inserted this path while this one parsed with
    // no lock held. Either result is fine -- both came from the same bytes.
    // That duplicated parse is the accepted cost of not serializing every
    // miss behind one lock for its full duration.
    if (!FindFresh(filePath)) {
        InsertFresh(std::move(entry));
    }
    return ExtractField(FieldIndex, g_Cache.front(), UnitIndex, FieldValue, maxlen);
}

// Exists solely to guarantee no exception crosses this __stdcall boundary
// into Total Commander's process.
int __stdcall ContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    try {
        return GetValueWImpl(FileName, FieldIndex, UnitIndex, FieldValue, maxlen, flags);
    } catch (...) {
        return ft_fieldempty;
    }
}

int __stdcall ContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    if (!FileName) return ft_nosuchfield;
    int reqLen = MultiByteToWideChar(CP_ACP, 0, FileName, -1, NULL, 0);
    if (reqLen <= 0) return ft_fileerror;

    std::wstring wFileName(reqLen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, FileName, -1, &wFileName[0], reqLen);

    // ft_delayed (0) can come back here on a slow miss and must pass
    // straight through unrecognised, like any other status code -- it is not
    // a success/length value to reinterpret.
    int res = ContentGetValueW(&wFileName[0], FieldIndex, UnitIndex, FieldValue, maxlen, flags);
    if (res == ft_stringw || res == ft_fulltextw) {
        // Inherently lossy legacy path for text outside the ANSI codepage.
        // WC_NO_BEST_FIT_CHARS maps an unrepresentable character to '?'
        // rather than a deceptive "best fit" substitution that silently
        // turns one letter into a different one.
        //
        // Reading back to the first NUL is safe only because every
        // ft_stringw/ft_fulltextw case above NUL-terminates within bounds.
        // A future case filling FieldValue some other way must preserve
        // that, or this reconstruction over-reads.
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
    // strncpy_s with a null or zero-sized destination reaches the CRT
    // invalid-parameter handler inside TC's own process.
    if (!DetectString || maxlen <= 0) return 0;
    const char* str = "EXT=\"PNG\" | EXT=\"JPG\" | EXT=\"JPEG\" | EXT=\"WEBP\" | EXT=\"AVIF\" | EXT=\"TIFF\" | EXT=\"TIF\"";
    strncpy_s(DetectString, maxlen, str, _TRUNCATE);
    return 0; // per the SDK, this function's return value is not a status code
}

void __stdcall ContentSetDefaultParams(ContentDefaultParamStruct* dps) {
    UNREFERENCED_PARAMETER(dps); // no persisted default settings to initialize from
}

void __stdcall ContentSendStateInformation(int state, char* path) {
    UNREFERENCED_PARAMETER(state); // no per-directory cache invalidation needed: every g_Cache entry is staleness-checked by file stamp on each access
    UNREFERENCED_PARAMETER(path);
}

void __stdcall ContentSendStateInformationW(int state, WCHAR* path) {
    UNREFERENCED_PARAMETER(state);
    UNREFERENCED_PARAMETER(path);
}

// Per docs/contentstopgetvalue.htm, called only while a background-thread
// ContentGetValue is active: set a flag, let the lengthy operation poll it
// and return ft_fieldempty. TC calls this speculatively whenever it wants to
// stop waiting, so finding nothing in flight is the normal case.
void __stdcall ContentStopGetValueW(WCHAR* FileName) {
    if (!FileName) return;
    std::wstring key = ToLowerPathKey(FileName);
    SrwGuard lock(g_InFlightMutex);
    // EVERY entry for this path, not just the first: duplicate keys are
    // legal and TC gives no way to say which parse it means.
    for (auto& kv : g_InFlight) {
        if (kv.first == key) kv.second->store(true, std::memory_order_relaxed);
    }
}

void __stdcall ContentStopGetValue(char* FileName) {
    if (!FileName) return;
    int reqLen = MultiByteToWideChar(CP_ACP, 0, FileName, -1, NULL, 0);
    if (reqLen <= 0) return;

    std::wstring wFileName(reqLen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, FileName, -1, &wFileName[0], reqLen);
    ContentStopGetValueW(&wFileName[0]);
}
