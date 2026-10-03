#include "metadata_parser.h"
#include "aimg_abort.h"
#include <cstring>
#include <algorithm>
#include <windows.h>

// std::ifstream pulled the iostreams/locale machinery into the /MT build;
// these Win32 calls keep the import table KERNEL32-only. FILE_SHARE_DELETE is
// deliberate so Total Commander can rename/delete a file while it is being
// read (MSVC's ifstream opened deny-none, i.e. read|write).
class FileReader {
public:
    FileReader() = default;
    ~FileReader() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool Open(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
    }
    bool Size(uint64_t& out) const {
        LARGE_INTEGER li;
        if (!GetFileSizeEx(h_, &li) || li.QuadPart < 0) return false;
        out = (uint64_t)li.QuadPart;
        return true;
    }
    bool Seek(uint64_t absPos) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)absPos;
        return SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) != 0;
    }
    bool Skip(uint64_t delta) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)delta;
        return SetFilePointerEx(h_, li, nullptr, FILE_CURRENT) != 0;
    }
    // Callers never ask for more than 16 MB, so one DWORD-sized ReadFile is enough.
    size_t Read(void* dst, size_t n) {
        if (n == 0 || n > MAXDWORD) return 0;
        DWORD got = 0;
        if (!ReadFile(h_, dst, (DWORD)n, &got, nullptr)) return 0;
        return (size_t)got;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// ---------------------------------------------------------------------------
// Tiny zlib Inflate implementation (RFC 1950 / RFC 1951) for zero dependencies
// ---------------------------------------------------------------------------

namespace TinyDeflate {

struct BitStream {
    const uint8_t* data;
    size_t size;
    size_t bit_pos;
    // Set once a read runs past the end. read_bits() still returns 0 there,
    // which is indistinguishable from a genuine zero bit -- so every caller
    // that acts on a value it just read (a length, a repeat count, a literal,
    // a symbol) MUST check this, or a truncated stream decodes as fabricated
    // zeros and reports success.
    bool overrun = false;

    BitStream(const uint8_t* d, size_t s) : data(d), size(s), bit_pos(0) {}

    uint32_t read_bits(size_t count) {
        uint32_t val = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t byte_idx = bit_pos / 8;
            size_t bit_idx = bit_pos % 8;
            if (byte_idx >= size) { overrun = true; return 0; }
            uint32_t bit = (data[byte_idx] >> bit_idx) & 1;
            val |= (bit << i);
            bit_pos++;
        }
        return val;
    }
};

// Canonical Huffman decoder, puff.c-style range decoding: O(code length) per
// symbol rather than a scan over every symbol per bit. build() assigns codes
// in ascending (length, index) order, which is the order decode() walks.
struct HuffmanDecoder {
    uint16_t count[16] = {0};   // number of codes of each length (1..15)
    std::vector<uint16_t> symbol; // symbols sorted by (length, original index)

    bool build(const uint8_t* lengths, size_t cnt) {
        for (int i = 0; i < 16; i++) count[i] = 0;
        for (size_t i = 0; i < cnt; ++i) {
            if (lengths[i] < 16) count[lengths[i]]++;
        }
        count[0] = 0;

        uint16_t offs[16] = {0};
        for (int len = 1; len < 16; len++) {
            offs[len] = (len == 1) ? 0 : (uint16_t)(offs[len - 1] + count[len - 1]);
        }

        symbol.assign(cnt, 0);
        for (size_t i = 0; i < cnt; i++) {
            uint8_t len = lengths[i];
            // Mirrors the counting loop's `< 16` guard. Unreachable today
            // only because the code-length alphabet tops out at 15 -- i.e.
            // safe by caller accident. Without it, len >= 16 indexes offs[16]
            // out of bounds and writes through the result.
            if (len != 0 && len < 16) {
                symbol[offs[len]++] = (uint16_t)i;
            }
        }
        return true;
    }

    uint16_t decode(BitStream& bs) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= (int)bs.read_bits(1);
            int cnt = count[len];
            if (code - first < cnt) {
                return symbol[index + (code - first)];
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return 0xFFFF; // Error
    }
};

// Zip-bomb cap: metadata text is never legitimately this large.
static const size_t kMaxInflateOutput = 32 * 1024 * 1024;

// BFINAL travels out through `outFinal` so the return value means exactly
// one thing: false = corrupt, stop and keep what was already appended.
// Conflating the two (returning `final_block != 0`) made "clean but not
// last" and "corrupt" the same answer to the caller.
static bool InflateBlock(BitStream& bs, std::vector<uint8_t>& out, bool& outFinal) {
    uint32_t final_block = bs.read_bits(1);
    outFinal = final_block != 0;
    uint32_t block_type = bs.read_bits(2);

    if (block_type == 0) { // Uncompressed
        bs.bit_pos = (bs.bit_pos + 7) & ~7ULL; // align to byte
        uint32_t len = bs.read_bits(16);
        uint32_t nlen = bs.read_bits(16);
        if (bs.overrun) return false; // truncated before len/nlen were both fully present
        if ((len ^ 0xFFFF) != nlen) return false;
        if (out.size() + len > kMaxInflateOutput) return false;
        for (uint32_t i = 0; i < len; i++) {
            uint32_t b = bs.read_bits(8);
            if (bs.overrun) return false; // do not pad `out` with fabricated zeros
            out.push_back((uint8_t)b);
        }
    } else if (block_type == 1 || block_type == 2) { // Huffman
        HuffmanDecoder lit_decoder, dist_decoder;
        if (block_type == 1) { // Fixed Huffman
            uint8_t lit_lens[288];
            for (int i = 0; i <= 143; i++) lit_lens[i] = 8;
            for (int i = 144; i <= 255; i++) lit_lens[i] = 9;
            for (int i = 256; i <= 279; i++) lit_lens[i] = 7;
            for (int i = 280; i <= 287; i++) lit_lens[i] = 8;
            lit_decoder.build(lit_lens, 288);

            uint8_t dist_lens[32];
            for (int i = 0; i < 32; i++) dist_lens[i] = 5;
            dist_decoder.build(dist_lens, 32);
        } else { // Dynamic Huffman
            uint32_t hlit = bs.read_bits(5) + 257;
            uint32_t hdist = bs.read_bits(5) + 1;
            uint32_t hclen = bs.read_bits(4) + 4;
            if (bs.overrun) return false; // truncated before hlit/hdist/hclen were fully present

            static const uint8_t cl_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t code_lens[19] = {0};
            for (uint32_t i = 0; i < hclen; i++) {
                code_lens[cl_order[i]] = (uint8_t)bs.read_bits(3);
            }
            if (bs.overrun) return false; // truncated mid code-length table
            HuffmanDecoder cl_decoder;
            cl_decoder.build(code_lens, 19);

            std::vector<uint8_t> combined_lens(hlit + hdist, 0);
            size_t idx = 0;
            while (idx < hlit + hdist) {
                uint16_t sym = cl_decoder.decode(bs);
                if (bs.overrun) return false; // truncated mid code-length symbol
                if (sym < 16) {
                    combined_lens[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    uint32_t repeat = bs.read_bits(2) + 3;
                    if (bs.overrun) return false;
                    uint8_t prev = idx > 0 ? combined_lens[idx - 1] : 0;
                    while (repeat-- > 0 && idx < combined_lens.size()) combined_lens[idx++] = prev;
                } else if (sym == 17) {
                    uint32_t repeat = bs.read_bits(3) + 3;
                    if (bs.overrun) return false;
                    while (repeat-- > 0 && idx < combined_lens.size()) combined_lens[idx++] = 0;
                } else if (sym == 18) {
                    uint32_t repeat = bs.read_bits(7) + 11;
                    if (bs.overrun) return false;
                    while (repeat-- > 0 && idx < combined_lens.size()) combined_lens[idx++] = 0;
                } else {
                    // Bail out rather than spin without advancing `idx`.
                    return false;
                }
            }
            lit_decoder.build(combined_lens.data(), hlit);
            dist_decoder.build(combined_lens.data() + hlit, hdist);
        }

        static const uint16_t length_base[29] = {
            3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
        };
        static const uint8_t length_extra[29] = {
            0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
        };
        static const uint16_t dist_base[30] = {
            1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
        };
        static const uint8_t dist_extra[30] = {
            0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
        };

        while (true) {
            if (out.size() >= kMaxInflateOutput) return false;
            uint16_t sym = lit_decoder.decode(bs);
            if (bs.overrun) return false; // never decode a phantom zero-bit code
            if (sym == 256) break; // End of block
            if (sym < 256) {
                out.push_back((uint8_t)sym);
            } else if (sym >= 257 && sym <= 285) {
                uint32_t len_idx = sym - 257;
                uint32_t length = length_base[len_idx] + bs.read_bits(length_extra[len_idx]);
                if (bs.overrun) return false;
                uint16_t dist_sym = dist_decoder.decode(bs);
                if (bs.overrun) return false;
                // Inside this loop a `break` means end-of-block and nothing
                // else: every error exit is a `return false`. A bad distance
                // symbol, an out-of-range back-reference and an unassigned
                // literal/length symbol are corrupt data, not a boundary --
                // breaking here would report a clean non-final block and have
                // InflateZlib re-enter mid-block on garbage.
                if (dist_sym >= 30) return false;
                uint32_t distance = dist_base[dist_sym] + bs.read_bits(dist_extra[dist_sym]);
                if (bs.overrun) return false;

                if (distance > out.size()) return false;
                if (out.size() + length > kMaxInflateOutput) return false;
                size_t start = out.size() - distance;
                for (uint32_t i = 0; i < length; i++) {
                    out.push_back(out[start + i]);
                }
            } else {
                return false; // Error: unassigned literal/length symbol (286/287)
            }
        }
    } else {
        return false;
    }

    return true;
}

static bool InflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (size < 2) return false;
    // CM must be 8: deflate is the only method zlib/PNG define, and decoding
    // from data + 2 assumes that two-byte header, not another framing.
    if ((data[0] & 0x0F) != 8) return false;
    // FDICT means a 4-byte DICTID follows the header, so decoding from
    // data + 2 would read it as bitstream content and yield garbage
    // indistinguishable from real text. FCHECK is deliberately NOT enforced:
    // it shifts nothing structurally, and rejecting on it would throw out a
    // non-conforming writer's otherwise-decodable stream.
    if ((data[1] & 0x20) != 0) return false;
    BitStream bs(data + 2, size - 2);
    while (bs.bit_pos / 8 < size - 2 && out.size() < kMaxInflateOutput) {
        if (AbortRequested()) return !out.empty(); // keep the clean prefix
        bool final_block = false;
        if (!InflateBlock(bs, out, final_block)) break; // corrupt: keep the clean prefix, stop
        if (final_block) break;
    }
    return !out.empty();
}

} // namespace TinyDeflate


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t ReadU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint16_t ReadU16BE(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static uint16_t ReadU16LE(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

static uint32_t ReadU32LE(const uint8_t* p) {
    return uint32_t(p[0]) | (uint16_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static std::string Utf16LEToUtf8(const uint8_t* data, size_t byteLen) {
    size_t wchar_count = byteLen / 2;
    if (wchar_count == 0) return "";
    // `data` is an arbitrary offset into an EXIF tag buffer with no uint16_t
    // alignment guarantee, so casting it to LPCWSTR is formally UB. Copy into
    // an aligned buffer first, as Utf16BEToUtf8 already must for its swap.
    std::vector<uint16_t> aligned(wchar_count);
    memcpy(aligned.data(), data, wchar_count * 2);
    int reqSize = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)aligned.data(), (int)wchar_count, NULL, 0, NULL, NULL);
    if (reqSize <= 0) return "";
    std::string res(reqSize, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)aligned.data(), (int)wchar_count, &res[0], reqSize, NULL, NULL);
    return res;
}

static std::string Utf16BEToUtf8(const uint8_t* data, size_t byteLen) {
    size_t wchar_count = byteLen / 2;
    if (wchar_count == 0) return "";
    std::vector<uint16_t> swapped(wchar_count);
    for (size_t i = 0; i < wchar_count; i++) {
        swapped[i] = ReadU16BE(data + i * 2);
    }
    int reqSize = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)swapped.data(), (int)wchar_count, NULL, 0, NULL, NULL);
    if (reqSize <= 0) return "";
    std::string res(reqSize, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)swapped.data(), (int)wchar_count, &res[0], reqSize, NULL, NULL);
    return res;
}

// EXIF's "UNICODE\0" UserComment nominally follows the TIFF header's byte
// order, but plenty of real writers always emit UTF-16BE regardless, and
// trusting the header there swaps every pair into garbage CJK codepoints.
// ASCII/Latin code units have a zero high byte, so whichever half is mostly
// zero gives the true order; a tie falls back to the caller's guess.
static bool DetectUtf16IsLE(const uint8_t* data, size_t byteLen, bool headerIsLE) {
    size_t wchar_count = byteLen / 2;
    if (wchar_count == 0) return headerIsLE;
    size_t sample = (wchar_count < 512) ? wchar_count : 512;
    size_t zeroLow = 0, zeroHigh = 0;
    for (size_t i = 0; i < sample; i++) {
        if (data[i * 2] == 0) zeroHigh++;      // byte0==0 => BE (0x00XX)
        if (data[i * 2 + 1] == 0) zeroLow++;   // byte1==0 => LE (0xXX00)
    }
    if (zeroLow == zeroHigh) return headerIsLE;
    return zeroLow > zeroHigh;
}

// XMP stores a described value inside <rdf:Alt><rdf:li ...>TEXT</rdf:li>,
// not as the tag's own text content, so reading the tag body verbatim puts
// literal markup into text_chunks. Returns the input UNCHANGED (never a
// truncated substring) when the container is absent or unterminated, so a
// value that genuinely isn't wrapped is never mangled.
static std::string UnwrapRdfAlt(const std::string& body) {
    size_t liStart = body.find("<rdf:li");
    if (liStart == std::string::npos) return body;
    size_t tagEnd = body.find('>', liStart);
    if (tagEnd == std::string::npos) return body;
    size_t contentStart = tagEnd + 1;
    size_t contentEnd = body.find("</rdf:li>", contentStart);
    if (contentEnd == std::string::npos) return body;
    return body.substr(contentStart, contentEnd - contentStart);
}

static std::string UnescapeXml(const std::string& input) {
    std::string res;
    res.reserve(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '&') {
            if (input.compare(i, 6, "&quot;") == 0) { res += '"'; i += 5; }
            else if (input.compare(i, 4, "&lt;") == 0) { res += '<'; i += 3; }
            else if (input.compare(i, 4, "&gt;") == 0) { res += '>'; i += 3; }
            else if (input.compare(i, 5, "&amp;") == 0) { res += '&'; i += 4; }
            else if (input.compare(i, 5, "&#10;") == 0) { res += '\n'; i += 4; }
            else if (input.compare(i, 5, "&#13;") == 0) { res += '\r'; i += 4; }
            else res += input[i];
        } else {
            res += input[i];
        }
    }
    return res;
}

static void ParseXMP(const std::string& xmpStr, RawImageMetadata& outMetadata) {
    outMetadata.text_chunks["xmp"] = xmpStr;

    // Markup spelled out as literals rather than built per tag per call from
    // a vector of bare names: that cost 14 heap allocations on every XMP
    // packet to reproduce compile-time-known text. Repeating the tag name
    // three times per row is the honest price of no runtime string building.
    struct XmpTag {
        const char* name;
        const char* open;
        const char* close;
    };
    static const XmpTag kXmpTags[] = {
        {"exif:UserComment", "<exif:UserComment>", "</exif:UserComment>"},
        {"dc:description",   "<dc:description>",   "</dc:description>"},
        {"xmp:Description",  "<xmp:Description>",  "</xmp:Description>"},
        {"ComfyUI:prompt",   "<ComfyUI:prompt>",   "</ComfyUI:prompt>"},
        {"ComfyUI:workflow", "<ComfyUI:workflow>", "</ComfyUI:workflow>"},
        {"prompt",           "<prompt>",           "</prompt>"},
        {"workflow",         "<workflow>",         "</workflow>"},
    };

    for (const auto& tag : kXmpTags) {
        size_t start = xmpStr.find(tag.open);
        if (start != std::string::npos) {
            start += strlen(tag.open);
            size_t end = xmpStr.find(tag.close, start);
            if (end != std::string::npos) {
                std::string val = UnescapeXml(UnwrapRdfAlt(xmpStr.substr(start, end - start)));
                outMetadata.text_chunks[std::string("xmp:") + tag.name] = val;
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Main Extractor Entry Point
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractMetadata(const std::wstring& filePath, RawImageMetadata& outMetadata) {
    FileReader file;
    if (!file.Open(filePath)) return false;

    uint64_t fileSize = 0;
    if (!file.Size(fileSize)) return false;
    if (fileSize < 12) return false;

    uint8_t sig[16] = {0};
    size_t sigLen = (size_t)std::min<uint64_t>(fileSize, sizeof(sig));
    sigLen = file.Read(sig, sigLen); // a short read leaves sig[] zeroed past this point

    if (sigLen >= 8 && sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G') {
        if (!file.Seek(8)) return false;
        return ExtractPNG(file, fileSize, outMetadata);
    }

    if (sigLen >= 2 && sig[0] == 0xFF && sig[1] == 0xD8) {
        if (!file.Seek(2)) return false;
        return ExtractJPEG(file, fileSize, outMetadata);
    }

    if (AbortRequested()) return false; // no point starting the bounded read below

    // These three need a buffer: WebP allows EXIF/XMP after the image data,
    // and TIFF IFD offsets can point anywhere in the file, so neither can
    // assume metadata sits early the way PNG/JPEG do.
    size_t readSize = (size_t)std::min<uint64_t>(fileSize, 16 * 1024 * 1024);
    std::vector<uint8_t> buffer(readSize);
    if (!file.Seek(0)) return false;
    buffer.resize(file.Read(buffer.data(), readSize)); // a short read must not leave a fabricated tail of zeros to be walked as data
    if (buffer.size() < 12) return false;

    if (sigLen >= 12 && memcmp(sig, "RIFF", 4) == 0 && memcmp(sig + 8, "WEBP", 4) == 0) {
        return ExtractWebP(buffer, outMetadata);
    }
    if (sigLen >= 12 && memcmp(sig + 4, "ftyp", 4) == 0) {
        return ExtractISOBMFF_AVIF(buffer, outMetadata);
    }
    if (sigLen >= 4 && ((sig[0] == 'I' && sig[1] == 'I' && sig[2] == 0x2A) ||
                        (sig[0] == 'M' && sig[1] == 'M' && sig[2] == 0x00))) {
        return ExtractTIFF(buffer, outMetadata);
    }

    return false;
}

// ---------------------------------------------------------------------------
// PNG Extractor
// ---------------------------------------------------------------------------

// Shared by zTXt (always compressed) and iTXt (optionally). False only when
// inflation was asked for and failed.
static bool DecodePngText(const uint8_t* data, size_t len, bool compressed, std::string& out) {
    if (compressed) {
        std::vector<uint8_t> decompressed;
        if (!TinyDeflate::InflateZlib(data, len, decompressed)) return false;
        out.assign((const char*)decompressed.data(), decompressed.size());
    } else {
        out.assign((const char*)data, len);
    }
    return true;
}

static void StorePngText(RawImageMetadata& outMetadata, const std::string& key,
                          const uint8_t* data, size_t len, bool compressed) {
    std::string text;
    if (DecodePngText(data, len, compressed, text)) {
        outMetadata.text_chunks[key] = std::move(text);
    }
}

// Packs a chunk tag into a uint32_t so dispatch is one integer compare
// instead of a std::string build -- this runs on every chunk, IDAT included.
static constexpr uint32_t PackChunkType(char a, char b, char c, char d) {
    return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)c << 8) | (uint32_t)(uint8_t)d;
}

bool MetadataParser::ExtractPNG(FileReader& file, uint64_t fileSize, RawImageMetadata& outMetadata) {
    uint64_t pos = 8; // caller has already positioned `file` here, right after the signature

    static const uint32_t kIEND = PackChunkType('I', 'E', 'N', 'D');
    static const uint32_t kTEXt = PackChunkType('t', 'E', 'X', 't');
    static const uint32_t kZTXt = PackChunkType('z', 'T', 'X', 't');
    static const uint32_t kITXt = PackChunkType('i', 'T', 'X', 't');
    static const uint32_t kEXIf = PackChunkType('e', 'X', 'I', 'f');

    auto isTextish = [](uint32_t t) {
        return t == kTEXt || t == kZTXt || t == kITXt || t == kEXIf;
    };

    // A crafted chunk can claim an implausible length for a type that would
    // otherwise be buffered; genuine metadata is always small.
    static const uint64_t kMaxTextChunkPayload = 16 * 1024 * 1024;

    while (pos + 8 <= fileSize) {
        if (AbortRequested()) break; // stop walking, return what was found

        uint8_t header[8];
        if (file.Read(header, 8) != 8) break;

        uint32_t length = ReadU32BE(header);
        uint32_t chunkType = ReadU32BE(header + 4);
        pos += 8;

        if (pos + (uint64_t)length + 4 > fileSize) break; // chunk claims more than remains in the file

        if (chunkType == kIEND) break;

        if (isTextish(chunkType) && length <= kMaxTextChunkPayload) {
            std::vector<uint8_t> chunkData(length);
            if (length > 0) {
                if (file.Read(chunkData.data(), length) != length) break;
            }
            const uint8_t* data = chunkData.data();

            if (chunkType == kTEXt) {
                const uint8_t* nullPos = (const uint8_t*)memchr(data, 0, length);
                if (nullPos) {
                    std::string key((const char*)data, nullPos - data);
                    std::string text((const char*)nullPos + 1, length - (nullPos - data + 1));
                    outMetadata.text_chunks[key] = text;
                }
            } else if (chunkType == kZTXt) {
                const uint8_t* nullPos = (const uint8_t*)memchr(data, 0, length);
                if (nullPos && (nullPos + 1 < data + length)) {
                    std::string key((const char*)data, nullPos - data);
                    uint8_t compMethod = *(nullPos + 1);
                    const uint8_t* compData = nullPos + 2;
                    size_t compLen = length - (nullPos - data + 2);

                    if (compMethod == 0 && compLen > 0) {
                        StorePngText(outMetadata, key, compData, compLen, /*compressed=*/true);
                    }
                }
            } else if (chunkType == kITXt) {
                const uint8_t* nullPos = (const uint8_t*)memchr(data, 0, length);
                if (nullPos && (nullPos + 2 < data + length)) {
                    std::string key((const char*)data, nullPos - data);
                    uint8_t compFlag = *(nullPos + 1);
                    uint8_t compMethod = *(nullPos + 2);

                    const uint8_t* ptr = nullPos + 3;
                    const uint8_t* end = data + length;

                    while (ptr < end && *ptr != 0) ptr++;
                    if (ptr < end) ptr++;
                    while (ptr < end && *ptr != 0) ptr++;
                    if (ptr < end) ptr++;

                    if (ptr <= end) {
                        size_t textLen = end - ptr;
                        bool compressed = (compFlag == 1 && compMethod == 0);
                        // PNG's standard home for an embedded XMP packet.
                        // Routed through ParseXMP like every other container's
                        // XMP, so its sub-fields land in "xmp:*" keys instead
                        // of raw XML reaching the text matchers.
                        if (key == "XML:com.adobe.xmp") {
                            std::string text;
                            if (DecodePngText(ptr, textLen, compressed, text)) {
                                ParseXMP(text, outMetadata);
                            }
                        } else {
                            StorePngText(outMetadata, key, ptr, textLen, compressed);
                        }
                    }
                }
            } else if (chunkType == kEXIf) {
                ParseEXIF(data, length, outMetadata);
            }

            pos += length;
            if (!file.Skip(4)) break; // skip CRC
            pos += 4;
        } else {
            // Uninteresting, or too large to plausibly be metadata: skip the
            // payload and CRC without reading them.
            if (!file.Skip((uint64_t)length + 4)) break;
            pos += (uint64_t)length + 4;
        }
    }

    return !outMetadata.text_chunks.empty();
}

// ---------------------------------------------------------------------------
// JPEG Extractor
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractJPEG(FileReader& file, uint64_t fileSize, RawImageMetadata& outMetadata) {
    uint64_t pos = 2; // caller has already positioned `file` here, right after the SOI marker

    while (pos + 4 <= fileSize) {
        if (AbortRequested()) break; // stop walking, return what was found

        if (!file.Seek(pos)) break;
        uint8_t header[4];
        if (file.Read(header, 4) != 4) break;

        if (header[0] != 0xFF) break;
        // T.81 B.1.1.3 allows any number of 0xFF fill bytes before a marker.
        // Treating one as the marker reads the length two bytes early and
        // abandons the walk.
        if (header[1] == 0xFF) { pos++; continue; }
        uint8_t marker = header[1];
        if (marker == 0xDA || marker == 0xD9) break; // SOS/EOI: entropy-coded data follows, never read it

        // TEM, RSTn and SOI are standalone per T.81 and carry no length.
        // Reading one anyway yields a nonsense length that fails the bounds
        // check and silently drops every later metadata segment.
        if (marker == 0x01 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) { pos += 2; continue; }

        uint32_t length = ReadU16BE(header + 2);
        if (length < 2) break;
        if (pos + 2 + length > fileSize) break;
        size_t payloadLen = length - 2;

        if (marker == 0xE1 || marker == 0xFE) {
            std::vector<uint8_t> payload(payloadLen);
            if (payloadLen > 0) {
                if (file.Read(payload.data(), payloadLen) != payloadLen) break;
            }
            if (marker == 0xE1) {
                ParseEXIF(payload.data(), payloadLen, outMetadata);
                if (payloadLen >= 29 && memcmp(payload.data(), "http://ns.adobe.com/xap/1.0/\0", 29) == 0) {
                    ParseXMP(std::string((const char*)payload.data() + 29, payloadLen - 29), outMetadata);
                }
            } else {
                outMetadata.text_chunks["Comment"] = std::string((const char*)payload.data(), payloadLen);
            }
        }

        pos += 2 + length;
    }

    return !outMetadata.text_chunks.empty();
}

// ---------------------------------------------------------------------------
// WebP Extractor
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractWebP(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata) {
    size_t pos = 12; // Skip RIFF [4B] WEBP
    size_t total = buffer.size();

    while (pos + 8 <= total) {
        std::string chunkType((const char*)buffer.data() + pos, 4);
        uint32_t chunkSize = ReadU32LE(buffer.data() + pos + 4);
        pos += 8;

        if ((uint64_t)pos + chunkSize > (uint64_t)total) break;

        const uint8_t* chunkData = buffer.data() + pos;

        if (chunkType == "EXIF") {
            ParseEXIF(chunkData, chunkSize, outMetadata);
        } else if (chunkType == "XMP ") {
            ParseXMP(std::string((const char*)chunkData, chunkSize), outMetadata);
        } else if (chunkType != "VP8 " && chunkType != "VP8L" && chunkType != "VP8X" && chunkType != "ICCP" && chunkType != "ANIM" && chunkType != "ANMF" && chunkType != "ALPH") {
            // ALPH is the binary alpha plane, not text: without excluding it
            // the catch-all below stores it as a metadata chunk and reports
            // success on a file carrying no metadata at all.
            if (chunkSize > 4 && chunkSize < 2 * 1024 * 1024) {
                outMetadata.text_chunks[chunkType] = std::string((const char*)chunkData, chunkSize);
            }
        }

        pos += chunkSize + (chunkSize & 1);
    }

    return !outMetadata.text_chunks.empty();
}

// ---------------------------------------------------------------------------
// ISOBMFF / AVIF Extractor
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractISOBMFF_AVIF(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata) {
    size_t pos = 0;
    size_t total = buffer.size();

    while (pos + 8 <= total) {
        uint32_t boxSize32 = ReadU32BE(buffer.data() + pos);
        std::string boxType((const char*)buffer.data() + pos + 4, 4);
        uint64_t boxSize = boxSize32;
        size_t boxHeaderLen = 8;
        if (boxSize32 == 1) {
            if (pos + 16 > total) break;
            uint64_t hi = ReadU32BE(buffer.data() + pos + 8);
            uint64_t lo = ReadU32BE(buffer.data() + pos + 12);
            boxSize = (hi << 32) | lo;
            boxHeaderLen = 16;
        }
        if (boxSize < boxHeaderLen || pos + boxSize > (uint64_t)total) break;

        if (boxType == "meta") {
            // "meta" is a FullBox: its header (8 bytes, or 16 in the 64-bit
            // largesize form) plus 4 bytes of version/flags precede the inner
            // list. A hardcoded +12 misreads every inner box under largesize.
            size_t innerPos = pos + boxHeaderLen + 4;
            size_t innerEnd = pos + (size_t)boxSize;
            while (innerPos + 8 <= innerEnd) {
                uint32_t inSize = ReadU32BE(buffer.data() + innerPos);
                std::string inType((const char*)buffer.data() + innerPos + 4, 4);
                if (inSize < 8 || (uint64_t)innerPos + inSize > (uint64_t)innerEnd) break;

                const uint8_t* inData = buffer.data() + innerPos + 8;
                size_t inLen = inSize - 8;

                if (inType == "Exif") {
                    ParseEXIF(inData, inLen, outMetadata);
                } else if (inType == "xml ") {
                    ParseXMP(std::string((const char*)inData, inLen), outMetadata);
                }

                innerPos += inSize;
            }
        }

        pos += (size_t)boxSize;
    }

    return !outMetadata.text_chunks.empty();
}

// ---------------------------------------------------------------------------
// TIFF Extractor
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractTIFF(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata) {
    return ParseEXIF(buffer.data(), buffer.size(), outMetadata);
}

// ---------------------------------------------------------------------------
// Generic Robust EXIF Parser
// ---------------------------------------------------------------------------

bool MetadataParser::ParseEXIF(const uint8_t* data, size_t size, RawImageMetadata& outMetadata) {
    if (size < 8) return false;

    size_t headerOffset = 0;
    bool foundHeader = false;
    for (size_t i = 0; i + 4 <= size && i < 64; i++) {
        if ((data[i] == 'I' && data[i+1] == 'I' && data[i+2] == 0x2A && data[i+3] == 0x00) ||
            (data[i] == 'M' && data[i+1] == 'M' && data[i+2] == 0x00 && data[i+3] == 0x2A)) {
            headerOffset = i;
            foundHeader = true;
            break;
        }
    }
    if (!foundHeader) return false;

    const uint8_t* tiffData = data + headerOffset;
    size_t tiffSize = size - headerOffset;

    bool isLE = (tiffData[0] == 'I');

    auto Read16 = [isLE](const uint8_t* p) -> uint16_t { return isLE ? ReadU16LE(p) : ReadU16BE(p); };
    auto Read32 = [isLE](const uint8_t* p) -> uint32_t { return isLE ? ReadU32LE(p) : ReadU32BE(p); };

    uint32_t ifdOffset = Read32(tiffData + 4);
    if (ifdOffset >= tiffSize) return false;

    uint32_t exifIFDOffset = 0;

    auto ProcessIFD = [&](uint32_t offset) {
        if ((uint64_t)offset + 2 > (uint64_t)tiffSize) return;
        uint16_t count = Read16(tiffData + offset);
        size_t p = offset + 2;

        // count * bytes-per-component, not count alone: this drives the
        // inline-vs-offset decision and the bounds check.
        static const uint32_t kTiffTypeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        constexpr uint16_t kTiffTypeCount = sizeof(kTiffTypeSize) / sizeof(kTiffTypeSize[0]);

        for (uint16_t i = 0; i < count && (uint64_t)p + 12 <= (uint64_t)tiffSize; i++, p += 12) {
            uint16_t tag = Read16(tiffData + p);
            uint16_t type = Read16(tiffData + p + 2);
            uint32_t cnt = Read32(tiffData + p + 4);
            uint32_t valOrOff = Read32(tiffData + p + 8);

            uint32_t elemSize = (type < kTiffTypeCount) ? kTiffTypeSize[type] : 0;
            if (elemSize == 0) continue; // unknown/unsupported type
            if (cnt == 0) continue; // byteLen==0 would take the inline branch and store an empty entry
            uint64_t byteLen = (uint64_t)cnt * elemSize;

            if (byteLen > 4 && valOrOff + byteLen > (uint64_t)tiffSize) continue;
            const uint8_t* valPtr = (byteLen <= 4) ? (tiffData + p + 8) : (tiffData + valOrOff);

            if (tag == 0x8769) { // ExifIFDPointer
                exifIFDOffset = valOrOff;
            } else if (tag == 0x9286) { // UserComment
                // Strip the 8-byte character-code designator only when it
                // really is one of the four EXIF-defined values: plenty of
                // real writers omit it and start the text at byte 0, and
                // stripping unconditionally ate their first 8 characters.
                static const uint8_t kAsciiDesignator[8] = {'A','S','C','I','I',0,0,0};
                static const uint8_t kJisDesignator[8] = {'J','I','S',0,0,0,0,0};
                static const uint8_t kUndefinedDesignator[8] = {0,0,0,0,0,0,0,0};
                if (cnt >= 8 && memcmp(valPtr, "UNICODE\0", 8) == 0) {
                    bool actualIsLE = DetectUtf16IsLE(valPtr + 8, cnt - 8, isLE);
                    outMetadata.text_chunks["exif:UserComment"] = actualIsLE ? Utf16LEToUtf8(valPtr + 8, cnt - 8) : Utf16BEToUtf8(valPtr + 8, cnt - 8);
                } else if (cnt >= 8 && (memcmp(valPtr, kAsciiDesignator, 8) == 0 ||
                                        memcmp(valPtr, kJisDesignator, 8) == 0 ||
                                        memcmp(valPtr, kUndefinedDesignator, 8) == 0)) {
                    outMetadata.text_chunks["exif:UserComment"] = std::string((const char*)valPtr + 8, cnt - 8);
                } else {
                    // No designator (cnt < 8 included): keep the value whole.
                    outMetadata.text_chunks["exif:UserComment"] = std::string((const char*)valPtr, cnt);
                }
            } else {
                // Plain raw-text tags, all stored identically.
                static const struct { uint16_t tag; const char* key; } kPlainTextTags[] = {
                    {0x010e, "exif:ImageDescription"},
                    {0x927C, "exif:MakerNote"},
                    {0x013B, "exif:Artist"},
                    {0x013C, "exif:Software"},
                };
                for (const auto& e : kPlainTextTags) {
                    if (tag == e.tag) {
                        outMetadata.text_chunks[e.key] = std::string((const char*)valPtr, cnt);
                        break;
                    }
                }
            }
        }
    };

    ProcessIFD(ifdOffset);
    if (exifIFDOffset != 0 && exifIFDOffset < tiffSize) {
        ProcessIFD(exifIFDOffset);
    }

    return !outMetadata.text_chunks.empty();
}
