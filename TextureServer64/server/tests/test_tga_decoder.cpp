// test_tga_decoder.cpp - Tests for TgaDecoder
// Compile: cl /EHsc /std:c++17 /I..\..\shared /I..\src test_tga_decoder.cpp ..\src\TgaDecoder.cpp
// or:      g++ -std=c++17 -I../../shared -I../src test_tga_decoder.cpp ../src/TgaDecoder.cpp -o test_tga_decoder

#include "TgaDecoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define TEST_ASSERT(cond, msg)                                             \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__,    \
                         __LINE__);                                        \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg)                                          \
    do {                                                                   \
        if ((a) != (b)) {                                                  \
            std::fprintf(stderr, "FAIL: %s  (got %llu, expected %llu)"     \
                         "  (%s:%d)\n", msg,                               \
                         (unsigned long long)(a),                          \
                         (unsigned long long)(b),                          \
                         __FILE__, __LINE__);                              \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Helper: build a minimal TGA header ──────────────────────────────────
static std::vector<uint8_t> make_tga_header(uint16_t w, uint16_t h,
                                            uint8_t bpp,
                                            uint8_t image_type = 2,
                                            uint8_t descriptor = 0)
{
    std::vector<uint8_t> hdr(18, 0);
    hdr[2]  = image_type;
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = bpp;
    hdr[17] = descriptor;
    return hdr;
}

// ── Test 1: uncompressed 32bpp BGRA TGA (bottom-up) ────────────────────
static void test_decode_uncompressed_tga()
{
    std::printf("  test_decode_uncompressed_tga ... ");

    // 2x2 image, 32bpp, bottom-up (default).
    // Row 0 in file = bottom row of image.
    // Pixel layout in file (BGRA order):
    //   file row 0 (bottom):  pixel(0,1) pixel(1,1)
    //   file row 1 (top):     pixel(0,0) pixel(1,0)
    auto buf = make_tga_header(2, 2, 32);

    // Bottom row of image: y=1
    uint8_t px_0_1[] = { 0x10, 0x20, 0x30, 0x40 }; // B G R A
    uint8_t px_1_1[] = { 0x50, 0x60, 0x70, 0x80 };
    // Top row of image: y=0
    uint8_t px_0_0[] = { 0xA0, 0xB0, 0xC0, 0xD0 };
    uint8_t px_1_0[] = { 0xE0, 0xF0, 0x01, 0x02 };

    // file row 0 = bottom of image (y=1)
    buf.insert(buf.end(), px_0_1, px_0_1 + 4);
    buf.insert(buf.end(), px_1_1, px_1_1 + 4);
    // file row 1 = top of image (y=0)
    buf.insert(buf.end(), px_0_0, px_0_0 + 4);
    buf.insert(buf.end(), px_1_0, px_1_0 + 4);

    TexServer::TgaDecoder dec;
    DecodedTexture tex{};
    bool ok = dec.Decode(buf.data(), buf.size(), tex);

    TEST_ASSERT(ok, "Decode should succeed");
    TEST_ASSERT_EQ(tex.width, 2u, "width");
    TEST_ASSERT_EQ(tex.height, 2u, "height");
    TEST_ASSERT_EQ(tex.mip_levels, 1u, "mip_levels");
    TEST_ASSERT(tex.format == TexProto::PixelFormat::BGRA8, "format");
    TEST_ASSERT_EQ(tex.pixels.size(), 16u, "pixel buffer size");

    // After vertical flip, output row 0 should be the top of the image.
    // Output row 0 = file row 1 = {px_0_0, px_1_0}
    // Output row 1 = file row 0 = {px_0_1, px_1_1}
    const uint8_t* p = tex.pixels.data();

    // Row 0, pixel 0 = px_0_0
    TEST_ASSERT(p[0] == 0xA0 && p[1] == 0xB0 && p[2] == 0xC0 && p[3] == 0xD0,
                "row0 pixel0");
    // Row 0, pixel 1 = px_1_0
    TEST_ASSERT(p[4] == 0xE0 && p[5] == 0xF0 && p[6] == 0x01 && p[7] == 0x02,
                "row0 pixel1");
    // Row 1, pixel 0 = px_0_1
    TEST_ASSERT(p[8] == 0x10 && p[9] == 0x20 && p[10] == 0x30 && p[11] == 0x40,
                "row1 pixel0");
    // Row 1, pixel 1 = px_1_1
    TEST_ASSERT(p[12] == 0x50 && p[13] == 0x60 && p[14] == 0x70 && p[15] == 0x80,
                "row1 pixel1");

    std::printf("PASS\n");
}

// ── Test 2: 24bpp BGR → BGRA with alpha=0xFF ───────────────────────────
static void test_decode_24bit_tga()
{
    std::printf("  test_decode_24bit_tga ... ");

    // 2x2 image, 24bpp, top-down (descriptor bit 5 set = 0x20).
    // No flip needed, file order = output order.
    auto buf = make_tga_header(2, 2, 24, /*image_type=*/2, /*descriptor=*/0x20);

    // Row 0: pixel(0,0) pixel(1,0)  — BGR, 3 bytes each
    uint8_t row0[] = { 0x11, 0x22, 0x33,   0x44, 0x55, 0x66 };
    // Row 1: pixel(0,1) pixel(1,1)
    uint8_t row1[] = { 0x77, 0x88, 0x99,   0xAA, 0xBB, 0xCC };

    buf.insert(buf.end(), row0, row0 + 6);
    buf.insert(buf.end(), row1, row1 + 6);

    TexServer::TgaDecoder dec;
    DecodedTexture tex{};
    bool ok = dec.Decode(buf.data(), buf.size(), tex);

    TEST_ASSERT(ok, "Decode should succeed");
    TEST_ASSERT_EQ(tex.width, 2u, "width");
    TEST_ASSERT_EQ(tex.height, 2u, "height");
    TEST_ASSERT_EQ(tex.pixels.size(), 16u, "pixel buffer size");

    const uint8_t* p = tex.pixels.data();

    // pixel(0,0): BGR 0x11,0x22,0x33 -> BGRA 0x11,0x22,0x33,0xFF
    TEST_ASSERT(p[0] == 0x11 && p[1] == 0x22 && p[2] == 0x33 && p[3] == 0xFF,
                "row0 pixel0 alpha");
    // pixel(1,0): BGR 0x44,0x55,0x66 -> BGRA 0x44,0x55,0x66,0xFF
    TEST_ASSERT(p[4] == 0x44 && p[5] == 0x55 && p[6] == 0x66 && p[7] == 0xFF,
                "row0 pixel1 alpha");
    // pixel(0,1): BGR 0x77,0x88,0x99 -> BGRA 0x77,0x88,0x99,0xFF
    TEST_ASSERT(p[8] == 0x77 && p[9] == 0x88 && p[10] == 0x99 && p[11] == 0xFF,
                "row1 pixel0 alpha");
    // pixel(1,1): BGR 0xAA,0xBB,0xCC -> BGRA 0xAA,0xBB,0xCC,0xFF
    TEST_ASSERT(p[12] == 0xAA && p[13] == 0xBB && p[14] == 0xCC && p[15] == 0xFF,
                "row1 pixel1 alpha");

    std::printf("PASS\n");
}

// ── Test 3: reject RLE-compressed TGA (type 10) ────────────────────────
static void test_reject_rle_tga()
{
    std::printf("  test_reject_rle_tga ... ");

    // Build a header with image_type = 10 (RLE true-color).
    auto buf = make_tga_header(2, 2, 32, /*image_type=*/10);
    // Append dummy pixel data (won't be read)
    buf.resize(buf.size() + 16, 0);

    TexServer::TgaDecoder dec;
    DecodedTexture tex{};
    bool ok = dec.Decode(buf.data(), buf.size(), tex);

    TEST_ASSERT(!ok, "RLE TGA should be rejected");

    std::printf("PASS\n");
}

// ── main ────────────────────────────────────────────────────────────────
int main()
{
    std::printf("Running TGA decoder tests...\n");
    test_decode_uncompressed_tga();
    test_decode_24bit_tga();
    test_reject_rle_tga();

    if (g_failures == 0) {
        std::printf("All TGA decoder tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) FAILED.\n", g_failures);
    return 1;
}
