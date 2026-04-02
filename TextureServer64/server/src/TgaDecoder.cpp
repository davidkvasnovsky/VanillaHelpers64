// TgaDecoder.cpp - Uncompressed true-color TGA decoder
#include "TgaDecoder.h"
#include <cstring>

namespace TexServer {

// ── TGA header layout (18 bytes) ────────────────────────────────────────
#pragma pack(push, 1)
struct TgaHeader {
    uint8_t  id_length;
    uint8_t  colormap_type;
    uint8_t  image_type;       // 2 = uncompressed true-color
    uint8_t  colormap_spec[5]; // unused for type 2
    uint16_t x_origin;
    uint16_t y_origin;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;              // 24 or 32
    uint8_t  descriptor;       // bit 5 = top-down origin
};
#pragma pack(pop)

static_assert(sizeof(TgaHeader) == 18, "TGA header must be 18 bytes");

bool TgaDecoder::Decode(const uint8_t* data, size_t size,
                        DecodedTexture& result)
{
    // ── Validate minimum size for header ────────────────────────────────
    if (!data || size < sizeof(TgaHeader))
        return false;

    // ── Read header (memcpy to avoid alignment issues) ──────────────────
    TgaHeader hdr;
    std::memcpy(&hdr, data, sizeof(TgaHeader));

    // ── Only uncompressed true-color (type 2) is supported ──────────────
    if (hdr.image_type != 2)
        return false;

    // ── Must have no colour-map ─────────────────────────────────────────
    if (hdr.colormap_type != 0)
        return false;

    // ── Only 24 or 32 bpp ───────────────────────────────────────────────
    if (hdr.bpp != 24 && hdr.bpp != 32)
        return false;

    // ── Reject zero-sized images ────────────────────────────────────────
    if (hdr.width == 0 || hdr.height == 0)
        return false;

    const uint32_t w = hdr.width;
    const uint32_t h = hdr.height;
    const uint32_t src_bytes_per_pixel = hdr.bpp / 8;   // 3 or 4
    const uint64_t pixel_data_size =
        static_cast<uint64_t>(w) * h * src_bytes_per_pixel;
    const size_t pixel_offset = sizeof(TgaHeader) + hdr.id_length;

    // ── Bounds check ────────────────────────────────────────────────────
    if (pixel_offset + pixel_data_size > size)
        return false;

    const uint8_t* src = data + pixel_offset;

    // ── Allocate output (always BGRA8 = 4 bytes/pixel) ──────────────────
    const uint32_t dst_stride = w * 4;
    result.pixels.resize(static_cast<size_t>(w) * h * 4);
    result.width      = w;
    result.height     = h;
    result.format     = TexProto::PixelFormat::BGRA8;
    result.mip_levels = 1;

    // ── Determine vertical orientation ──────────────────────────────────
    // Bit 5 of descriptor: 1 = top-down (no flip needed).
    // Default (0) = bottom-up origin => flip vertically.
    const bool top_down = (hdr.descriptor & 0x20) != 0;

    const uint32_t src_stride = w * src_bytes_per_pixel;

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t src_row = y;
        const uint32_t dst_row = top_down ? y : (h - 1 - y);

        const uint8_t* row_src = src + static_cast<size_t>(src_row) * src_stride;
        uint8_t*       row_dst = result.pixels.data()
                                 + static_cast<size_t>(dst_row) * dst_stride;

        if (hdr.bpp == 32) {
            // BGRA -> BGRA (direct copy)
            std::memcpy(row_dst, row_src, dst_stride);
        } else {
            // BGR -> BGRA (insert alpha = 0xFF)
            for (uint32_t x = 0; x < w; ++x) {
                row_dst[x * 4 + 0] = row_src[x * 3 + 0]; // B
                row_dst[x * 4 + 1] = row_src[x * 3 + 1]; // G
                row_dst[x * 4 + 2] = row_src[x * 3 + 2]; // R
                row_dst[x * 4 + 3] = 0xFF;                // A
            }
        }
    }

    return true;
}

} // namespace TexServer
