#ifndef METADATA_PARSER_H
#define METADATA_PARSER_H

#include <string>
#include <map>
#include <vector>
#include <cstdint>

class FileReader;

struct RawImageMetadata {
    // Raw text keyed by container-level name: "parameters", "prompt",
    // "workflow", "Comment", "exif:*", "xmp:*".
    //
    // Deliberately std::map: DecodeCore tries candidates in iteration order,
    // and alphabetical order is what puts "prompt" (the authoritative
    // execution graph) ahead of "workflow" (a UI snapshot that can disagree
    // on seed). An unordered_map would make that hash-dependent.
    std::map<std::string, std::string> text_chunks;
};

class MetadataParser {
public:
    static bool ExtractMetadata(const std::wstring& filePath, RawImageMetadata& outMetadata);

private:
    // Streamed, not buffered: IDAT dwarfs everything else in a typical
    // multi-MB render and is skipped by seeking rather than read.
    // `file` must be positioned right after the 8-byte PNG signature.
    static bool ExtractPNG(FileReader& file, uint64_t fileSize, RawImageMetadata& outMetadata);
    // Streamed like ExtractPNG, stopping at SOS/EOI so the entropy-coded
    // scan data is never read. `file` must be positioned after the SOI.
    static bool ExtractJPEG(FileReader& file, uint64_t fileSize, RawImageMetadata& outMetadata);
    static bool ExtractWebP(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata);
    static bool ExtractISOBMFF_AVIF(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata);
    static bool ExtractTIFF(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata);

    static bool ParseEXIF(const uint8_t* data, size_t size, RawImageMetadata& outMetadata);
};

#endif // METADATA_PARSER_H
