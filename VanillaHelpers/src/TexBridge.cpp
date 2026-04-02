// This file is part of VanillaHelpers.
//
// TexBridge: Hooks WoW's SFile_Open to intercept .blp/.tga texture loading,
// reads raw file bytes via a second MPQ file handle, queues async decode on
// the 64-bit TextureServer64 companion process, and serves decoded pixels
// from shared memory.

#include "TexBridge.h"
#include "Common.h"
#include "Game.h"
#include "MinHook.h"
#include "Offsets.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <list>
#include <unordered_map>
#include <unordered_set>

// ── Protocol constants (mirror of Protocol.h to avoid cross-project include) ──
namespace TBProto {
    static constexpr const char *PIPE_NAME = "\\\\.\\pipe\\VH_TextureServer";
    static constexpr const char *SHM_NAME = "VH_TexServer_SharedMem";
    static constexpr uint32_t SLOT_COUNT = 64;
    static constexpr uint32_t SLOT_DATA_SIZE = 4u * 1024u * 1024u;
    static constexpr uint32_t SLOT_HEADER_SIZE = 64;
    static constexpr uint32_t SLOT_TOTAL = SLOT_HEADER_SIZE + SLOT_DATA_SIZE;
    static constexpr uint32_t SHM_HEADER_SIZE = 4096;
    static constexpr uint32_t SHM_MAGIC = 0x78544856;
    static constexpr uint32_t SHM_VERSION = 1;

    static constexpr uint32_t STATE_EMPTY = 0;
    static constexpr uint32_t STATE_READY = 3;

    static constexpr uint32_t MAX_INFLIGHT = 16;
    static constexpr uint32_t MAX_QUEUE_MB = 128;

    static uint64_t HashPath(const char *path) {
        constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
        uint64_t hash = FNV_OFFSET;
        for (const char *p = path; *p; ++p) {
            char c = *p;
            if (c == '/')  c = '\\';
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
            hash *= FNV_PRIME;
        }
        return hash;
    }

#pragma pack(push, 1)
    struct Request {
        uint8_t  cmd;
        uint8_t  priority;
        uint16_t path_len;
        uint32_t data_size;
    };
    struct Response {
        uint8_t  status;
        uint16_t slot_id;
        uint32_t width;
        uint32_t height;
        uint32_t data_size;
        uint8_t  format;
        uint8_t  mip_levels;
        uint8_t  reserved[2];
    };
    struct SlotHeader {
        volatile uint32_t state;
        uint32_t width;
        uint32_t height;
        uint32_t data_size;
        uint8_t  format;
        uint8_t  mip_levels;
        uint8_t  reserved[2];
        uint64_t path_hash;
        uint8_t  padding[32];
    };
    struct ShmHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t slot_count;
        uint32_t slot_data_size;
        uint64_t server_pid;
        volatile uint64_t sequence;
        uint8_t  padding[4096 - 32];
    };
#pragma pack(pop)

    static constexpr uint8_t CMD_LOAD = 0x01;
    static constexpr uint8_t CMD_PREFETCH = 0x02;
    static constexpr uint8_t CMD_SHUTDOWN = 0xFF;
    static constexpr uint8_t STATUS_OK = 0x00;
    static constexpr uint8_t STATUS_NOT_FOUND = 0x01;
    static constexpr uint8_t STATUS_CACHED = 0x04;
} // namespace TBProto

namespace TexBridge {

// ══════════════════════════════════════════════════════════════════════
//  File Logger
// ══════════════════════════════════════════════════════════════════════
// Writes to TexBridge.log next to the DLL. Flushed after every write
// for crash-safe diagnostics.

namespace {

struct IDirect3DDevice9;
struct IDirect3DTexture9;

struct D3DLOCKED_RECT {
    int Pitch;
    void *pBits;
};

using HRESULT = long;

static constexpr HRESULT D3D_OK = 0;
static constexpr uint32_t D3DPOOL_DEFAULT_RT = 0;
static constexpr uint32_t D3DPOOL_SYSTEMMEM_RT = 2;
static constexpr uint32_t D3DUSAGE_DYNAMIC = 0x00000200;
static constexpr uint32_t D3DFMT_A8R8G8B8 = 21;
static constexpr uint32_t D3DFMT_DXT1 = 827611204;
static constexpr uint32_t D3DFMT_DXT3 = 861165636;
static constexpr uint32_t D3DFMT_DXT5 = 894720068;

typedef HRESULT(__stdcall *D3DDeviceCreateTexture_fn)(
    void *pThis, uint32_t Width, uint32_t Height, uint32_t Levels,
    uint32_t Usage, uint32_t Format, uint32_t Pool, void **ppTexture,
    HANDLE *pSharedHandle);
typedef HRESULT(__stdcall *D3DTextureLockRect_fn)(
    void *pThis, uint32_t Level, D3DLOCKED_RECT *pLockedRect,
    const RECT *pRect, uint32_t Flags);
typedef HRESULT(__stdcall *D3DTextureUnlockRect_fn)(void *pThis, uint32_t Level);
typedef HRESULT(__stdcall *D3DTextureRelease_fn)(void *pThis);
typedef HRESULT(__stdcall *D3DTextureGetDevice_fn)(void *pThis, void **ppDevice);
typedef HRESULT(__stdcall *D3DDeviceUpdateTexture_fn)(void *pThis, void *srcTexture, void *dstTexture);

static FILE *s_logFile = nullptr;
static CRITICAL_SECTION s_logCS;
static bool s_logCSInit = false;

static void LogInit(const char *dllDir) {
    if (!s_logCSInit) {
        InitializeCriticalSection(&s_logCS);
        s_logCSInit = true;
    }
    if (s_logFile) return;

    std::string path = std::string(dllDir) + "TexBridge.log";
    fopen_s(&s_logFile, path.c_str(), "w");
    if (s_logFile) {
        fprintf(s_logFile, "[TexBridge] Log started\n");
        fflush(s_logFile);
    }
}

static void LogWrite(const char *fmt, ...) {
    if (!s_logFile) return;
    EnterCriticalSection(&s_logCS);
    va_list args;
    va_start(args, fmt);
    fprintf(s_logFile, "[TexBridge] ");
    vfprintf(s_logFile, fmt, args);
    fprintf(s_logFile, "\n");
    fflush(s_logFile);
    va_end(args);
    LeaveCriticalSection(&s_logCS);
}

static void LogClose() {
    if (s_logFile) {
        fprintf(s_logFile, "[TexBridge] Log closed\n");
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = nullptr;
    }
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════════
//  Fixed Buffer Pool
// ══════════════════════════════════════════════════════════════════════

namespace {

static constexpr int    POOL_COUNT = 8;
static constexpr uint32_t POOL_BUF_SIZE = 2u * 1024u * 1024u;

struct BufferPool {
    uint8_t *buffers[POOL_COUNT] = {};
    volatile LONG locks[POOL_COUNT] = {};

    bool Init() {
        for (int i = 0; i < POOL_COUNT; ++i) {
            buffers[i] = static_cast<uint8_t *>(
                VirtualAlloc(nullptr, POOL_BUF_SIZE,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!buffers[i]) {
                Destroy();
                return false;
            }
        }
        return true;
    }

    void Destroy() {
        for (int i = 0; i < POOL_COUNT; ++i) {
            if (buffers[i]) {
                VirtualFree(buffers[i], 0, MEM_RELEASE);
                buffers[i] = nullptr;
            }
            locks[i] = 0;
        }
    }

    int Acquire() {
        for (int i = 0; i < POOL_COUNT; ++i) {
            if (InterlockedCompareExchange(&locks[i], 1, 0) == 0)
                return i;
        }
        return -1;
    }

    void Release(int idx) {
        if (idx >= 0 && idx < POOL_COUNT)
            InterlockedExchange(&locks[idx], 0);
    }

    uint8_t *Get(int idx) {
        if (idx >= 0 && idx < POOL_COUNT) return buffers[idx];
        return nullptr;
    }
};

// CacheEntry records that a texture was successfully decoded on the server.
// No slot is held — the server's LRU cache retains the decoded pixels.
// When GetDecodedTexture is called, a fresh slot is requested (server cache hit).
struct CacheEntry {
    uint32_t width;
    uint32_t height;
    uint32_t data_size;
    uint8_t  format;
    uint8_t  mip_levels;
    bool     valid;
};

struct DecodeRequest {
    char     path[260];
    int      buf_idx;
    uint32_t raw_size;
    uint8_t  priority;
    uint64_t path_hash;
};

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════════
//  Module State
// ══════════════════════════════════════════════════════════════════════

static bool    s_initialized = false;
static bool    s_server_available = false;
static HMODULE s_hModule = nullptr;
static char    s_dllDir[MAX_PATH] = {};

// Shared memory
static HANDLE   s_shmMapping = nullptr;
static uint8_t *s_shmBase = nullptr;

// Buffer pool
static BufferPool s_pool;

// Decode cache
static SRWLOCK s_cacheLock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, CacheEntry> s_cache;

// Async decode queue
static SRWLOCK s_queueLock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_queueCV = CONDITION_VARIABLE_INIT;
static std::deque<DecodeRequest> s_queue;
static HANDLE  s_workerThread = nullptr;
static volatile bool s_workerRunning = false;

// Back-pressure
static volatile LONG s_inflight = 0;
static volatile LONG s_queuedBytes = 0;

// Stats
static volatile LONG s_stat_intercepted = 0;
static volatile LONG s_stat_queued = 0;
static volatile LONG s_stat_done = 0;
static volatile LONG s_stat_cache_hits = 0;
static volatile LONG s_stat_sync_fallback = 0;
static volatile LONG s_stat_bp_rejects = 0;
static volatile LONG s_stat_pool_misses = 0;
static volatile LONG s_stat_default_swaps = 0;
static volatile LONG s_stat_default_evictions = 0;

// Prefetch tracking
static constexpr int PREFETCH_HISTORY = 32;
static uint64_t s_recentDirs[PREFETCH_HISTORY] = {};
static int      s_recentDirIdx = 0;

// Original function pointer for hooked TextureCreate
static Game::TextureCreate_t TextureCreate_o = nullptr;

struct DefaultPoolEntry {
    uintptr_t texture_key;
    uint64_t  path_hash;
    void     *default_tex;
    uint32_t  size_bytes;
};

static SRWLOCK s_defaultPoolLock = SRWLOCK_INIT;
static std::list<DefaultPoolEntry> s_defaultPoolLru;
static std::unordered_map<uintptr_t, std::list<DefaultPoolEntry>::iterator> s_defaultPoolMap;
static size_t s_defaultPoolBytes = 0;
static constexpr size_t DEFAULT_POOL_BUDGET_BYTES = 256u * 1024u * 1024u;

// ══════════════════════════════════════════════════════════════════════
//  Phase 2: Texture Memory Tracking & Struct Discovery
// ══════════════════════════════════════════════════════════════════════
//
// Goal: discover the HTEXTURE__ struct layout empirically, then free
// CPU-side decoded pixel buffers after D3D upload completes, saving
// hundreds of MB of 32-bit virtual address space.
//
// TextureCreate_h records HTEXTURE* → pathHash.
// TextureGetGxTex_h probes the struct to find width/height/pixelbuf offsets.
// Once discovered, pixel buffers are freed after upload; the server's
// LRU cache can re-populate them on demand (device reset, etc.).

// Map HTEXTURE pointers to path hashes for texture lifecycle tracking.
static SRWLOCK s_texMapLock = SRWLOCK_INIT;
static std::unordered_map<uintptr_t, uint64_t> s_texMap;
static std::unordered_map<uintptr_t, std::string> s_texPathMap;

// Set of HTEXTURE pointers already probed (avoid redundant scans).
static SRWLOCK s_probedSetLock = SRWLOCK_INIT;
static std::unordered_set<uintptr_t> s_probedSet;

// Probing limits
static volatile LONG s_probedCount = 0;
static constexpr LONG MAX_PROBES = 50;          // hex-dump first N
static constexpr LONG MAX_STRUCT_SCAN = 100;     // scan first N with pattern match

// Discovered struct offsets (-1 = not yet discovered).
// These are byte offsets from the start of HTEXTURE__.
static volatile LONG s_off_width     = -1;  // uint32 width
static volatile LONG s_off_height    = -1;  // uint32 height (expected at width+4)
static volatile LONG s_off_pixelbuf  = -1;  // pointer to decoded pixel buffer
static volatile LONG s_off_datasize  = -1;  // uint32 pixel data size

// Candidate offset voting: each probe votes for a (width,height) pair offset.
// The offset with the most votes is accepted.
static constexpr int VOTE_SLOTS = 128;       // scan up to 512 bytes (128 dwords)
static volatile LONG s_whVotes[VOTE_SLOTS] = {};  // votes per dword offset

// Phase 2 stats
static volatile LONG s_stat_gxtex_calls   = 0;
static volatile LONG s_stat_probed        = 0;
static volatile LONG s_stat_freed_texbufs = 0;
static volatile LONG64 s_stat_freed_bytes = 0;

// Original TextureGetGxTex function pointer
static Game::TextureGetGxTex_t TextureGetGxTex_o = nullptr;

// ══════════════════════════════════════════════════════════════════════
//  Helpers
// ══════════════════════════════════════════════════════════════════════

static bool IsTextureFile(const char *path) {
    if (!path) return false;
    size_t len = strlen(path);
    if (len < 5) return false;
    const char *ext = path + len - 4;
    return (_stricmp(ext, ".blp") == 0 || _stricmp(ext, ".tga") == 0);
}

static bool ContainsI(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    const size_t needleLen = strlen(needle);
    if (needleLen == 0) return true;
    for (const char *p = haystack; *p; ++p) {
        if (_strnicmp(p, needle, needleLen) == 0)
            return true;
    }
    return false;
}

static bool IsTargetTexturePath(const char *path) {
    if (!path) return false;
    return ContainsI(path, "WORLD\\") ||
           ContainsI(path, "CREATURE\\") ||
           ContainsI(path, "CHARACTER\\") ||
           ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\") ||
           ContainsI(path, "XTEXTURES\\") ||
           ContainsI(path, "INTERFACE\\GLUES\\MODELS\\");
}

static bool IsDxtFormat(uint8_t format) {
    return format == 0x01 || format == 0x02 || format == 0x03;
}

static bool IsBgraFormat(uint8_t format) {
    return format == 0x05 || format == 0x00;
}

static bool IsTargetUncompressedPath(const char *path) {
    if (!path) return false;
    return ContainsI(path, "CHARACTER\\") ||
           ContainsI(path, "CREATURE\\") ||
           ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\CAPE\\") ||
           ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\SHOULDER\\") ||
           ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\WEAPON\\");
}

static bool ShouldSwapFormatForPath(const char *path, uint8_t format,
                                    uint32_t width, uint32_t height) {
    if (IsDxtFormat(format))
        return IsTargetTexturePath(path);
    if (IsBgraFormat(format))
        return IsTargetUncompressedPath(path) && (width >= 512 || height >= 512);
    return false;
}

static uint32_t D3DFormatFromProto(uint8_t format) {
    switch (format) {
    case 0x00:
    case 0x05: return D3DFMT_A8R8G8B8;
    case 0x01: return D3DFMT_DXT1;
    case 0x02: return D3DFMT_DXT3;
    case 0x03: return D3DFMT_DXT5;
    default:   return 0;
    }
}

static uint32_t ComputeTextureBytes(uint32_t width, uint32_t height,
                                    uint8_t format, uint8_t mipLevels) {
    uint32_t levels = mipLevels == 0 ? 1 : mipLevels;
    uint64_t total = 0;
    uint32_t w = width;
    uint32_t h = height;
    for (uint32_t i = 0; i < levels; ++i) {
        uint32_t cw = (w > 1) ? w : 1;
        uint32_t ch = (h > 1) ? h : 1;
        if (IsDxtFormat(format)) {
            const uint32_t blockSize = (format == 0x01) ? 8u : 16u;
            uint32_t bw = (cw + 3) / 4;
            uint32_t bh = (ch + 3) / 4;
            total += static_cast<uint64_t>(bw) * bh * blockSize;
        } else if (IsBgraFormat(format)) {
            total += static_cast<uint64_t>(cw) * ch * 4u;
        } else {
            return 0;
        }
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }
    return static_cast<uint32_t>(total);
}

static uint32_t MaxMipLevels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
        ++levels;
    }
    return levels;
}

static uint32_t ClampLevelsToPayload(uint32_t width, uint32_t height,
                                     uint8_t format, uint32_t requestedLevels,
                                     uint32_t dataSize) {
    uint32_t maxLevels = MaxMipLevels(width, height);
    uint32_t levels = requestedLevels == 0 ? 1u : requestedLevels;
    if (levels > maxLevels) levels = maxLevels;

    while (levels > 1) {
        uint32_t needed = ComputeTextureBytes(width, height, format,
                                              static_cast<uint8_t>(levels));
        if (needed != 0 && needed <= dataSize)
            break;
        --levels;
    }
    return levels;
}

static void ReleaseD3DTexture(void *tex) {
    if (!tex) return;
    auto vtable = *reinterpret_cast<uint32_t **>(tex);
    auto fnRelease = reinterpret_cast<D3DTextureRelease_fn>(vtable[2]);
    fnRelease(tex);
}

static void TouchDefaultPoolEntry(uintptr_t textureKey) {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    auto it = s_defaultPoolMap.find(textureKey);
    if (it != s_defaultPoolMap.end())
        s_defaultPoolLru.splice(s_defaultPoolLru.begin(), s_defaultPoolLru, it->second);
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
}

static void EvictDefaultPoolToBudget() {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    while (s_defaultPoolBytes > DEFAULT_POOL_BUDGET_BYTES && !s_defaultPoolLru.empty()) {
        auto &entry = s_defaultPoolLru.back();
        ReleaseD3DTexture(entry.default_tex);
        if (entry.size_bytes <= s_defaultPoolBytes)
            s_defaultPoolBytes -= entry.size_bytes;
        else
            s_defaultPoolBytes = 0;
        s_defaultPoolMap.erase(entry.texture_key);
        s_defaultPoolLru.pop_back();
        InterlockedIncrement(&s_stat_default_evictions);
    }
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
}

static void TrackDefaultPoolTexture(uintptr_t textureKey, uint64_t pathHash,
                                    void *defaultTex, uint32_t sizeBytes) {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    auto it = s_defaultPoolMap.find(textureKey);
    if (it != s_defaultPoolMap.end()) {
        s_defaultPoolBytes -= it->second->size_bytes;
        it->second->default_tex = defaultTex;
        it->second->size_bytes = sizeBytes;
        it->second->path_hash = pathHash;
        s_defaultPoolBytes += sizeBytes;
        s_defaultPoolLru.splice(s_defaultPoolLru.begin(), s_defaultPoolLru, it->second);
    } else {
        s_defaultPoolLru.push_front(DefaultPoolEntry{textureKey, pathHash, defaultTex, sizeBytes});
        s_defaultPoolMap[textureKey] = s_defaultPoolLru.begin();
        s_defaultPoolBytes += sizeBytes;
    }
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
    EvictDefaultPoolToBudget();
}

static bool OpenSharedMemory() {
    s_shmMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, TBProto::SHM_NAME);
    if (!s_shmMapping) {
        LogWrite("OpenSharedMemory: OpenFileMappingA failed, err=%lu", GetLastError());
        return false;
    }

    s_shmBase = static_cast<uint8_t *>(
        MapViewOfFile(s_shmMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!s_shmBase) {
        LogWrite("OpenSharedMemory: MapViewOfFile failed, err=%lu", GetLastError());
        CloseHandle(s_shmMapping);
        s_shmMapping = nullptr;
        return false;
    }

    auto *hdr = reinterpret_cast<const TBProto::ShmHeader *>(s_shmBase);
    if (hdr->magic != TBProto::SHM_MAGIC || hdr->version != TBProto::SHM_VERSION) {
        LogWrite("OpenSharedMemory: bad magic=%08X version=%u",
                 hdr->magic, hdr->version);
        UnmapViewOfFile(s_shmBase);
        CloseHandle(s_shmMapping);
        s_shmBase = nullptr;
        s_shmMapping = nullptr;
        return false;
    }

    LogWrite("OpenSharedMemory: OK, server PID=%llu, slots=%u",
             static_cast<unsigned long long>(hdr->server_pid), hdr->slot_count);
    return true;
}

static void CloseSharedMemory() {
    if (s_shmBase)    { UnmapViewOfFile(s_shmBase); s_shmBase = nullptr; }
    if (s_shmMapping) { CloseHandle(s_shmMapping);  s_shmMapping = nullptr; }
}

static const TBProto::SlotHeader *GetSlotHeader(int32_t slot) {
    if (!s_shmBase || slot < 0 || slot >= static_cast<int32_t>(TBProto::SLOT_COUNT))
        return nullptr;
    return reinterpret_cast<const TBProto::SlotHeader *>(
        s_shmBase + TBProto::SHM_HEADER_SIZE +
        static_cast<uint64_t>(slot) * TBProto::SLOT_TOTAL);
}

static const uint8_t *GetSlotData(int32_t slot) {
    if (!s_shmBase || slot < 0 || slot >= static_cast<int32_t>(TBProto::SLOT_COUNT))
        return nullptr;
    return s_shmBase + TBProto::SHM_HEADER_SIZE +
           static_cast<uint64_t>(slot) * TBProto::SLOT_TOTAL +
           TBProto::SLOT_HEADER_SIZE;
}

// ── Pipe I/O ─────────────────────────────────────────────────────────

static bool WritePipeFull(HANDLE pipe, const void *data, uint32_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, p, remaining, &written, nullptr) || written == 0)
            return false;
        p += written;
        remaining -= written;
    }
    return true;
}

static bool ReadPipeFull(HANDLE pipe, void *data, uint32_t size) {
    uint8_t *p = static_cast<uint8_t *>(data);
    uint32_t remaining = size;
    while (remaining > 0) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, p, remaining, &bytesRead, nullptr) || bytesRead == 0)
            return false;
        p += bytesRead;
        remaining -= bytesRead;
    }
    return true;
}

// ── Send decode request to server ────────────────────────────────────

static bool ReconnectServer() {
    LogWrite("ReconnectServer: attempting reconnect");
    CloseSharedMemory();
    s_server_available = false;
    bool ok = EnsureServerRunning();
    LogWrite("ReconnectServer: %s", ok ? "OK" : "FAILED");
    return ok;
}

static int32_t SendToServer(const char *path, const uint8_t *rawData,
                            uint32_t rawSize, uint8_t priority,
                            TBProto::Response *outResp) {
    // Retry pipe open up to 3 times on ERROR_PIPE_BUSY (231).
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        pipe = CreateFileA(
            TBProto::PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // Wait up to 500ms for a pipe instance to become available.
            WaitNamedPipeA(TBProto::PIPE_NAME, 500);
            continue;
        }
        if (attempt == 0 && err == ERROR_FILE_NOT_FOUND && ReconnectServer())
            continue;
        LogWrite("SendToServer: pipe open failed, err=%lu", err);
        return -1;
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        LogWrite("SendToServer: pipe busy after 3 retries for '%s'", path);
        return -1;
    }

    uint16_t pathLen = static_cast<uint16_t>(strlen(path));

    TBProto::Request req{};
    req.cmd = TBProto::CMD_LOAD;
    req.priority = priority;
    req.path_len = pathLen;
    req.data_size = rawData ? rawSize : 0;

    bool ok = WritePipeFull(pipe, &req, sizeof(req));
    if (ok && pathLen > 0)
        ok = WritePipeFull(pipe, path, pathLen);
    if (ok && rawData && rawSize > 0)
        ok = WritePipeFull(pipe, rawData, rawSize);

    if (!ok) {
        LogWrite("SendToServer: pipe write failed for '%s'", path);
        CloseHandle(pipe);
        return -1;
    }

    TBProto::Response resp{};
    ok = ReadPipeFull(pipe, &resp, sizeof(resp));
    CloseHandle(pipe);

    if (!ok) {
        LogWrite("SendToServer: pipe read failed for '%s'", path);
        return -1;
    }

    if (outResp) *outResp = resp;

    if (resp.status != TBProto::STATUS_OK &&
        resp.status != TBProto::STATUS_CACHED) {
        LogWrite("SendToServer: server returned status=%u for '%s'",
                 resp.status, path);
        return -1;
    }

    LogWrite("SendToServer: OK slot=%u %ux%u %u bytes for '%s'",
             resp.slot_id, resp.width, resp.height, resp.data_size, path);
    return static_cast<int32_t>(resp.slot_id);
}

// ── Read a full file via Storm's SFile API (direct calls, not hooked) ──

static uint32_t ReadFileViaStorm(const char *path, uint8_t *buf, uint32_t bufSize) {
    Game::SFile *file = nullptr;
    if (!Game::SFile_Open(path, &file) || !file) {
        LogWrite("ReadFileViaStorm: SFile_Open failed for '%s'", path);
        return 0;
    }

    uint32_t totalRead = 0;
    while (totalRead < bufSize) {
        uint32_t chunk = bufSize - totalRead;
        if (chunk > 65536) chunk = 65536;

        uint32_t bytesRead = 0;
        uint64_t ok = Game::SFile_Read(file, buf + totalRead, chunk, &bytesRead,
                                       nullptr, nullptr);
        if (!ok || bytesRead == 0)
            break;
        totalRead += bytesRead;
    }

    Game::SFile_Close(file);
    LogWrite("ReadFileViaStorm: read %u bytes from '%s'", totalRead, path);
    return totalRead;
}

// ── Directory hash for prefetch tracking ─────────────────────────────

static uint64_t DirHash(const char *path) {
    const char *last = nullptr;
    for (const char *p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') last = p;
    }
    if (!last) return 0;

    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET;
    for (const char *p = path; p < last; ++p) {
        char c = *p;
        if (c == '/')  c = '\\';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        hash *= FNV_PRIME;
    }
    return hash;
}

// ══════════════════════════════════════════════════════════════════════
//  Async Decode Worker Thread
// ══════════════════════════════════════════════════════════════════════

static DWORD WINAPI DecodeWorkerProc(LPVOID /*param*/) {
    LogWrite("Worker thread started");
    while (s_workerRunning) {
        DecodeRequest req;

        {
            AcquireSRWLockExclusive(&s_queueLock);
            while (s_queue.empty() && s_workerRunning) {
                SleepConditionVariableSRW(&s_queueCV, &s_queueLock, 200, 0);
            }
            if (!s_workerRunning && s_queue.empty()) {
                ReleaseSRWLockExclusive(&s_queueLock);
                break;
            }
            if (s_queue.empty()) {
                ReleaseSRWLockExclusive(&s_queueLock);
                continue;
            }
            req = s_queue.front();
            s_queue.pop_front();
            ReleaseSRWLockExclusive(&s_queueLock);
        }

        InterlockedIncrement(&s_inflight);
        LogWrite("Worker: processing '%s' (%u bytes, buf=%d)",
                 req.path, req.raw_size, req.buf_idx);

        uint8_t *rawBuf = s_pool.Get(req.buf_idx);
        TBProto::Response resp{};
        int32_t slot = SendToServer(req.path, rawBuf, req.raw_size,
                                    req.priority, &resp);

        s_pool.Release(req.buf_idx);
        InterlockedExchangeAdd(&s_queuedBytes,
                               -static_cast<LONG>(req.raw_size));
        InterlockedDecrement(&s_inflight);

        if (slot >= 0) {
            // Record that this texture decoded successfully on the server.
            CacheEntry entry{};
            entry.width = resp.width;
            entry.height = resp.height;
            entry.data_size = resp.data_size;
            entry.format = resp.format;
            entry.mip_levels = resp.mip_levels;
            entry.valid = true;

            AcquireSRWLockExclusive(&s_cacheLock);
            s_cache[req.path_hash] = entry;
            ReleaseSRWLockExclusive(&s_cacheLock);

            // Release the slot immediately — the server's LRU cache holds
            // the decoded pixels. A fresh slot will be allocated on demand
            // when GetDecodedTexture is called.
            ReleaseSlot(slot);

            InterlockedIncrement(&s_stat_done);
            LogWrite("Worker: decoded OK %ux%u for '%s' (slot released)",
                     resp.width, resp.height, req.path);
        } else {
            LogWrite("Worker: FAILED for '%s'", req.path);
        }
    }
    LogWrite("Worker thread exiting");
    return 0;
}

static void StartWorker() {
    if (s_workerThread) return;
    s_workerRunning = true;
    s_workerThread = CreateThread(nullptr, 0, DecodeWorkerProc,
                                  nullptr, 0, nullptr);
    LogWrite("StartWorker: thread=%p", s_workerThread);
}

static void StopWorker() {
    if (!s_workerThread) return;
    s_workerRunning = false;
    WakeAllConditionVariable(&s_queueCV);
    WaitForSingleObject(s_workerThread, 3000);
    CloseHandle(s_workerThread);
    s_workerThread = nullptr;

    AcquireSRWLockExclusive(&s_queueLock);
    while (!s_queue.empty()) {
        s_pool.Release(s_queue.front().buf_idx);
        s_queue.pop_front();
    }
    ReleaseSRWLockExclusive(&s_queueLock);
}

// ── Queue a decode request ───────────────────────────────────────────

static bool QueueDecode(const char *path, int bufIdx, uint32_t rawSize,
                        uint8_t priority) {
    LONG inflight = InterlockedCompareExchange(&s_inflight, 0, 0);
    LONG queued   = InterlockedCompareExchange(&s_queuedBytes, 0, 0);
    if (static_cast<uint32_t>(inflight) >= TBProto::MAX_INFLIGHT ||
        static_cast<uint32_t>(queued) + rawSize >
            TBProto::MAX_QUEUE_MB * 1024u * 1024u)
    {
        InterlockedIncrement(&s_stat_bp_rejects);
        LogWrite("QueueDecode: back-pressure reject for '%s'", path);
        return false;
    }

    DecodeRequest req{};
    strncpy_s(req.path, sizeof(req.path), path, _TRUNCATE);
    req.buf_idx   = bufIdx;
    req.raw_size  = rawSize;
    req.priority  = priority;
    req.path_hash = TBProto::HashPath(path);

    InterlockedExchangeAdd(&s_queuedBytes, static_cast<LONG>(rawSize));

    AcquireSRWLockExclusive(&s_queueLock);
    s_queue.push_back(req);
    ReleaseSRWLockExclusive(&s_queueLock);

    WakeConditionVariable(&s_queueCV);
    InterlockedIncrement(&s_stat_queued);
    return true;
}

// ══════════════════════════════════════════════════════════════════════
//  Phase 2: HTEXTURE Struct Probing & TextureGetGxTex Hook
// ══════════════════════════════════════════════════════════════════════
//
// Strategy: After TextureGetGxTex completes (meaning the texture is
// fully decoded + uploaded to D3D), we scan the HTEXTURE memory for
// width/height values that match our pre-decoded cache entry. By voting
// across many textures with different dimensions, we discover the struct
// layout with high confidence.
//
// Phase 2a: Probe and discover offsets (log findings)
// Phase 2b: Once offsets are confirmed, free pixel buffers after upload

// Confirmed HTEXTURE struct offsets from Phase 2a probing:
//   +0x000: uint32_t vtable (constant 0x008026B4)
//   +0x004: uint32_t type/flags
//   +0x008: char filename[260] (inline path, MAX_PATH)
//   +0x124: CStatus embedded (vtable 0x007FFA10 = VFTABLE_CSTATUS)
//   +0x138: void* async_request (non-null = still loading, null = loaded)
//   +0x140: CGxTex* gxTex (filled after decode+upload completes)
//   +0x144: uint32_t width
//   +0x148: uint32_t height
//   +0x14C: uint32_t mipLevels/formatFlag
//
// Confirmed CGxTex struct offsets:
//   gx+0x000: uint32_t flags (0x00010000 typical)
//   gx+0x004: uint32_t width
//   gx+0x008: uint32_t height
//   gx+0x014: uint32_t name_hash
//   gx+0x034: uint32_t mipCount
//   gx+0x040: HTEXTURE__* backPtr
//   gx+0x044: void* callback (constant 0x00448840)
//   gx+0x048: IDirect3DTexture9* d3dTex (COM object in 0x289xxxxx range)
//   gx+0x050: CGxTex* prevInList
static constexpr int OFF_HTEX_ASYNC_REQ = 0x138;
static constexpr int OFF_HTEX_GXTEX     = 0x140;
static constexpr int OFF_HTEX_WIDTH     = 0x144;
static constexpr int OFF_HTEX_HEIGHT    = 0x148;
static constexpr int OFF_HTEX_MIPS      = 0x14C;

static constexpr int OFF_GX_D3DTEX      = 0x048;  // IDirect3DTexture9*

// D3D9 COM vtable indices for IDirect3DTexture9.
// IDirect3DResource9::GetType = IUnknown(3) + 7 = 10
static constexpr int VTIDX_GETTYPE = 10;
// IDirect3DBaseTexture9::GetLevelCount = IUnknown(3) + IDirect3DResource9(8) + 2 = 13
static constexpr int VTIDX_GETLEVELCOUNT = 13;
// IDirect3DTexture9::GetLevelDesc = IUnknown(3) + IDirect3DResource9(8)
//                                  + IDirect3DBaseTexture9(5) + 1 = 17
static constexpr int VTIDX_GETLEVELDESC = 17;

// D3DRESOURCETYPE enum values
static constexpr uint32_t D3DRTYPE_TEXTURE = 3;

// D3DPOOL enum values
static constexpr uint32_t D3DPOOL_DEFAULT   = 0;
static constexpr uint32_t D3DPOOL_MANAGED   = 1;
static constexpr uint32_t D3DPOOL_SYSTEMMEM = 2;

// D3DSURFACE_DESC layout (32 bytes)
struct D3DSurfDesc {
    uint32_t Format;
    uint32_t Type;
    uint32_t Usage;
    uint32_t Pool;
    uint32_t MultiSampleType;
    uint32_t MultiSampleQuality;
    uint32_t Width;
    uint32_t Height;
};

// COM calling convention: __stdcall on x86 with this as first param
typedef uint32_t(__stdcall *GetType_fn)(void *pThis);
typedef HRESULT(__stdcall *GetLevelDesc_fn)(void *pThis, uint32_t Level, D3DSurfDesc *pDesc);
typedef uint32_t(__stdcall *GetLevelCount_fn)(void *pThis);

static const char *PoolName(uint32_t pool) {
    switch (pool) {
    case 0: return "DEFAULT";
    case 1: return "MANAGED";
    case 2: return "SYSTEMMEM";
    case 3: return "SCRATCH";
    default: return "UNKNOWN";
    }
}

static bool TrySwapToDefaultPool(Game::HTEXTURE__ *texture, Game::CGxTex *gxTex,
                                 uint64_t pathHash, const char *path) {
    if (!texture || !gxTex || !path)
        return false;
    const uint32_t *htexWords = reinterpret_cast<const uint32_t *>(texture);
    const uint32_t width = htexWords[OFF_HTEX_WIDTH / 4];
    const uint32_t height = htexWords[OFF_HTEX_HEIGHT / 4];
    if (width < 256 && height < 256)
        return false;

    CacheEntry cached{};
    {
        AcquireSRWLockShared(&s_cacheLock);
        auto it = s_cache.find(pathHash);
        if (it != s_cache.end() && it->second.valid)
            cached = it->second;
        ReleaseSRWLockShared(&s_cacheLock);
    }
    if (!cached.valid || !ShouldSwapFormatForPath(path, cached.format, width, height))
        return false;

    DecodedInfo info{};
    if (!GetDecodedTexture(path, nullptr, 0, info)) {
        LogWrite("DEFAULT_SWAP_FAIL: cache fetch miss '%s'", path);
        return false;
    }

    if (!ShouldSwapFormatForPath(path, info.format, width, height) ||
        info.width != width || info.height != height) {
        LogWrite("DEFAULT_SWAP_FAIL: mismatched payload '%s' payload=%ux%u fmt=%u htex=%ux%u",
                 path, info.width, info.height, info.format, width, height);
        ReleaseSlot(info.slot);
        return false;
    }

    const uint32_t *gxWords = reinterpret_cast<const uint32_t *>(gxTex);
    void *managedTex = reinterpret_cast<void *>(gxWords[OFF_GX_D3DTEX / 4]);
    if (!managedTex) {
        LogWrite("DEFAULT_SWAP_FAIL: null managed tex '%s'", path);
        ReleaseSlot(info.slot);
        return false;
    }

    auto texVtable = *reinterpret_cast<uint32_t **>(managedTex);
    auto fnGetDevice = reinterpret_cast<D3DTextureGetDevice_fn>(texVtable[3]);
    void *device = nullptr;
    HRESULT getDeviceHr = fnGetDevice(managedTex, &device);
    if (getDeviceHr != D3D_OK || !device) {
        LogWrite("DEFAULT_SWAP_FAIL: GetDevice hr=0x%08lX '%s'",
                 static_cast<unsigned long>(getDeviceHr), path);
        ReleaseSlot(info.slot);
        return false;
    }

    auto devVtable = *reinterpret_cast<uint32_t **>(device);
    auto fnCreateTexture = reinterpret_cast<D3DDeviceCreateTexture_fn>(devVtable[23]);
    auto fnUpdateTexture = reinterpret_cast<D3DDeviceUpdateTexture_fn>(devVtable[31]);
    void *defaultTex = nullptr;
    void *stagingTex = nullptr;
    const uint32_t d3dFmt = D3DFormatFromProto(info.format);
    if (d3dFmt == 0) {
        LogWrite("DEFAULT_SWAP_FAIL: unsupported format=%u '%s'", info.format, path);
        ReleaseD3DTexture(device);
        ReleaseSlot(info.slot);
        return false;
    }
    const uint32_t levels = ClampLevelsToPayload(info.width, info.height, info.format,
                                                 info.mip_levels, info.data_size);
    HRESULT hr = fnCreateTexture(device, info.width, info.height, levels,
                                 0, d3dFmt, D3DPOOL_DEFAULT_RT, &defaultTex, nullptr);
    if (hr != D3D_OK || !defaultTex) {
        LogWrite("DEFAULT_SWAP_FAIL: CreateTexture hr=0x%08lX '%s'",
                 static_cast<unsigned long>(hr), path);
        ReleaseD3DTexture(device);
        ReleaseSlot(info.slot);
        return false;
    }

    hr = fnCreateTexture(device, info.width, info.height, levels,
                         0, d3dFmt, D3DPOOL_SYSTEMMEM_RT, &stagingTex, nullptr);
    if (hr != D3D_OK || !stagingTex) {
        LogWrite("DEFAULT_SWAP_FAIL: CreateStagingTexture hr=0x%08lX '%s'",
                 static_cast<unsigned long>(hr), path);
        ReleaseD3DTexture(defaultTex);
        ReleaseD3DTexture(device);
        ReleaseSlot(info.slot);
        return false;
    }

    auto stagingVtable = *reinterpret_cast<uint32_t **>(stagingTex);
    auto fnLockRect = reinterpret_cast<D3DTextureLockRect_fn>(stagingVtable[19]);
    auto fnUnlockRect = reinterpret_cast<D3DTextureUnlockRect_fn>(stagingVtable[20]);

    const uint8_t *src = info.pixels;
    uint32_t remaining = info.data_size;
    uint32_t w = info.width;
    uint32_t h = info.height;
    for (uint32_t level = 0; level < levels; ++level) {
        D3DLOCKED_RECT rect{};
        if (fnLockRect(stagingTex, level, &rect, nullptr, 0) != D3D_OK) {
            LogWrite("DEFAULT_SWAP_FAIL: LockRect level=%u '%s'", level, path);
            ReleaseD3DTexture(stagingTex);
            ReleaseD3DTexture(defaultTex);
            ReleaseD3DTexture(device);
            ReleaseSlot(info.slot);
            return false;
        }

        const uint32_t cw = (w > 1) ? w : 1;
        const uint32_t ch = (h > 1) ? h : 1;
        uint32_t rowBytes = 0;
        uint32_t copyRows = 0;
        uint32_t levelBytes = 0;
        if (IsDxtFormat(info.format)) {
            const uint32_t blockSize = (info.format == 0x01) ? 8u : 16u;
            const uint32_t bw = (cw + 3) / 4;
            const uint32_t bh = (ch + 3) / 4;
            rowBytes = bw * blockSize;
            copyRows = bh;
            levelBytes = rowBytes * bh;
        } else if (IsBgraFormat(info.format)) {
            rowBytes = cw * 4u;
            copyRows = ch;
            levelBytes = rowBytes * ch;
        } else {
            LogWrite("DEFAULT_SWAP_FAIL: unsupported upload fmt=%u '%s'", info.format, path);
            fnUnlockRect(stagingTex, level);
            ReleaseD3DTexture(stagingTex);
            ReleaseD3DTexture(defaultTex);
            ReleaseD3DTexture(device);
            ReleaseSlot(info.slot);
            return false;
        }
        if (levelBytes > remaining) {
            LogWrite("DEFAULT_SWAP_FAIL: insufficient mip data level=%u need=%u have=%u '%s'",
                     level, levelBytes, remaining, path);
            fnUnlockRect(stagingTex, level);
            ReleaseD3DTexture(stagingTex);
            ReleaseD3DTexture(defaultTex);
            ReleaseD3DTexture(device);
            ReleaseSlot(info.slot);
            return false;
        }

        uint8_t *dst = static_cast<uint8_t *>(rect.pBits);
        for (uint32_t row = 0; row < copyRows; ++row) {
            memcpy(dst + row * rect.Pitch, src + row * rowBytes, rowBytes);
        }
        fnUnlockRect(stagingTex, level);

        src += levelBytes;
        remaining -= levelBytes;
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }

    hr = fnUpdateTexture(device, stagingTex, defaultTex);
    ReleaseD3DTexture(stagingTex);
    ReleaseD3DTexture(device);
    if (hr != D3D_OK) {
        LogWrite("DEFAULT_SWAP_FAIL: UpdateTexture hr=0x%08lX '%s'",
                 static_cast<unsigned long>(hr), path);
        ReleaseD3DTexture(defaultTex);
        ReleaseSlot(info.slot);
        return false;
    }

    uint32_t *mutableGxWords = const_cast<uint32_t *>(gxWords);
    mutableGxWords[OFF_GX_D3DTEX / 4] = reinterpret_cast<uint32_t>(defaultTex);
    ReleaseD3DTexture(managedTex);
    ReleaseSlot(info.slot);

    TrackDefaultPoolTexture(reinterpret_cast<uintptr_t>(texture), pathHash, defaultTex,
                           ComputeTextureBytes(info.width, info.height, info.format, static_cast<uint8_t>(levels)));
    InterlockedIncrement(&s_stat_default_swaps);
    LogWrite("DEFAULT_SWAP: '%s' %ux%u fmt=%u mips=%u",
             path, info.width, info.height, info.format, levels);
    return true;
}

// D3DFORMAT common values
static const char *D3DFormatName(uint32_t fmt) {
    switch (fmt) {
    case 21: return "A8R8G8B8";
    case 22: return "X8R8G8B8";
    case 827611204: return "DXT1";   // MAKEFOURCC('D','X','T','1')
    case 861165636: return "DXT3";   // MAKEFOURCC('D','X','T','3')
    case 894720068: return "DXT5";   // MAKEFOURCC('D','X','T','5')
    case 50: return "X1R5G5B5";
    case 25: return "A8R8G8B8";
    case 36: return "A16B16G16R16F";
    default: return "OTHER";
    }
}

// Accumulate total D3D managed memory for reporting.
static volatile LONG64 s_total_d3d_managed_bytes = 0;
static volatile LONG   s_total_d3d_managed_count = 0;

static void ProbeTextureStruct(Game::HTEXTURE__ *tex, uint64_t pathHash,
                                Game::CGxTex *gxTex) {
    // Get our cached decode info for this texture.
    CacheEntry cached{};
    {
        AcquireSRWLockShared(&s_cacheLock);
        auto it = s_cache.find(pathHash);
        if (it != s_cache.end() && it->second.valid)
            cached = it->second;
        ReleaseSRWLockShared(&s_cacheLock);
    }
    if (!cached.valid || cached.width == 0 || cached.height == 0)
        return;

    __try {
        const uint32_t *words = reinterpret_cast<const uint32_t *>(tex);

        LONG probeIdx = InterlockedIncrement(&s_probedCount);

        // ── Verify confirmed HTEXTURE offsets ──
        uint32_t htex_width  = words[OFF_HTEX_WIDTH / 4];
        uint32_t htex_height = words[OFF_HTEX_HEIGHT / 4];
        uint32_t htex_gxptr  = words[OFF_HTEX_GXTEX / 4];
        uint32_t htex_async  = words[OFF_HTEX_ASYNC_REQ / 4];
        uint32_t htex_mips   = words[OFF_HTEX_MIPS / 4];

        bool loaded = (htex_async == 0 && htex_width > 0);
        bool gxMatch = (htex_gxptr == reinterpret_cast<uint32_t>(gxTex));

        if (probeIdx <= 30) {
            LogWrite("PROBE #%ld: tex=%p gxTex=%p w=%u h=%u mips=%u "
                     "async=%08X gxOff=%08X gxMatch=%s loaded=%s "
                     "expect=%ux%u",
                     probeIdx, tex, gxTex,
                     htex_width, htex_height, htex_mips,
                     htex_async, htex_gxptr,
                     gxMatch ? "YES" : "NO",
                     loaded ? "YES" : "NO",
                     cached.width, cached.height);
        }

        // Skip further probing if texture not fully loaded yet.
        if (!loaded) {
            InterlockedIncrement(&s_stat_probed);
            return;
        }

        // ── Query D3D9 texture via COM vtable ──
        // CGxTex+0x048 = IDirect3DTexture9*. Call GetLevelDesc to confirm
        // pool type (MANAGED vs DEFAULT) and compute memory footprint.
        if (gxTex && probeIdx <= 30) {
            const uint32_t *gxWords = reinterpret_cast<const uint32_t *>(gxTex);
            uint32_t d3dTexPtr = gxWords[OFF_GX_D3DTEX / 4];

            if (d3dTexPtr >= 0x00100000 && d3dTexPtr <= 0x7FFF0000) {
                void *pD3DTex = reinterpret_cast<void *>(d3dTexPtr);
                uint32_t *vtable = *reinterpret_cast<uint32_t **>(pD3DTex);

                auto fnGetType = reinterpret_cast<GetType_fn>(vtable[VTIDX_GETTYPE]);
                uint32_t resourceType = fnGetType(pD3DTex);

                if (resourceType != D3DRTYPE_TEXTURE) {
                    LogWrite("  D3D9: candidate=%p GetType=%u (expected %u texture)",
                             pD3DTex, resourceType, D3DRTYPE_TEXTURE);
                    InterlockedIncrement(&s_stat_probed);
                    return;
                }

                // Get mip level count.
                auto fnGetLevelCount = reinterpret_cast<GetLevelCount_fn>(vtable[VTIDX_GETLEVELCOUNT]);
                uint32_t levels = fnGetLevelCount(pD3DTex);

                // Query level 0 for pool type and format.
                D3DSurfDesc desc{};
                auto fnGetLevelDesc = reinterpret_cast<GetLevelDesc_fn>(vtable[VTIDX_GETLEVELDESC]);
                HRESULT hr = fnGetLevelDesc(pD3DTex, 0, &desc);

                if (SUCCEEDED(hr)) {
                    // Calculate total memory across all mip levels.
                    uint64_t totalBytes = 0;
                    bool isDxt = (desc.Format == 827611204 ||   // DXT1
                                  desc.Format == 861165636 ||   // DXT3
                                  desc.Format == 894720068);    // DXT5

                    uint32_t mw = desc.Width, mh = desc.Height;
                    for (uint32_t m = 0; m < levels; ++m) {
                        uint32_t w = (mw > 1) ? mw : 1;
                        uint32_t h = (mh > 1) ? mh : 1;
                        if (isDxt) {
                            uint32_t bw = (w + 3) / 4;
                            uint32_t bh = (h + 3) / 4;
                            uint32_t blockSize = (desc.Format == 827611204) ? 8 : 16;
                            totalBytes += static_cast<uint64_t>(bw) * bh * blockSize;
                        } else {
                            // Assume 4 bytes per pixel (A8R8G8B8)
                            totalBytes += static_cast<uint64_t>(w) * h * 4;
                        }
                        mw /= 2;
                        mh /= 2;
                    }

                    // For MANAGED pool, D3D holds both VRAM + system memory copies.
                    // System memory copy = totalBytes, which is the OOM source.
                    if (desc.Pool == D3DPOOL_MANAGED) {
                        InterlockedExchangeAdd64(&s_total_d3d_managed_bytes,
                                                 static_cast<LONG64>(totalBytes));
                        InterlockedIncrement(&s_total_d3d_managed_count);
                    }

                    LogWrite("  D3D9: IDirect3DTexture9=%p type=%u pool=%s fmt=%s(%u) "
                             "%ux%u mips=%u totalBytes=%llu managed_accum=%lld(%ld tex)",
                             pD3DTex,
                             resourceType,
                             PoolName(desc.Pool),
                             D3DFormatName(desc.Format), desc.Format,
                             desc.Width, desc.Height, levels,
                             static_cast<unsigned long long>(totalBytes),
                             static_cast<long long>(s_total_d3d_managed_bytes),
                             static_cast<long>(s_total_d3d_managed_count));
                } else {
                    LogWrite("  D3D9: GetLevelDesc FAILED hr=0x%08lX", static_cast<unsigned long>(hr));
                }
            }
        }

        InterlockedIncrement(&s_stat_probed);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogWrite("PROBE: ACCESS VIOLATION reading tex=%p gxTex=%p", tex, gxTex);
    }
}

static Game::CGxTex * __fastcall TextureGetGxTex_h(
    Game::HTEXTURE__ *texture,
    int edx,                    // unused (fastcall padding for thiscall)
    Game::CStatus *status)
{
    InterlockedIncrement(&s_stat_gxtex_calls);

    // Call original — this triggers decode + D3D upload if not already done.
    Game::CGxTex *gxTex = TextureGetGxTex_o(texture, edx, status);

    // Skip if Phase 2 not active, texture invalid, or not loaded yet.
    if (!s_initialized || !s_server_available || !texture || !gxTex)
        return gxTex;

    // Look up the path hash for this HTEXTURE.
    uint64_t pathHash = 0;
    {
        AcquireSRWLockShared(&s_texMapLock);
        auto it = s_texMap.find(reinterpret_cast<uintptr_t>(texture));
        if (it != s_texMap.end())
            pathHash = it->second;
        ReleaseSRWLockShared(&s_texMapLock);
    }
    if (pathHash == 0)
        return gxTex;

    std::string texturePath;
    {
        AcquireSRWLockShared(&s_texMapLock);
        auto it = s_texPathMap.find(reinterpret_cast<uintptr_t>(texture));
        if (it != s_texPathMap.end())
            texturePath = it->second;
        ReleaseSRWLockShared(&s_texMapLock);
    }

    TouchDefaultPoolEntry(reinterpret_cast<uintptr_t>(texture));

    // Only probe each HTEXTURE once.
    bool alreadyProbed = false;
    {
        AcquireSRWLockShared(&s_probedSetLock);
        alreadyProbed = s_probedSet.count(reinterpret_cast<uintptr_t>(texture)) > 0;
        ReleaseSRWLockShared(&s_probedSetLock);
    }
    if (alreadyProbed)
        return gxTex;

    // Mark as probed.
    {
        AcquireSRWLockExclusive(&s_probedSetLock);
        s_probedSet.insert(reinterpret_cast<uintptr_t>(texture));
        ReleaseSRWLockExclusive(&s_probedSetLock);
    }

    // Probe if still within limit and texture is in our decode cache.
    if (s_probedCount < MAX_STRUCT_SCAN) {
        bool inCache = false;
        {
            AcquireSRWLockShared(&s_cacheLock);
            auto it = s_cache.find(pathHash);
            inCache = (it != s_cache.end() && it->second.valid);
            ReleaseSRWLockShared(&s_cacheLock);
        }
        if (inCache) {
            ProbeTextureStruct(texture, pathHash, gxTex);
        }
    }

    if (!texturePath.empty())
        TrySwapToDefaultPool(texture, gxTex, pathHash, texturePath.c_str());

    // ── Phase 2b: Free pixel buffer after D3D upload ──
    // TODO: Once the CGxTex struct layout is mapped and the pixel buffer
    // (or D3D managed pool system memory copy) is identified, we can:
    //   1. Read the pixel buffer pointer from CGxTex
    //   2. Free/decommit the CPU-side copy after D3D upload
    //   3. On device reset, re-populate from server's LRU cache

    return gxTex;
}

// ══════════════════════════════════════════════════════════════════════
//  TextureCreate Hook
// ══════════════════════════════════════════════════════════════════════
// Intercepts ALL texture creation. For .blp/.tga files: reads raw bytes
// via SFile, queues async decode on the server, then lets the game's
// original TextureCreate run normally as fallback.
//
// Calling convention: __fastcall (ECX=filename, EDX=status, stack=rest)

static Game::HTEXTURE__ * __fastcall TextureCreate_h(
    const char *filename,           // ECX
    Game::CStatus *status,          // EDX
    Game::CGxTexFlags texFlags,     // stack
    int unkParam1,                  // stack
    int unkParam2)                  // stack
{
    // Quick checks before any work.
    if (s_initialized && s_server_available && IsTextureFile(filename)) {

        InterlockedIncrement(&s_stat_intercepted);

        // Log first 50 intercepts, then every 100th.
        LONG count = s_stat_intercepted;
        if (count <= 50 || (count % 100) == 0)
            LogWrite("TextureCreate_h: #%ld '%s'", count, filename);

        // Already cached?
        uint64_t pathHash = TBProto::HashPath(filename);
        bool alreadyCached = false;
        {
            AcquireSRWLockShared(&s_cacheLock);
            auto it = s_cache.find(pathHash);
            alreadyCached = (it != s_cache.end() && it->second.valid);
            ReleaseSRWLockShared(&s_cacheLock);
        }

        if (alreadyCached) {
            InterlockedIncrement(&s_stat_cache_hits);
        } else {
            // Acquire pool buffer.
            int bufIdx = s_pool.Acquire();
            if (bufIdx < 0) {
                InterlockedIncrement(&s_stat_pool_misses);
                if (count <= 50)
                    LogWrite("TextureCreate_h: no pool buffer for '%s'", filename);
            } else {
                // Read raw bytes via SFile (direct, not hooked).
                uint8_t *buf = s_pool.Get(bufIdx);
                uint32_t rawSize = ReadFileViaStorm(filename, buf, POOL_BUF_SIZE);

                if (rawSize == 0) {
                    s_pool.Release(bufIdx);
                } else {
                    // Track directory for prefetch.
                    uint64_t dh = DirHash(filename);
                    if (dh != 0) {
                        s_recentDirs[s_recentDirIdx % PREFETCH_HISTORY] = dh;
                        s_recentDirIdx++;
                    }

                    // Queue async decode. On back-pressure, release buffer.
                    if (!QueueDecode(filename, bufIdx, rawSize, 128)) {
                        s_pool.Release(bufIdx);
                    }
                }
            }
        }
    }

    // ALWAYS call original — game must create its HTEXTURE regardless.
    Game::HTEXTURE__ *htex = TextureCreate_o(filename, status, texFlags, unkParam1, unkParam2);

    // Phase 2: record HTEXTURE* → pathHash for struct probing and lifecycle tracking.
    if (htex && s_initialized && IsTextureFile(filename)) {
        uint64_t ph = TBProto::HashPath(filename);
        AcquireSRWLockExclusive(&s_texMapLock);
        s_texMap[reinterpret_cast<uintptr_t>(htex)] = ph;
        s_texPathMap[reinterpret_cast<uintptr_t>(htex)] = filename;
        ReleaseSRWLockExclusive(&s_texMapLock);
    }

    return htex;
}

// ══════════════════════════════════════════════════════════════════════
//  Public API
// ══════════════════════════════════════════════════════════════════════

bool Initialize(HMODULE hVanillaHelpers) {
    s_hModule = hVanillaHelpers;

    // Determine DLL directory for log file and server exe lookup.
    if (s_hModule) {
        GetModuleFileNameA(s_hModule, s_dllDir, MAX_PATH);
        char *lastSlash = strrchr(s_dllDir, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
    }

    LogInit(s_dllDir);
    LogWrite("Initialize: hModule=%p, dir='%s'", s_hModule, s_dllDir);

    if (!s_pool.Init()) {
        LogWrite("Initialize: BufferPool.Init FAILED");
        return false;
    }
    LogWrite("Initialize: BufferPool OK (%d x %u KiB)", POOL_COUNT, POOL_BUF_SIZE / 1024);

    s_initialized = true;
    LogWrite("Initialize: done");
    return true;
}

bool EnsureServerRunning() {
    LogWrite("EnsureServerRunning: checking for existing shared memory...");

    // Check if server is already running.
    if (OpenSharedMemory()) {
        s_server_available = true;
        StartWorker();
        LogWrite("EnsureServerRunning: server already running, SHM connected");
        return true;
    }

    // Find TextureServer64.exe next to our DLL.
    std::string exePath = std::string(s_dllDir) + "TextureServer64.exe";
    LogWrite("EnsureServerRunning: looking for '%s'", exePath.c_str());

    DWORD attrs = GetFileAttributesA(exePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        LogWrite("EnsureServerRunning: exe NOT FOUND (err=%lu)", GetLastError());
        return false;
    }

    // Check for TexBridgeVisible.txt — if present, launch with --visible.
    std::string visibleFlag;
    std::string visCheck = std::string(s_dllDir) + "TexBridgeVisible.txt";
    if (GetFileAttributesA(visCheck.c_str()) != INVALID_FILE_ATTRIBUTES) {
        visibleFlag = " --visible";
        LogWrite("EnsureServerRunning: TexBridgeVisible.txt found, using --visible");
    }

    // Build command line.
    std::string cmdLine = "\"" + exePath + "\"" + visibleFlag;
    LogWrite("EnsureServerRunning: launching: %s", cmdLine.c_str());

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Use CREATE_NEW_CONSOLE when visible, CREATE_NO_WINDOW otherwise.
    DWORD flags = visibleFlag.empty() ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE;

    // CreateProcessA needs a mutable command line buffer.
    char cmdBuf[MAX_PATH * 2] = {};
    strncpy_s(cmdBuf, sizeof(cmdBuf), cmdLine.c_str(), _TRUNCATE);

    if (!CreateProcessA(
            nullptr,        // use cmdLine for exe path
            cmdBuf,         // command line (mutable)
            nullptr, nullptr,
            FALSE, flags, nullptr,
            s_dllDir,       // working directory
            &si, &pi)) {
        LogWrite("EnsureServerRunning: CreateProcessA FAILED, err=%lu", GetLastError());
        return false;
    }

    LogWrite("EnsureServerRunning: launched PID=%lu", pi.dwProcessId);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Poll for shared memory (up to 3 seconds).
    for (int i = 0; i < 30; i++) {
        Sleep(100);
        if (OpenSharedMemory()) {
            s_server_available = true;
            StartWorker();
            LogWrite("EnsureServerRunning: SHM connected after %d ms", (i + 1) * 100);
            return true;
        }
    }

    LogWrite("EnsureServerRunning: TIMEOUT waiting for SHM");
    return false;
}

bool InstallHooks() {
    LogWrite("InstallHooks: initialized=%d server_available=%d",
             s_initialized, s_server_available);

    if (!s_initialized || !s_server_available) {
        LogWrite("InstallHooks: skipping (no server)");
        return true;
    }

    // Hook TextureCreate — entry point for ALL texture loading.
    HOOK_FUNCTION(Offsets::FUN_TEXTURE_CREATE, TextureCreate_h, TextureCreate_o);
    LogWrite("InstallHooks: TextureCreate hooked OK, original=%p", TextureCreate_o);

    // Phase 2: Hook TextureGetGxTex — fires after async decode + D3D upload.
    // Used to probe HTEXTURE struct layout and (once offsets are confirmed)
    // free CPU-side pixel buffers after the GPU copy is complete.
    HOOK_FUNCTION(Offsets::FUN_TEXTURE_GET_GX_TEX, TextureGetGxTex_h, TextureGetGxTex_o);
    LogWrite("InstallHooks: TextureGetGxTex hooked OK, original=%p", TextureGetGxTex_o);

    return true;
}

void Shutdown() {
    LogWrite("Shutdown: stopping worker...");
    StopWorker();

    // Send shutdown to server (best-effort).
    if (s_server_available) {
        HANDLE pipe = CreateFileA(
            TBProto::PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            TBProto::Request req{};
            req.cmd = TBProto::CMD_SHUTDOWN;
            DWORD written;
            WriteFile(pipe, &req, sizeof(req), &written, nullptr);
            CloseHandle(pipe);
            LogWrite("Shutdown: sent CMD_SHUTDOWN to server");
        }
    }

    AcquireSRWLockExclusive(&s_cacheLock);
    s_cache.clear();
    ReleaseSRWLockExclusive(&s_cacheLock);

    // Phase 2: clear texture tracking maps.
    AcquireSRWLockExclusive(&s_texMapLock);
    s_texMap.clear();
    s_texPathMap.clear();
    ReleaseSRWLockExclusive(&s_texMapLock);

    AcquireSRWLockExclusive(&s_probedSetLock);
    s_probedSet.clear();
    ReleaseSRWLockExclusive(&s_probedSetLock);

    CloseSharedMemory();
    s_pool.Destroy();
    s_server_available = false;
    s_initialized = false;

    // Report Phase 2 struct discovery results.
    LogWrite("Shutdown: Phase2 probed=%ld gxtexCalls=%ld freed=%ld",
             s_stat_probed, s_stat_gxtex_calls,
             s_stat_freed_texbufs);
    LogWrite("Shutdown: D3D managed textures=%ld total_system_mem=%lld MB",
             static_cast<long>(s_total_d3d_managed_count),
             static_cast<long long>(s_total_d3d_managed_bytes / (1024 * 1024)));

    LogWrite("Shutdown: complete. Stats: intercepted=%ld queued=%ld done=%ld "
             "cacheHits=%ld syncFallback=%ld bpRejects=%ld poolMisses=%ld",
             s_stat_intercepted, s_stat_queued, s_stat_done,
             s_stat_cache_hits, s_stat_sync_fallback,
             s_stat_bp_rejects, s_stat_pool_misses);
    LogClose();
}

void OnFrameTick() {
    EvictDefaultPoolToBudget();
}

bool GetDecodedTexture(const char *path, const void *rawData, uint32_t rawSize,
                       DecodedInfo &info) {
    if (!s_initialized || !s_server_available) return false;
    if (!IsTextureFile(path)) return false;

    uint64_t pathHash = TBProto::HashPath(path);
    const uint8_t *rawBytes = static_cast<const uint8_t *>(rawData);
    uint32_t ownedRawSize = 0;
    std::vector<uint8_t> ownedRaw;

    // 1. Check if the server has this texture in its LRU cache (pre-decoded
    //    by the async pipeline). If so, request a fresh slot — the server
    //    will respond instantly from its cache.
    bool serverHasIt = false;
    {
        AcquireSRWLockShared(&s_cacheLock);
        auto it = s_cache.find(pathHash);
        serverHasIt = (it != s_cache.end() && it->second.valid);
        ReleaseSRWLockShared(&s_cacheLock);
    }

    if (serverHasIt) {
        InterlockedIncrement(&s_stat_cache_hits);

        // Ask the server for a fresh slot from its cache. Raw bytes are only
        // needed if the cache missed and we must fall back to a decode.
        TBProto::Response resp{};
        int32_t slot = SendToServer(path, nullptr, 0, 0, &resp);
        if (slot >= 0) {
            const TBProto::SlotHeader *sh = GetSlotHeader(slot);
            const uint8_t *pixels = GetSlotData(slot);
            if (sh && pixels && sh->state == TBProto::STATE_READY) {
                info.slot      = slot;
                info.width     = sh->width;
                info.height    = sh->height;
                info.data_size = sh->data_size;
                info.format    = sh->format;
                info.mip_levels = sh->mip_levels;
                info.pixels    = pixels;
                return true;
            }
        }

        if (resp.status == TBProto::STATUS_NOT_FOUND) {
            AcquireSRWLockExclusive(&s_cacheLock);
            auto it = s_cache.find(pathHash);
            if (it != s_cache.end()) {
                it->second.valid = false;
            }
            ReleaseSRWLockExclusive(&s_cacheLock);
            LogWrite("GetDecodedTexture: invalidated stale cache hint for '%s'", path);
        }
        // Fall through to sync fallback if slot request failed.
    }

    // 2. Synchronous fallback: full decode request.
    if (!rawBytes || rawSize == 0) {
        Game::SFile *file = nullptr;
        if (!Game::SFile_Open(path, &file) || !file) {
            LogWrite("GetDecodedTexture: SFile_Open fallback failed for '%s'", path);
            return false;
        }

        constexpr uint32_t chunkSize = 64 * 1024;
        ownedRaw.resize(chunkSize);
        while (true) {
            uint32_t bytesRead = 0;
            uint64_t ok = Game::SFile_Read(file, ownedRaw.data() + ownedRawSize,
                                           chunkSize, &bytesRead, nullptr, nullptr);
            if (!ok || bytesRead == 0) {
                break;
            }
            ownedRawSize += bytesRead;
            if (bytesRead < chunkSize) {
                break;
            }
            ownedRaw.resize(ownedRawSize + chunkSize);
        }
        Game::SFile_Close(file);

        if (ownedRawSize == 0) {
            LogWrite("GetDecodedTexture: raw fallback read failed for '%s'", path);
            return false;
        }

        ownedRaw.resize(ownedRawSize);
        LogWrite("GetDecodedTexture: loaded %u raw bytes for stale cache miss '%s'",
                 ownedRawSize, path);
        rawBytes = ownedRaw.data();
        rawSize = ownedRawSize;
    }

    InterlockedIncrement(&s_stat_sync_fallback);

    TBProto::Response resp{};
    int32_t slot = SendToServer(path,
                                rawBytes,
                                rawSize, 128, &resp);
    if (slot < 0) return false;

    const TBProto::SlotHeader *sh = GetSlotHeader(slot);
    const uint8_t *pixels = GetSlotData(slot);
    if (!sh || !pixels) return false;
    if (sh->state != TBProto::STATE_READY) return false;

    info.slot      = slot;
    info.width     = sh->width;
    info.height    = sh->height;
    info.data_size = sh->data_size;
    info.format    = sh->format;
    info.mip_levels = sh->mip_levels;
    info.pixels    = pixels;

    // Cache the success for future lookups.
    CacheEntry ce{};
    ce.width     = resp.width;
    ce.height    = resp.height;
    ce.data_size = resp.data_size;
    ce.format    = resp.format;
    ce.mip_levels = resp.mip_levels;
    ce.valid     = true;

    AcquireSRWLockExclusive(&s_cacheLock);
    s_cache[pathHash] = ce;
    ReleaseSRWLockExclusive(&s_cacheLock);

    return true;
}

void ReleaseSlot(int32_t slot) {
    if (!s_shmBase || slot < 0 || slot >= static_cast<int32_t>(TBProto::SLOT_COUNT))
        return;

    volatile LONG *state = reinterpret_cast<volatile LONG *>(
        s_shmBase + TBProto::SHM_HEADER_SIZE +
        static_cast<uint64_t>(slot) * TBProto::SLOT_TOTAL);

    InterlockedExchange(state, static_cast<LONG>(TBProto::STATE_EMPTY));
}

void GetStats(PipelineStats &out) {
    out.textures_intercepted  = static_cast<uint32_t>(s_stat_intercepted);
    out.async_decodes_queued  = static_cast<uint32_t>(s_stat_queued);
    out.async_decodes_done    = static_cast<uint32_t>(s_stat_done);
    out.cache_hits            = static_cast<uint32_t>(s_stat_cache_hits);
    out.sync_fallbacks        = static_cast<uint32_t>(s_stat_sync_fallback);
    out.back_pressure_rejects = static_cast<uint32_t>(s_stat_bp_rejects);
    out.buffer_pool_misses    = static_cast<uint32_t>(s_stat_pool_misses);

    // Phase 2
    out.gxtex_calls           = static_cast<uint32_t>(s_stat_gxtex_calls);
    out.struct_probes         = static_cast<uint32_t>(s_stat_probed);
    out.discovered_width_off  = static_cast<int32_t>(s_off_width);
    out.discovered_height_off = static_cast<int32_t>(s_off_height);
    out.discovered_pixbuf_off = static_cast<int32_t>(s_off_pixelbuf);
    out.freed_texbufs         = static_cast<uint32_t>(s_stat_freed_texbufs);
}

} // namespace TexBridge
