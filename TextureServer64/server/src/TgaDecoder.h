#pragma once
// TgaDecoder.h - Uncompressed true-color TGA texture decoder for TextureServer64

#include "BlpDecoder.h" // DecodedTexture

#include <cstddef>
#include <cstdint>

namespace TexServer {

class TgaDecoder {
public:
    /// Decode an uncompressed true-color TGA (type 2) into BGRA8 pixels.
    /// Supports 24bpp (BGR) and 32bpp (BGRA).  Returns false on invalid or
    /// unsupported data.
    static auto Decode(const uint8_t* data, size_t size, DecodedTexture& result) -> bool;
};

} // namespace TexServer
