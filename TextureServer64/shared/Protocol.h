#pragma once
// Protocol.h - Shared IPC wire format for VanillaHelpers TextureServer64
// Included by both the 64-bit server and the 32-bit client.
// Requirements: standard C++ only, no platform headers.

#include <cstdint>
#include <cstddef>

namespace TexProto {

// ── Named pipe / shared memory names ────────────────────────────────────
constexpr const char* PIPE_NAME = "\\\\.\\pipe\\VH_TextureServer";
constexpr const char* SHM_NAME  = "VH_TexServer_SharedMem";

// ── Slot / shared-memory layout constants ───────────────────────────────
constexpr uint32_t SLOT_COUNT     = 64;
constexpr uint32_t SLOT_DATA_SIZE = 4u * 1024u * 1024u;   // 4 MiB
constexpr uint32_t SLOT_HEADER    = 64;                     // bytes
constexpr uint32_t SLOT_TOTAL     = SLOT_HEADER + SLOT_DATA_SIZE;
constexpr uint32_t SHM_HEADER     = 4096;
constexpr uint64_t SHM_TOTAL_SIZE = static_cast<uint64_t>(SHM_HEADER)
                                  + static_cast<uint64_t>(SLOT_COUNT) * SLOT_TOTAL;

// ── Windowed shared-memory layout ──────────────────────────────────────
// Slot data is split across multiple file mappings to avoid a single huge mapping.
constexpr uint32_t SHM_WINDOW_COUNT     = 4;
constexpr uint32_t SLOTS_PER_WINDOW     = SLOT_COUNT / SHM_WINDOW_COUNT;
constexpr uint64_t SHM_DATA_WINDOW_SIZE =
    static_cast<uint64_t>(SLOTS_PER_WINDOW) * SLOT_TOTAL;
constexpr const char *SHM_DATA_NAME_PREFIX = "VH_TexServer_SharedMem_Data";

// ── Command enum (client -> server) ─────────────────────────────────────
enum class Cmd : uint8_t {
    Load     = 0x01,
    Prefetch = 0x02,
    Evict    = 0x03,
    Query    = 0x04,
    Stats    = 0x05,
    Shutdown = 0xFF,
};

// ── Status enum (server -> client) ──────────────────────────────────────
enum class Status : uint8_t {
    Ok          = 0x00,
    NotFound    = 0x01,
    DecodeFail  = 0x02,
    NoSlot      = 0x03,
    Cached      = 0x04,
    NotCached   = 0x05,
    ServerError = 0xFF,
};

// ── Pixel format enum ───────────────────────────────────────────────────
enum class PixelFormat : uint8_t {
    RGBA8   = 0x00,
    DXT1    = 0x01,
    DXT3    = 0x02,
    DXT5    = 0x03,
    Palette = 0x04,
    BGRA8   = 0x05,
};

// ── Slot state machine ──────────────────────────────────────────────────
// EMPTY(0) → READING(1) → DECODING(2) → READY(3) → UPLOADED(4) → EMPTY(0)
enum class SlotState : uint32_t {
    Empty    = 0,
    Reading  = 1,   // Main thread writing raw bytes
    Decoding = 2,   // Worker thread decoding
    Ready    = 3,   // Decoded pixels available
    Uploaded = 4,   // Client has copied to D3D9, can free
};

// ── Packed wire structs ─────────────────────────────────────────────────
#pragma pack(push, 1)

// Client request header.
// For Load/Prefetch: followed by path_len bytes of path, then data_size bytes of raw file data.
// For Query/Evict: followed by path_len bytes of path only.
// For Stats/Shutdown: no payload.
struct Request {
    Cmd      cmd;        // 1 byte
    uint8_t  priority;   // 1 byte  (0 = highest)
    uint16_t path_len;   // 2 bytes (texture path for cache key)
    uint32_t data_size;  // 4 bytes (raw BLP/TGA bytes; 0 for non-Load commands)
};                        // total: 8 bytes

// Server response header.
struct Response {
    Status      status;      // 1 byte
    uint16_t    slot_id;     // 2 bytes
    uint32_t    width;       // 4 bytes
    uint32_t    height;      // 4 bytes
    uint32_t    data_size;   // 4 bytes
    PixelFormat format;      // 1 byte
    uint8_t     mip_levels;  // 1 byte
    uint8_t     reserved[2]; // 2 bytes
};                            // total: 19 bytes (packed)

// Shared-memory slot header (one per slot, at the start of each SLOT_TOTAL block).
// NOTE: SlotHeader::state and ShmHeader::sequence use plain `volatile` rather
// than std::atomic because they reside in cross-process shared memory (32-bit
// DLL <-> 64-bit server).  std::atomic layout is implementation-defined and
// may differ between compilers/architectures.  Correctness relies on x86 TSO
// guarantees and Interlocked* functions at access sites.
struct SlotHeader {
    volatile uint32_t state;       // 4 bytes — SlotState enum
    uint32_t          width;       // 4 bytes
    uint32_t          height;      // 4 bytes
    uint32_t          data_size;   // 4 bytes — decoded pixel bytes
    PixelFormat       format;      // 1 byte
    uint8_t           mip_levels;  // 1 byte
    uint8_t           reserved[2]; // 2 bytes
    uint64_t          path_hash;   // 8 bytes — FNV-1a for cache key
    uint8_t           padding[32]; // 32 bytes
};                                  // total: 60 bytes

// Global shared-memory header (at offset 0 of the mapped region).
struct ShmHeader {
    uint32_t          magic;              // 4 bytes
    uint32_t          version;            // 4 bytes
    uint32_t          slot_count;         // 4 bytes
    uint32_t          slot_data_size;     // 4 bytes
    uint64_t          server_pid;         // 8 bytes
    volatile uint64_t sequence;           // 8 bytes
    uint8_t           padding[4096 - 32]; // fills to exactly 4096
};                                         // total: 4096 bytes

#pragma pack(pop)

// Static size assertions.
static_assert(sizeof(Request)    == 8,           "Request must be 8 bytes");
static_assert(sizeof(Response)   == 19,          "Response must be 19 bytes (packed)");
static_assert(sizeof(SlotHeader) <= SLOT_HEADER, "SlotHeader must fit in SLOT_HEADER bytes");
static_assert(sizeof(ShmHeader)  == SHM_HEADER,  "ShmHeader must be exactly SHM_HEADER bytes");

// ── Magic / version constants ───────────────────────────────────────────
constexpr uint32_t SHM_MAGIC   = 0x78544856;  // 'VHTx' in little-endian
constexpr uint32_t SHM_VERSION = 1;

// ── Path hashing (FNV-1a with normalization) ────────────────────────────
// Normalizes: forward slashes -> backslash, ASCII uppercase -> lowercase.
inline uint64_t HashPath(const char* path) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

    uint64_t hash = FNV_OFFSET;
    for (const char* p = path; *p != '\0'; ++p) {
        char c = *p;
        if (c == '/')  c = '\\';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        hash *= FNV_PRIME;
    }
    return hash;
}

// ── Back-pressure / tuning constants ────────────────────────────────────
constexpr uint32_t MAX_INFLIGHT_DECODES  = 16;   // max concurrent decode jobs
constexpr uint32_t MAX_DECODE_QUEUE_MB   = 128;  // max raw bytes queued for decode
constexpr uint32_t PREFETCH_LOOKAHEAD    = 8;    // how many textures to prefetch ahead

} // namespace TexProto
