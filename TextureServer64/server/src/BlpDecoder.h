#pragma once
// BlpDecoder.h - BLP1/BLP2 texture format decoder for TextureServer64
//
// Supported formats:
//   BLP1 compression=0 (JPEG)   → decoded via stb_image to BGRA8
//   BLP1 compression=1 (Palette) → decoded to BGRA8
//   BLP2 compression=1 (Palette) → decoded to BGRA8
//   BLP2 compression=2 (DXT)    → passed through as DXT1/DXT3/DXT5
//   BLP2 compression=3 (Uncompressed) → decoded to BGRA8

#include "../../shared/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class BlpCompression : uint32_t {
    Jpeg = 0,         // BLP1 only
    Palette = 1,      // BLP1 and BLP2
    Dxt = 2,          // BLP2 only
    Uncompressed = 3, // BLP2 only
};

struct BlpInfo {
    uint32_t width;
    uint32_t height;
    BlpCompression compression;
    uint32_t alpha_depth;
    uint32_t alpha_type; // BLP2: 0=DXT1, 1=DXT3, 7=DXT5
    uint32_t flags;
    uint32_t mip_offsets[16];
    uint32_t mip_sizes[16];
    uint32_t mip_count;
    bool is_blp2; // true if BLP2, false if BLP1
};

struct DecodedTexture {
    std::vector<uint8_t> pixels;
    uint32_t width{};
    uint32_t height{};
    TexProto::PixelFormat format;
    uint8_t mip_levels{};
};

class BlpDecoder {
public:
    // Parse and validate a BLP1 or BLP2 header. Returns false on invalid data.
    static auto ParseHeader(const uint8_t* data, size_t size, BlpInfo& info) -> bool;

    // Full decode of BLP data into BGRA8 (or DXT pass-through) pixels.
    static auto Decode(const uint8_t* data, size_t size, DecodedTexture& result) -> bool;

private:
    static auto DecodePalette(const uint8_t* data, size_t size, const BlpInfo& info, DecodedTexture& result) -> bool;
    static auto DecodeJpeg(const uint8_t* data, size_t size, const BlpInfo& info, DecodedTexture& result) -> bool;
    static auto DecodeDxt(const uint8_t* data, size_t size, const BlpInfo& info, DecodedTexture& result) -> bool;
};
