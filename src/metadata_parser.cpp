#include "metadata_parser.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <windows.h>

// ---------------------------------------------------------------------------
// Tiny zlib Inflate implementation (RFC 1950 / RFC 1951) for zero dependencies
// ---------------------------------------------------------------------------

namespace TinyDeflate {

struct BitStream {
    const uint8_t* data;
    size_t size;
    size_t bit_pos;

    BitStream(const uint8_t* d, size_t s) : data(d), size(s), bit_pos(0) {}

    uint32_t read_bits(size_t count) {
        uint32_t val = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t byte_idx = bit_pos / 8;
            size_t bit_idx = bit_pos % 8;
            if (byte_idx >= size) return 0;
            uint32_t bit = (data[byte_idx] >> bit_idx) & 1;
            val |= (bit << i);
            bit_pos++;
        }
        return val;
    }
};

// Canonical Huffman decoder (puff.c-style range decoding): O(code length)
// per symbol instead of a linear scan over every symbol for every bit. The
// canonical code assignment in build() below hands out codes to symbols in
// ascending (length, index) order, which is exactly the order this decode()
// table walk requires.
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
            if (len != 0) {
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

// Cap on decompressed output size to prevent zip-bomb style memory exhaustion
// from a maliciously crafted zTXt/iTXt chunk. Metadata text is never legitimately
// this large.
static const size_t kMaxInflateOutput = 32 * 1024 * 1024;

static bool InflateBlock(BitStream& bs, std::vector<uint8_t>& out) {
    uint32_t final_block = bs.read_bits(1);
    uint32_t block_type = bs.read_bits(2);

    if (block_type == 0) { // Uncompressed
        bs.bit_pos = (bs.bit_pos + 7) & ~7ULL; // align to byte
        uint32_t len = bs.read_bits(16);
        uint32_t nlen = bs.read_bits(16);
        if ((len ^ 0xFFFF) != nlen) return false;
        if (out.size() + len > kMaxInflateOutput) return false;
        for (uint32_t i = 0; i < len; i++) {
            out.push_back((uint8_t)bs.read_bits(8));
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

            static const uint8_t cl_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t code_lens[19] = {0};
            for (uint32_t i = 0; i < hclen; i++) {
                code_lens[cl_order[i]] = (uint8_t)bs.read_bits(3);
            }
            HuffmanDecoder cl_decoder;
            cl_decoder.build(code_lens, 19);

            std::vector<uint8_t> combined_lens(hlit + hdist, 0);
            size_t idx = 0;
            while (idx < hlit + hdist) {
                uint16_t sym = cl_decoder.decode(bs);
                if (sym < 16) {
                    combined_lens[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    uint32_t repeat = bs.read_bits(2) + 3;
                    uint8_t prev = idx > 0 ? combined_lens[idx - 1] : 0;
                    while (repeat-- > 0 && idx < combined_lens.size()) combined_lens[idx++] = prev;
                } else if (sym == 17) {
                    uint32_t repeat = bs.read_bits(3) + 3;
                    while (repeat-- > 0 && idx < combined_lens.size()) combined_lens[idx++] = 0;
                } else if (sym == 18) {
                    uint32_t repeat = bs.read_bits(7) + 11;
                    while (repeat-- > 0 && idx < combined_lens.size()) combined_lens[idx++] = 0;
                } else {
                    // Decode error (0xFFFF) or unexpected symbol: bail out instead
                    // of spinning forever without making progress on `idx`.
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
            if (sym == 256) break; // End of block
            if (sym < 256) {
                out.push_back((uint8_t)sym);
            } else if (sym >= 257 && sym <= 285) {
                uint32_t len_idx = sym - 257;
                uint32_t length = length_base[len_idx] + bs.read_bits(length_extra[len_idx]);
                uint16_t dist_sym = dist_decoder.decode(bs);
                if (dist_sym >= 30) break;
                uint32_t distance = dist_base[dist_sym] + bs.read_bits(dist_extra[dist_sym]);

                if (distance > out.size()) break;
                if (out.size() + length > kMaxInflateOutput) return false;
                size_t start = out.size() - distance;
                for (uint32_t i = 0; i < length; i++) {
                    out.push_back(out[start + i]);
                }
            } else {
                break; // Error
            }
        }
    } else {
        return false;
    }

    return final_block != 0;
}

static bool InflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (size < 2) return false;
    BitStream bs(data + 2, size - 2);
    while (bs.bit_pos / 8 < size - 2 && out.size() < kMaxInflateOutput) {
        if (InflateBlock(bs, out)) break;
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
    int reqSize = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)data, (int)wchar_count, NULL, 0, NULL, NULL);
    if (reqSize <= 0) return "";
    std::string res(reqSize, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)data, (int)wchar_count, &res[0], reqSize, NULL, NULL);
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

// Sniff the actual byte order of a raw UTF-16 buffer holding mostly ASCII/Latin
// generation-parameter text (prompts, "Steps:", "Negative prompt:", etc).
// EXIF's "UNICODE\0" UserComment convention nominally follows the TIFF header's
// byte order, but plenty of real-world writers (CivitAI preview downloaders,
// among others) always emit UTF-16BE regardless of what the TIFF header says.
// Trusting the header there silently swaps every byte pair, turning ASCII text
// into garbage CJK-range codepoints. Since ASCII/Latin-1 code units always have
// a zero high byte, whichever half of each 16-bit unit is mostly zero tells us
// the true byte order; ties/empty input fall back to the caller-supplied guess.
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

    static const std::vector<std::string> tags = {
        "exif:UserComment", "dc:description", "xmp:Description",
        "ComfyUI:prompt", "ComfyUI:workflow", "prompt", "workflow"
    };

    for (const auto& tag : tags) {
        std::string openTag = "<" + tag + ">";
        std::string closeTag = "</" + tag + ">";
        size_t start = xmpStr.find(openTag);
        if (start != std::string::npos) {
            start += openTag.size();
            size_t end = xmpStr.find(closeTag, start);
            if (end != std::string::npos) {
                std::string val = UnescapeXml(xmpStr.substr(start, end - start));
                outMetadata.text_chunks["xmp:" + tag] = val;
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Main Extractor Entry Point
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractMetadata(const std::wstring& filePath, RawImageMetadata& outMetadata) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    std::streamsize fileSize = file.tellg();
    if (fileSize < 12) return false;
    file.seekg(0, std::ios::beg);

    // Sniff the container signature from just the first 16 bytes -- no need
    // to touch the rest of the file yet.
    uint8_t sig[16] = {0};
    size_t sigLen = (size_t)std::min<std::streamsize>(fileSize, sizeof(sig));
    file.read((char*)sig, (std::streamsize)sigLen);
    sigLen = (size_t)file.gcount(); // a short/failed read leaves the rest of sig[] zeroed; shrink sigLen to what actually landed

    if (sigLen >= 8 && sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G') {
        file.seekg(8, std::ios::beg);
        return ExtractPNG(file, (uint64_t)fileSize, outMetadata);
    }

    if (sigLen >= 2 && sig[0] == 0xFF && sig[1] == 0xD8) {
        file.seekg(2, std::ios::beg);
        return ExtractJPEG(file, (uint64_t)fileSize, outMetadata);
    }

    // WebP/AVIF/TIFF parsers work off an in-memory buffer: WebP's spec allows
    // EXIF/XMP chunks to appear after the (large) image-data chunk, and TIFF
    // IFD offsets are absolute file offsets that can point anywhere, so
    // neither can assume metadata sits early like JPEG/PNG do.
    size_t readSize = (size_t)std::min<std::streamsize>(fileSize, 16 * 1024 * 1024);
    std::vector<uint8_t> buffer(readSize);
    file.seekg(0, std::ios::beg);
    file.read((char*)buffer.data(), (std::streamsize)readSize);

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

// Shared by zTXt (always zlib-compressed) and iTXt (optionally compressed):
// either zlib-inflate `data` or copy it as-is into `out`. Returns false if
// `compressed` was requested and inflation failed (nothing to store).
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

// Packs a 4-byte chunk type tag into a single uint32_t so every chunk header
// (including the routinely-skipped ones, e.g. IDAT) can be dispatched with
// one integer comparison instead of building a std::string and running
// several 4-char string compares against it -- this runs on every chunk in
// the file, not just the text-ish ones.
static constexpr uint32_t PackChunkType(char a, char b, char c, char d) {
    return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)c << 8) | (uint32_t)(uint8_t)d;
}

bool MetadataParser::ExtractPNG(std::ifstream& file, uint64_t fileSize, RawImageMetadata& outMetadata) {
    uint64_t pos = 8; // caller has already positioned `file` here, right after the signature

    static const uint32_t kIEND = PackChunkType('I', 'E', 'N', 'D');
    static const uint32_t kTEXt = PackChunkType('t', 'E', 'X', 't');
    static const uint32_t kZTXt = PackChunkType('z', 'T', 'X', 't');
    static const uint32_t kITXt = PackChunkType('i', 'T', 'X', 't');
    static const uint32_t kEXIf = PackChunkType('e', 'X', 'I', 'f');

    // Every chunk except these (above all IDAT, the pixel data -- routinely
    // most of an AI render's file size) is skipped via seekg() unread.
    auto isTextish = [](uint32_t t) {
        return t == kTEXt || t == kZTXt || t == kITXt || t == kEXIf;
    };

    // Guards against a crafted chunk claiming an implausibly large length
    // for a type we'd otherwise buffer -- genuine text/EXIF metadata is
    // always small. Matches the existing zip-bomb-style cap philosophy
    // elsewhere in this file (TinyDeflate::kMaxInflateOutput).
    static const uint64_t kMaxTextChunkPayload = 16 * 1024 * 1024;

    while (pos + 8 <= fileSize) {
        uint8_t header[8];
        file.read((char*)header, 8);
        if ((uint64_t)file.gcount() != 8) break;

        uint32_t length = ReadU32BE(header);
        uint32_t chunkType = ReadU32BE(header + 4);
        pos += 8;

        if (pos + (uint64_t)length + 4 > fileSize) break; // chunk claims more than remains in the file

        if (chunkType == kIEND) break;

        if (isTextish(chunkType) && length <= kMaxTextChunkPayload) {
            std::vector<uint8_t> chunkData(length);
            if (length > 0) {
                file.read((char*)chunkData.data(), (std::streamsize)length);
                if ((uint64_t)file.gcount() != length) break;
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
                        // PNG's conventional home for an embedded XMP packet
                        // (keyword "XML:com.adobe.xmp", per the XMP spec) --
                        // route through ParseXMP the same way JPEG/WebP/AVIF's
                        // XMP segments/chunks/boxes already do, instead of
                        // dumping the whole raw XML/RDF text verbatim under
                        // this literal keyword (which would otherwise make it
                        // a candidate for the generic A1111/Fooocus text
                        // matchers below -- see DecodeCore's "xmp" key skip).
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
            file.seekg(4, std::ios::cur); // skip CRC
            pos += 4;
        } else {
            // Not a chunk type we care about (or a text-ish chunk too large
            // to plausibly be real metadata): skip its payload + CRC without
            // reading it into memory.
            file.seekg((std::streamoff)((uint64_t)length + 4), std::ios::cur);
            pos += (uint64_t)length + 4;
        }

        if (!file.good()) break;
    }

    return !outMetadata.text_chunks.empty();
}

// ---------------------------------------------------------------------------
// JPEG Extractor
// ---------------------------------------------------------------------------

bool MetadataParser::ExtractJPEG(std::ifstream& file, uint64_t fileSize, RawImageMetadata& outMetadata) {
    uint64_t pos = 2; // caller has already positioned `file` here, right after the SOI marker

    while (pos + 4 <= fileSize) {
        file.seekg((std::streamoff)pos, std::ios::beg);
        uint8_t header[4];
        file.read((char*)header, 4);
        if ((uint64_t)file.gcount() != 4) break;

        if (header[0] != 0xFF) break;
        uint8_t marker = header[1];
        if (marker == 0xDA || marker == 0xD9) break; // start-of-scan / end-of-image: entropy-coded pixel data follows, never read it

        uint32_t length = ReadU16BE(header + 2);
        if (length < 2) break;
        if (pos + 2 + length > fileSize) break;
        size_t payloadLen = length - 2;

        if (marker == 0xE1 || marker == 0xFE) {
            std::vector<uint8_t> payload(payloadLen);
            if (payloadLen > 0) {
                file.read((char*)payload.data(), (std::streamsize)payloadLen);
                if ((uint64_t)file.gcount() != payloadLen) break;
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
        } else if (chunkType != "VP8 " && chunkType != "VP8L" && chunkType != "VP8X" && chunkType != "ICCP" && chunkType != "ANIM" && chunkType != "ANMF") {
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
            size_t innerPos = pos + 12;
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

        // Byte length = component count * bytes-per-component of `type`, not
        // count alone -- needed for the inline-vs-offset (<=4 bytes) decision
        // and bounds check. Only matters once a multi-byte-component tag is
        // added; all tags below are 1-byte-per-component (ASCII/BYTE/UNDEFINED).
        static const uint32_t kTiffTypeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        constexpr uint16_t kTiffTypeCount = sizeof(kTiffTypeSize) / sizeof(kTiffTypeSize[0]);

        for (uint16_t i = 0; i < count && (uint64_t)p + 12 <= (uint64_t)tiffSize; i++, p += 12) {
            uint16_t tag = Read16(tiffData + p);
            uint16_t type = Read16(tiffData + p + 2);
            uint32_t cnt = Read32(tiffData + p + 4);
            uint32_t valOrOff = Read32(tiffData + p + 8);

            uint32_t elemSize = (type < kTiffTypeCount) ? kTiffTypeSize[type] : 0;
            if (elemSize == 0) continue; // unknown/unsupported type
            uint64_t byteLen = (uint64_t)cnt * elemSize;

            if (byteLen > 4 && valOrOff + byteLen > (uint64_t)tiffSize) continue;
            const uint8_t* valPtr = (byteLen <= 4) ? (tiffData + p + 8) : (tiffData + valOrOff);

            if (tag == 0x8769) { // ExifIFDPointer
                exifIFDOffset = valOrOff;
            } else if (tag == 0x9286) { // UserComment
                if (cnt >= 8) {
                    if (memcmp(valPtr, "UNICODE\0", 8) == 0) {
                        bool actualIsLE = DetectUtf16IsLE(valPtr + 8, cnt - 8, isLE);
                        outMetadata.text_chunks["exif:UserComment"] = actualIsLE ? Utf16LEToUtf8(valPtr + 8, cnt - 8) : Utf16BEToUtf8(valPtr + 8, cnt - 8);
                    } else {
                        // "ASCII\0\0\0" designator or anything else (undesignated/unrecognized):
                        // treat as raw 8-bit text either way.
                        outMetadata.text_chunks["exif:UserComment"] = std::string((const char*)valPtr + 8, cnt - 8);
                    }
                } else {
                    outMetadata.text_chunks["exif:UserComment"] = std::string((const char*)valPtr, cnt);
                }
            } else {
                // ImageDescription/MakerNote/Artist/Software: plain raw-text tags,
                // all stored the same way -- table lookup instead of one branch each.
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
