#ifndef METADATA_PARSER_H
#define METADATA_PARSER_H

#include <string>
#include <map>
#include <vector>
#include <fstream>

struct RawImageMetadata {
    // Map of key -> text content extracted from file
    // e.g. "parameters" -> "masterpiece, 1girl...\nNegative prompt: ...\nSteps: 20..."
    // e.g. "prompt" -> "{...}" (ComfyUI graph)
    // e.g. "workflow" -> "{...}" (ComfyUI workflow)
    // e.g. "exif:UserComment" -> "..."
    // e.g. "exif:ImageDescription" -> "..."
    // e.g. "Comment" -> "..."
    //
    // Deliberately std::map, not unordered_map: AImgDecoder::DecodeCore
    // iterates text_chunks to build its candidate list, and when both
    // "prompt" and "workflow" chunks are present for a ComfyUI file, the
    // FIRST candidate in iteration order is tried first. std::map's
    // alphabetical order happens to put "prompt" (the authoritative
    // API/execution-format graph) before "workflow" (a live UI snapshot that
    // can disagree on seed on control_after_generate seeds -- see the
    // seed-fallback comment in aimg_decoder.cpp's DecodeComfyUI for why). An
    // unordered_map would make that ordering hash-dependent and could
    // silently flip which chunk wins.
    std::map<std::string, std::string> text_chunks;
};

class MetadataParser {
public:
    static bool ExtractMetadata(const std::wstring& filePath, RawImageMetadata& outMetadata);

private:
    // PNG is parsed straight from the file stream rather than a fully
    // buffered copy: non-text chunks (in particular IDAT, which holds the
    // actual pixel data and dwarfs everything else in a typical multi-MB AI
    // render) are skipped with seekg() instead of being read into memory.
    // `file` must be positioned right after the 8-byte PNG signature.
    static bool ExtractPNG(std::ifstream& file, uint64_t fileSize, RawImageMetadata& outMetadata);
    // Streamed like ExtractPNG: JPEG marker segments are walked directly off
    // the file stream and the loop always stops at the SOS/EOI marker, so the
    // (potentially multi-MB) entropy-coded scan data after it is never read.
    // `file` must be positioned right after the 2-byte SOI marker.
    static bool ExtractJPEG(std::ifstream& file, uint64_t fileSize, RawImageMetadata& outMetadata);
    static bool ExtractWebP(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata);
    static bool ExtractISOBMFF_AVIF(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata);
    static bool ExtractTIFF(const std::vector<uint8_t>& buffer, RawImageMetadata& outMetadata);

    static bool ParseEXIF(const uint8_t* data, size_t size, RawImageMetadata& outMetadata);
};

#endif // METADATA_PARSER_H
