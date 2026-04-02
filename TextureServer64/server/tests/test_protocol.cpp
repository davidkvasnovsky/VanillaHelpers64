// test_protocol.cpp - Tests for the shared Protocol.h definitions.
// Compile: cl /EHsc /std:c++17 /I..\..\shared test_protocol.cpp
// or:      g++ -std=c++17 -I../../shared test_protocol.cpp -o test_protocol

#include "Protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// ── test_struct_sizes ───────────────────────────────────────────────────
static void test_struct_sizes() {
    std::printf("test_struct_sizes...\n");

    TEST_ASSERT_EQ(sizeof(TexProto::Request), 8u, "Request size");
    TEST_ASSERT_EQ(sizeof(TexProto::Response), 19u, "Response size");
    TEST_ASSERT(sizeof(TexProto::SlotHeader) <= TexProto::SLOT_HEADER,
                "SlotHeader <= SLOT_HEADER");
    TEST_ASSERT_EQ(sizeof(TexProto::SlotHeader), 60u, "SlotHeader exact size");
    TEST_ASSERT_EQ(sizeof(TexProto::ShmHeader), (size_t)TexProto::SHM_HEADER,
                   "ShmHeader == SHM_HEADER (4096)");

    std::printf("  Request:    %zu bytes\n", sizeof(TexProto::Request));
    std::printf("  Response:   %zu bytes\n", sizeof(TexProto::Response));
    std::printf("  SlotHeader: %zu bytes\n", sizeof(TexProto::SlotHeader));
    std::printf("  ShmHeader:  %zu bytes\n", sizeof(TexProto::ShmHeader));
}

// ── test_hash_normalization ─────────────────────────────────────────────
static void test_hash_normalization() {
    std::printf("test_hash_normalization...\n");

    // Case insensitivity: upper and lower must produce the same hash.
    uint64_t h1 = TexProto::HashPath("Textures\\Armor.blp");
    uint64_t h2 = TexProto::HashPath("textures\\armor.blp");
    TEST_ASSERT_EQ(h1, h2, "Case insensitive: upper vs lower");

    // Slash normalization: forward slashes treated as backslashes.
    uint64_t h3 = TexProto::HashPath("textures/armor.blp");
    TEST_ASSERT_EQ(h1, h3, "Slash normalization: / vs \\");

    // Mixed case + mixed slashes.
    uint64_t h4 = TexProto::HashPath("TEXTURES/ARMOR.BLP");
    TEST_ASSERT_EQ(h1, h4, "Mixed case + slashes");

    // Different paths must produce different hashes.
    uint64_t ha = TexProto::HashPath("textures\\armor.blp");
    uint64_t hb = TexProto::HashPath("textures\\weapon.blp");
    TEST_ASSERT(ha != hb, "Different paths must differ");

    // Empty string should be the FNV offset basis (no iterations).
    uint64_t he = TexProto::HashPath("");
    TEST_ASSERT_EQ(he, 14695981039346656037ULL, "Empty string == FNV offset basis");

    std::printf("  hash(\"textures\\\\armor.blp\") = 0x%016llx\n",
                (unsigned long long)h1);
}

// ── test_shm_total_size ─────────────────────────────────────────────────
static void test_shm_total_size() {
    std::printf("test_shm_total_size...\n");

    // Verify the formula: SHM_HEADER + SLOT_COUNT * SLOT_TOTAL
    uint64_t expected = static_cast<uint64_t>(TexProto::SHM_HEADER)
                      + static_cast<uint64_t>(TexProto::SLOT_COUNT)
                        * TexProto::SLOT_TOTAL;
    TEST_ASSERT_EQ(TexProto::SHM_TOTAL_SIZE, expected,
                   "SHM_TOTAL_SIZE formula");

    // SLOT_TOTAL = SLOT_HEADER + SLOT_DATA_SIZE
    TEST_ASSERT_EQ(TexProto::SLOT_TOTAL,
                   TexProto::SLOT_HEADER + TexProto::SLOT_DATA_SIZE,
                   "SLOT_TOTAL formula");

    // Must be under 300 MB.
    constexpr uint64_t MAX_BYTES = 300ULL * 1024 * 1024;
    TEST_ASSERT(TexProto::SHM_TOTAL_SIZE < MAX_BYTES,
                "SHM_TOTAL_SIZE < 300 MiB");

    std::printf("  SHM_TOTAL_SIZE = %llu bytes (%.1f MiB)\n",
                (unsigned long long)TexProto::SHM_TOTAL_SIZE,
                TexProto::SHM_TOTAL_SIZE / (1024.0 * 1024.0));
}

// ── test_enum_values ────────────────────────────────────────────────────
static void test_enum_values() {
    std::printf("test_enum_values...\n");

    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Cmd::Load),     0x01u, "Cmd::Load");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Cmd::Prefetch), 0x02u, "Cmd::Prefetch");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Cmd::Evict),    0x03u, "Cmd::Evict");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Cmd::Query),    0x04u, "Cmd::Query");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Cmd::Stats),    0x05u, "Cmd::Stats");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Cmd::Shutdown), 0xFFu, "Cmd::Shutdown");

    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::Ok),          0x00u, "Status::Ok");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::NotFound),    0x01u, "Status::NotFound");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::DecodeFail),  0x02u, "Status::DecodeFail");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::NoSlot),      0x03u, "Status::NoSlot");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::Cached),      0x04u, "Status::Cached");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::NotCached),   0x05u, "Status::NotCached");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::Status::ServerError), 0xFFu, "Status::ServerError");

    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::PixelFormat::RGBA8),   0x00u, "PixelFormat::RGBA8");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::PixelFormat::DXT1),    0x01u, "PixelFormat::DXT1");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::PixelFormat::DXT3),    0x02u, "PixelFormat::DXT3");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::PixelFormat::DXT5),    0x03u, "PixelFormat::DXT5");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::PixelFormat::Palette), 0x04u, "PixelFormat::Palette");
    TEST_ASSERT_EQ(static_cast<uint8_t>(TexProto::PixelFormat::BGRA8),   0x05u, "PixelFormat::BGRA8");
}

// ── test_constants ──────────────────────────────────────────────────────
static void test_constants() {
    std::printf("test_constants...\n");

    TEST_ASSERT_EQ(TexProto::SHM_MAGIC,   0x78544856u, "SHM_MAGIC");
    TEST_ASSERT_EQ(TexProto::SHM_VERSION,  1u,          "SHM_VERSION");
    TEST_ASSERT_EQ(TexProto::SLOT_COUNT,    64u,         "SLOT_COUNT");
    TEST_ASSERT_EQ(TexProto::SLOT_DATA_SIZE, 4u * 1024u * 1024u, "SLOT_DATA_SIZE");
}

// ── main ────────────────────────────────────────────────────────────────
int main() {
    std::printf("=== Protocol Tests ===\n");

    test_struct_sizes();
    test_hash_normalization();
    test_shm_total_size();
    test_enum_values();
    test_constants();

    if (g_failures == 0) {
        std::printf("\nAll tests PASSED.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d test(s) FAILED.\n", g_failures);
        return 1;
    }
}
