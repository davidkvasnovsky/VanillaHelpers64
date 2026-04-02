// test_blp_decoder.cpp - Unit tests for BlpDecoder
// Build: cl /EHsc /std:c++17 /I../src test_blp_decoder.cpp ../src/BlpDecoder.cpp

#include "../src/BlpDecoder.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__);  \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                        \
        if ((a) != (b)) {                                                       \
            std::fprintf(stderr, "  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define RUN_TEST(fn)                                                           \
    do {                                                                        \
        g_tests_run++;                                                          \
        std::fprintf(stdout, "Running %s ... ", #fn);                          \
        if (fn()) {                                                             \
            g_tests_passed++;                                                   \
            std::fprintf(stdout, "PASS\n");                                    \
        } else {                                                                \
            std::fprintf(stdout, "FAIL\n");                                    \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// Helper: write a little-endian uint32 into a byte buffer
// ---------------------------------------------------------------------------
static void WriteU32(uint8_t* dst, uint32_t val) {
    dst[0] = static_cast<uint8_t>(val);
    dst[1] = static_cast<uint8_t>(val >> 8);
    dst[2] = static_cast<uint8_t>(val >> 16);
    dst[3] = static_cast<uint8_t>(val >> 24);
}

// ---------------------------------------------------------------------------
// Helper: build a minimal BLP1 palette-mode buffer
// ---------------------------------------------------------------------------
static std::vector<uint8_t> BuildSyntheticBlp(uint32_t width, uint32_t height,
                                               uint32_t alpha_depth) {
    uint32_t pixel_count = width * height;

    // Calculate alpha data size
    size_t alpha_size = 0;
    if (alpha_depth == 8)      alpha_size = pixel_count;
    else if (alpha_depth == 4) alpha_size = (pixel_count + 1) / 2;
    else if (alpha_depth == 1) alpha_size = (pixel_count + 7) / 8;

    size_t mip0_size = pixel_count + alpha_size;

    // Mip data starts right after header (156) + palette (1024)
    uint32_t mip0_offset = 156 + 256 * 4;
    size_t total_size = mip0_offset + mip0_size;

    std::vector<uint8_t> buf(total_size, 0);

    // Header: magic "BLP1"
    buf[0] = 'B'; buf[1] = 'L'; buf[2] = 'P'; buf[3] = '1';
    WriteU32(&buf[4],  1);            // compression = palette
    WriteU32(&buf[8],  alpha_depth);  // alpha depth
    WriteU32(&buf[12], width);
    WriteU32(&buf[16], height);
    WriteU32(&buf[20], 0);            // flags
    WriteU32(&buf[24], 1);            // has_mipmaps

    // Mipmap offset/size for level 0
    WriteU32(&buf[28], mip0_offset);
    WriteU32(&buf[92], static_cast<uint32_t>(mip0_size));

    // Palette at offset 156: entry 0 = red (BGRA: 0x00, 0x00, 0xFF, 0xFF)
    uint8_t* palette = &buf[156];
    palette[0] = 0x00; // B
    palette[1] = 0x00; // G
    palette[2] = 0xFF; // R
    palette[3] = 0xFF; // A

    // Fill remaining palette entries with black opaque
    for (int i = 1; i < 256; ++i) {
        palette[i * 4 + 0] = 0x00;
        palette[i * 4 + 1] = 0x00;
        palette[i * 4 + 2] = 0x00;
        palette[i * 4 + 3] = 0xFF;
    }

    // Pixel indices: all 0 (= palette entry 0 = red)
    uint8_t* indices = &buf[mip0_offset];
    std::memset(indices, 0, pixel_count);

    // Alpha data: fill with 0xFF for 8-bit, 0xFF for 4-bit, 0xFF for 1-bit
    if (alpha_size > 0) {
        uint8_t* alpha = indices + pixel_count;
        std::memset(alpha, 0xFF, alpha_size);
    }

    return buf;
}

// ---------------------------------------------------------------------------
// Test 1: Parse a valid BLP1 header
// ---------------------------------------------------------------------------
static bool test_parse_blp1_header() {
    auto buf = BuildSyntheticBlp(64, 64, 0);

    BlpDecoder decoder;
    BlpInfo info{};
    ASSERT_TRUE(decoder.ParseHeader(buf.data(), buf.size(), info));
    ASSERT_EQ(info.width, 64u);
    ASSERT_EQ(info.height, 64u);
    ASSERT_EQ(info.compression, BlpCompression::Palette);
    ASSERT_EQ(info.alpha_depth, 0u);
    ASSERT_EQ(info.mip_count, 1u);
    ASSERT_TRUE(info.mip_offsets[0] != 0);
    ASSERT_TRUE(info.mip_sizes[0] != 0);

    return true;
}

// ---------------------------------------------------------------------------
// Test 2: Reject invalid magic ("BLP2")
// ---------------------------------------------------------------------------
static bool test_reject_invalid_magic() {
    auto buf = BuildSyntheticBlp(64, 64, 0);
    // Overwrite magic to "BLP2"
    buf[3] = '2';

    BlpDecoder decoder;
    BlpInfo info{};
    ASSERT_TRUE(!decoder.ParseHeader(buf.data(), buf.size(), info));

    return true;
}

// ---------------------------------------------------------------------------
// Test 3: Decode a 4x4 palette texture (alpha_depth=0)
// ---------------------------------------------------------------------------
static bool test_decode_palette_texture() {
    auto buf = BuildSyntheticBlp(4, 4, 0);

    BlpDecoder decoder;
    DecodedTexture result{};
    ASSERT_TRUE(decoder.Decode(buf.data(), buf.size(), result));
    ASSERT_EQ(result.width, 4u);
    ASSERT_EQ(result.height, 4u);
    ASSERT_EQ(result.format, TexProto::PixelFormat::BGRA8);
    ASSERT_EQ(result.pixels.size(), static_cast<size_t>(4 * 4 * 4));

    // Every pixel should be palette entry 0: B=0x00, G=0x00, R=0xFF, A=0xFF
    for (uint32_t i = 0; i < 16; ++i) {
        ASSERT_EQ(result.pixels[i * 4 + 0], 0x00u); // B
        ASSERT_EQ(result.pixels[i * 4 + 1], 0x00u); // G
        ASSERT_EQ(result.pixels[i * 4 + 2], 0xFFu); // R
        ASSERT_EQ(result.pixels[i * 4 + 3], 0xFFu); // A (from palette)
    }

    return true;
}

// ---------------------------------------------------------------------------
// Test 4: Decode palette texture with alpha_depth=8
// ---------------------------------------------------------------------------
static bool test_decode_palette_alpha8() {
    auto buf = BuildSyntheticBlp(4, 4, 8);

    BlpDecoder decoder;
    DecodedTexture result{};
    ASSERT_TRUE(decoder.Decode(buf.data(), buf.size(), result));

    // Alpha data was filled with 0xFF, so alpha should be 0xFF
    for (uint32_t i = 0; i < 16; ++i) {
        ASSERT_EQ(result.pixels[i * 4 + 3], 0xFFu);
    }

    // Now modify alpha data to 0x80 for first pixel
    uint32_t mip0_offset = 156 + 256 * 4;
    buf[mip0_offset + 16] = 0x80; // alpha byte for pixel 0 (after 16 index bytes)

    DecodedTexture result2{};
    ASSERT_TRUE(decoder.Decode(buf.data(), buf.size(), result2));
    ASSERT_EQ(result2.pixels[0 * 4 + 3], 0x80u); // pixel 0 alpha

    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Decode palette texture with alpha_depth=1
// ---------------------------------------------------------------------------
static bool test_decode_palette_alpha1() {
    auto buf = BuildSyntheticBlp(4, 4, 1);

    // Alpha data is at mip0_offset + pixel_count
    uint32_t mip0_offset = 156 + 256 * 4;
    uint32_t pixel_count = 4 * 4;
    // Alpha is bit-packed: 2 bytes for 16 pixels
    // Set first byte to 0xAA = 10101010 => pixels 0,2,4,6 = 0, pixels 1,3,5,7 = 1
    buf[mip0_offset + pixel_count + 0] = 0xAA;
    buf[mip0_offset + pixel_count + 1] = 0xFF;

    BlpDecoder decoder;
    DecodedTexture result{};
    ASSERT_TRUE(decoder.Decode(buf.data(), buf.size(), result));

    // 0xAA = bit0=0, bit1=1, bit2=0, bit3=1, bit4=0, bit5=1, bit6=0, bit7=1
    ASSERT_EQ(result.pixels[0 * 4 + 3], 0x00u);  // bit 0 = 0
    ASSERT_EQ(result.pixels[1 * 4 + 3], 0xFFu);  // bit 1 = 1
    ASSERT_EQ(result.pixels[2 * 4 + 3], 0x00u);  // bit 2 = 0
    ASSERT_EQ(result.pixels[3 * 4 + 3], 0xFFu);  // bit 3 = 1

    // Second byte 0xFF => all 1s => pixels 8..15 all alpha 0xFF
    for (uint32_t i = 8; i < 16; ++i) {
        ASSERT_EQ(result.pixels[i * 4 + 3], 0xFFu);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Test 6: Reject truncated data
// ---------------------------------------------------------------------------
static bool test_reject_truncated_data() {
    BlpDecoder decoder;
    BlpInfo info{};

    // Too short for header
    uint8_t tiny[10] = {};
    ASSERT_TRUE(!decoder.ParseHeader(tiny, sizeof(tiny), info));

    // Valid header but truncated palette
    auto buf = BuildSyntheticBlp(4, 4, 0);
    DecodedTexture result{};
    ASSERT_TRUE(!decoder.Decode(buf.data(), 200, result));  // just past header, no full palette

    return true;
}

// ---------------------------------------------------------------------------
// Test 7: JPEG compression returns false (stub)
// ---------------------------------------------------------------------------
static bool test_jpeg_stub_returns_false() {
    auto buf = BuildSyntheticBlp(4, 4, 0);
    // Change compression to JPEG
    WriteU32(&buf[4], 0);

    BlpDecoder decoder;
    DecodedTexture result{};
    ASSERT_TRUE(!decoder.Decode(buf.data(), buf.size(), result));

    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::fprintf(stdout, "=== BlpDecoder Tests ===\n");

    RUN_TEST(test_parse_blp1_header);
    RUN_TEST(test_reject_invalid_magic);
    RUN_TEST(test_decode_palette_texture);
    RUN_TEST(test_decode_palette_alpha8);
    RUN_TEST(test_decode_palette_alpha1);
    RUN_TEST(test_reject_truncated_data);
    RUN_TEST(test_jpeg_stub_returns_false);

    std::fprintf(stdout, "\n%d/%d tests passed.\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
