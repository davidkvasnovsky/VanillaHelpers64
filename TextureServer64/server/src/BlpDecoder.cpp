// BlpDecoder.cpp - BLP1/BLP2 texture format decoder implementation
//
// BLP1 JPEG format:
//   At offset 156 (after header): uint32_t jpeg_header_size
//   Then jpeg_header_size bytes of shared JPEG tables (SOI, DQT, DHT, SOF)
//   Each mip at mip_offsets[i] contains the JPEG scan data
//   Complete JPEG = jpeg_header + mip_scan_data
//
// BLP2 DXT format:
//   Mip data at mip_offsets[i] contains raw DXT blocks, passed through.

#include "BlpDecoder.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

static constexpr size_t BLP1_HEADER_SIZE    = 156;
static constexpr size_t BLP1_PALETTE_OFFSET = 156;
static constexpr size_t BLP1_PALETTE_SIZE   = 256 * 4;

static constexpr size_t BLP2_HEADER_SIZE    = 148;

static inline uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// ── Header parsing ───────────────────────────────────────────────────

bool BlpDecoder::ParseHeader(const uint8_t* data, size_t size, BlpInfo& info) {
    if (!data || size < 8)
        return false;

    memset(&info, 0, sizeof(info));

    // Check magic
    bool is_blp1 = (data[0] == 'B' && data[1] == 'L' && data[2] == 'P' && data[3] == '1');
    bool is_blp2 = (data[0] == 'B' && data[1] == 'L' && data[2] == 'P' && data[3] == '2');

    if (!is_blp1 && !is_blp2) {
        printf("[BlpDecoder] Bad magic: %02X %02X %02X %02X\n",
               data[0], data[1], data[2], data[3]);
        return false;
    }

    info.is_blp2 = is_blp2;

    if (is_blp1) {
        if (size < BLP1_HEADER_SIZE) return false;

        uint32_t compression = ReadU32(data + 4);
        if (compression > 1) {
            printf("[BlpDecoder] BLP1 bad compression=%u\n", compression);
            return false;
        }
        info.compression = static_cast<BlpCompression>(compression);
        info.alpha_depth = ReadU32(data + 8);
        info.width       = ReadU32(data + 12);
        info.height      = ReadU32(data + 16);
        info.flags       = ReadU32(data + 20);
        info.alpha_type  = 0;

        for (int i = 0; i < 16; ++i) {
            info.mip_offsets[i] = ReadU32(data + 28 + i * 4);
            info.mip_sizes[i]   = ReadU32(data + 92 + i * 4);
        }
    } else {
        // BLP2 header layout:
        //   0:  magic "BLP2"
        //   4:  uint32 type (always 1)
        //   8:  uint8  compression (1=palette, 2=dxt, 3=uncompressed)
        //   9:  uint8  alpha_depth (0,1,4,8)
        //  10:  uint8  alpha_type (0=DXT1, 1=DXT3, 7=DXT5)
        //  11:  uint8  has_mips
        //  12:  uint32 width
        //  16:  uint32 height
        //  20:  uint32 mip_offsets[16]
        //  84:  uint32 mip_sizes[16]
        // 148:  palette (1024 bytes) if compression=1
        if (size < BLP2_HEADER_SIZE) return false;

        uint32_t type = ReadU32(data + 4);
        (void)type; // always 1

        uint8_t comp = data[8];
        if (comp < 1 || comp > 3) {
            printf("[BlpDecoder] BLP2 bad compression=%u\n", comp);
            return false;
        }
        info.compression = static_cast<BlpCompression>(comp);
        info.alpha_depth = data[9];
        info.alpha_type  = data[10];
        info.width       = ReadU32(data + 12);
        info.height      = ReadU32(data + 16);

        for (int i = 0; i < 16; ++i) {
            info.mip_offsets[i] = ReadU32(data + 20 + i * 4);
            info.mip_sizes[i]   = ReadU32(data + 84 + i * 4);
        }
    }

    if (info.width == 0 || info.height == 0) return false;

    // Count valid mipmap levels
    info.mip_count = 0;
    for (int i = 0; i < 16; ++i) {
        if (info.mip_offsets[i] != 0 && info.mip_sizes[i] != 0)
            info.mip_count = static_cast<uint32_t>(i + 1);
        else
            break;
    }

    return true;
}

// ── Main decode dispatch ─────────────────────────────────────────────

bool BlpDecoder::Decode(const uint8_t* data, size_t size, DecodedTexture& result) {
    BlpInfo info{};
    if (!ParseHeader(data, size, info)) {
        printf("[BlpDecoder] ParseHeader failed (size=%zu)\n", size);
        return false;
    }

    printf("[BlpDecoder] %s %ux%u comp=%u alpha_depth=%u alpha_type=%u mips=%u\n",
           info.is_blp2 ? "BLP2" : "BLP1",
           info.width, info.height,
           static_cast<unsigned>(info.compression),
           info.alpha_depth, info.alpha_type, info.mip_count);
    fflush(stdout);

    switch (info.compression) {
        case BlpCompression::Jpeg:
            return DecodeJpeg(data, size, info, result);
        case BlpCompression::Palette:
            return DecodePalette(data, size, info, result);
        case BlpCompression::Dxt:
            return DecodeDxt(data, size, info, result);
        case BlpCompression::Uncompressed:
            // BLP2 uncompressed: raw BGRA at mip offsets
            if (info.mip_count == 0) return false;
            {
                uint32_t mip0_off = info.mip_offsets[0];
                uint32_t mip0_sz  = info.mip_sizes[0];
                if (static_cast<uint64_t>(mip0_off) + mip0_sz > size) return false;
                result.pixels.assign(data + mip0_off, data + mip0_off + mip0_sz);
                result.width  = info.width;
                result.height = info.height;
                result.format = TexProto::PixelFormat::BGRA8;
                result.mip_levels = static_cast<uint8_t>(info.mip_count);
                return true;
            }
        default:
            return false;
    }
}

// ── BLP1 JPEG decode ─────────────────────────────────────────────────
// BLP1 JPEG layout (compression=0):
//   Offset 156: uint32_t jpeg_header_size
//   Offset 160: jpeg_header_size bytes of JPEG header (SOI, DQT, DHT, SOF0)
//   Each mip_offsets[i] → mip_sizes[i] bytes of JPEG scan data (SOS + entropy)
//   Complete JPEG for mip i = jpeg_header + data[mip_offsets[i]..+mip_sizes[i]]

bool BlpDecoder::DecodeJpeg(const uint8_t* data, size_t size,
                            const BlpInfo& info, DecodedTexture& result) {
    if (info.mip_count == 0) return false;

    // Read JPEG header size at offset 156
    if (size < 160) return false;
    uint32_t jpeg_hdr_size = ReadU32(data + 156);
    if (jpeg_hdr_size == 0 || jpeg_hdr_size > 1024) {
        printf("[BlpDecoder] JPEG header size=%u (suspicious)\n", jpeg_hdr_size);
        // Still try if <= 4096
        if (jpeg_hdr_size == 0 || jpeg_hdr_size > 4096) return false;
    }

    if (160 + jpeg_hdr_size > size) return false;

    const uint8_t* jpeg_header = data + 160;

    // Validate mip0
    uint32_t mip0_off = info.mip_offsets[0];
    uint32_t mip0_sz  = info.mip_sizes[0];
    if (mip0_off == 0 || mip0_sz == 0) return false;
    if (static_cast<uint64_t>(mip0_off) + mip0_sz > size) return false;

    // Build complete JPEG: header + mip0 scan data
    std::vector<uint8_t> jpeg_buf;
    jpeg_buf.reserve(jpeg_hdr_size + mip0_sz);
    jpeg_buf.insert(jpeg_buf.end(), jpeg_header, jpeg_header + jpeg_hdr_size);
    jpeg_buf.insert(jpeg_buf.end(), data + mip0_off, data + mip0_off + mip0_sz);

    // Decode via stb_image
    int w = 0, h = 0, channels = 0;
    uint8_t* decoded = stbi_load_from_memory(
        jpeg_buf.data(), static_cast<int>(jpeg_buf.size()),
        &w, &h, &channels, 4); // request RGBA

    if (!decoded) {
        printf("[BlpDecoder] stbi_load_from_memory failed: %s\n",
               stbi_failure_reason());
        fflush(stdout);
        return false;
    }

    // stb_image returns RGBA. Convert to BGRA for D3D9 compatibility.
    uint32_t pixel_count = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
    result.pixels.resize(static_cast<size_t>(pixel_count) * 4);
    for (uint32_t i = 0; i < pixel_count; ++i) {
        result.pixels[i * 4 + 0] = decoded[i * 4 + 2]; // B
        result.pixels[i * 4 + 1] = decoded[i * 4 + 1]; // G
        result.pixels[i * 4 + 2] = decoded[i * 4 + 0]; // R
        result.pixels[i * 4 + 3] = decoded[i * 4 + 3]; // A
    }
    stbi_image_free(decoded);

    result.width      = static_cast<uint32_t>(w);
    result.height     = static_cast<uint32_t>(h);
    result.format     = TexProto::PixelFormat::BGRA8;
    result.mip_levels = static_cast<uint8_t>(info.mip_count);

    printf("[BlpDecoder] JPEG decoded OK: %dx%d, %u pixels\n", w, h, pixel_count);
    fflush(stdout);

    return true;
}

// ── Palette decode (BLP1 and BLP2) ───────────────────────────────────

bool BlpDecoder::DecodePalette(const uint8_t* data, size_t size,
                               const BlpInfo& info, DecodedTexture& result) {
    size_t palette_offset = info.is_blp2 ? BLP2_HEADER_SIZE : BLP1_PALETTE_OFFSET;

    if (size < palette_offset + BLP1_PALETTE_SIZE)
        return false;
    if (info.mip_count == 0)
        return false;

    uint32_t mip0_offset = info.mip_offsets[0];
    uint32_t mip0_size   = info.mip_sizes[0];
    if (mip0_offset == 0 || mip0_size == 0)
        return false;
    if (static_cast<uint64_t>(mip0_offset) + mip0_size > size)
        return false;

    uint64_t pixel_count_64 = static_cast<uint64_t>(info.width) * info.height;
    if (pixel_count_64 > 8192u * 8192u)
        return false;
    uint32_t pixel_count = static_cast<uint32_t>(pixel_count_64);
    if (mip0_size < pixel_count)
        return false;

    const uint8_t* palette = data + palette_offset;
    const uint8_t* indices = data + mip0_offset;

    size_t alpha_data_size = 0;
    if (info.alpha_depth == 8)
        alpha_data_size = pixel_count;
    else if (info.alpha_depth == 4)
        alpha_data_size = (pixel_count + 1) / 2;
    else if (info.alpha_depth == 1)
        alpha_data_size = (pixel_count + 7) / 8;

    if (static_cast<uint64_t>(pixel_count) + alpha_data_size > mip0_size)
        return false;

    const uint8_t* alpha_data = indices + pixel_count;

    result.pixels.resize(static_cast<size_t>(pixel_count) * 4);
    result.width      = info.width;
    result.height     = info.height;
    result.format     = TexProto::PixelFormat::BGRA8;
    result.mip_levels = static_cast<uint8_t>(info.mip_count);

    for (uint32_t i = 0; i < pixel_count; ++i) {
        uint8_t idx = indices[i];
        const uint8_t* pal_entry = palette + static_cast<size_t>(idx) * 4;

        result.pixels[i * 4 + 0] = pal_entry[0]; // B
        result.pixels[i * 4 + 1] = pal_entry[1]; // G
        result.pixels[i * 4 + 2] = pal_entry[2]; // R

        uint8_t alpha = 0xFF;
        if (info.alpha_depth == 0) {
            alpha = pal_entry[3];
        } else if (info.alpha_depth == 8) {
            alpha = alpha_data[i];
        } else if (info.alpha_depth == 4) {
            uint8_t byte = alpha_data[i / 2];
            if (i % 2 == 0)
                alpha = (byte & 0x0F) * 17;
            else
                alpha = ((byte >> 4) & 0x0F) * 17;
        } else if (info.alpha_depth == 1) {
            uint8_t byte = alpha_data[i / 8];
            alpha = (byte >> (i % 8)) & 1 ? 0xFF : 0x00;
        }
        result.pixels[i * 4 + 3] = alpha;
    }

    return true;
}

// ── BLP2 DXT pass-through ────────────────────────────────────────────
// DXT blocks are passed through as-is. The format is determined by
// alpha_type: 0=DXT1, 1=DXT3, 7=DXT5.

bool BlpDecoder::DecodeDxt(const uint8_t* data, size_t size,
                           const BlpInfo& info, DecodedTexture& result) {
    if (info.mip_count == 0) return false;

    size_t total_size = 0;
    for (uint32_t i = 0; i < info.mip_count; ++i) {
        uint32_t mip_off = info.mip_offsets[i];
        uint32_t mip_sz  = info.mip_sizes[i];
        if (mip_off == 0 || mip_sz == 0) return false;
        if (static_cast<uint64_t>(mip_off) + mip_sz > size) return false;
        total_size += mip_sz;
    }

    // Check if mips are stored contiguously — common in WoW BLPs
    bool contiguous = (info.mip_count > 0);
    for (uint32_t i = 1; i < info.mip_count && contiguous; ++i) {
        contiguous = (info.mip_offsets[i] == info.mip_offsets[i - 1] + info.mip_sizes[i - 1]);
    }

    result.pixels.clear();
    if (contiguous && info.mip_count > 0) {
        uint32_t start = info.mip_offsets[0];
        result.pixels.assign(data + start, data + start + total_size);
    } else {
        result.pixels.reserve(total_size);
        for (uint32_t i = 0; i < info.mip_count; ++i) {
            uint32_t mip_off = info.mip_offsets[i];
            uint32_t mip_sz  = info.mip_sizes[i];
            result.pixels.insert(result.pixels.end(), data + mip_off, data + mip_off + mip_sz);
        }
    }
    result.width  = info.width;
    result.height = info.height;
    result.mip_levels = static_cast<uint8_t>(info.mip_count);

    // Map alpha_type to DXT variant.
    switch (info.alpha_type) {
        case 0:  result.format = TexProto::PixelFormat::DXT1; break;
        case 1:  result.format = TexProto::PixelFormat::DXT3; break;
        case 7:  result.format = TexProto::PixelFormat::DXT5; break;
        default: result.format = TexProto::PixelFormat::DXT1; break;
    }

    printf("[BlpDecoder] DXT pass-through: %ux%u, alpha_type=%u, mips=%u, %zu bytes\n",
           info.width, info.height, info.alpha_type, info.mip_count, total_size);
    fflush(stdout);

    return true;
}
