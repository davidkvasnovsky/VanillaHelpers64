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
#include <deque>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <intrin.h>
#include <process.h>

// ── Protocol constants (mirror of Protocol.h to avoid cross-project include) ──
namespace TBProto {
    static constexpr const char *PIPE_NAME = "\\\\.\\pipe\\VH_TextureServer";
    static constexpr const char *SHM_NAME = "VH_TexServer_SharedMem";
    static constexpr const char *SHM_DATA_NAME_PREFIX = "VH_TexServer_SharedMem_Data";
    static constexpr uint32_t SLOT_COUNT = 64;
    static constexpr uint32_t SLOT_DATA_SIZE = 4u * 1024u * 1024u;
    static constexpr uint32_t SLOT_HEADER_SIZE = 64;
    static constexpr uint32_t SLOT_TOTAL = SLOT_HEADER_SIZE + SLOT_DATA_SIZE;
    static constexpr uint32_t SHM_WINDOW_COUNT = 4;
    static constexpr uint32_t SLOTS_PER_WINDOW = SLOT_COUNT / SHM_WINDOW_COUNT;
    static constexpr uint32_t SHM_HEADER_SIZE = 4096;
    static constexpr uint64_t SHM_DATA_WINDOW_SIZE =
        static_cast<uint64_t>(SLOTS_PER_WINDOW) * SLOT_TOTAL;
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
// Writes to TexBridge.log next to the DLL. Flushed periodically
// (every ~60 ticks in OnFrameTick) for performance under Wine.

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
typedef ULONG(__stdcall *D3DTextureAddRef_fn)(void *pThis);
typedef HRESULT(__stdcall *D3DTextureRelease_fn)(void *pThis);
typedef HRESULT(__stdcall *D3DTextureGetDevice_fn)(void *pThis, void **ppDevice);
typedef HRESULT(__stdcall *D3DDeviceUpdateTexture_fn)(void *pThis, void *srcTexture, void *dstTexture);
typedef uintptr_t(__fastcall *TextureAllocMain_t)(void *thisptr, void *edx,
                                                  uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
typedef void(__fastcall *TextureAllocFreePayload_t)(void *thisptr, void *edx,
                                                    void *payload, uintptr_t arg2, uintptr_t arg3);
typedef uintptr_t(__fastcall *RetainedPayloadRead_t)(void *thisptr, void *edx,
                                                     void *dst, uintptr_t size);
typedef void(__fastcall *ReturnRetainedPayload_t)(void *thisptr, void *edxPayload);

static FILE *s_logFile = nullptr;
static SRWLOCK s_logLock = SRWLOCK_INIT;
static bool s_fullLogEnabled = false;

static bool StartsWithLogPrefix(const char *text, const char *prefix) {
    if (!text || !prefix)
        return false;
    size_t prefixLen = strlen(prefix);
    return strncmp(text, prefix, prefixLen) == 0;
}

static bool ShouldSuppressCompactLog(const char *fmt) {
    if (!fmt || s_fullLogEnabled)
        return false;

    static const char *const noisyPrefixes[] = {
        "DEFAULT_SWAP_INSTALL:",
        "DEFAULT_SWAP_TRACK:",
        "DEFAULT_SWAP:",
        "DEFAULT_SWAP_TOUCH:",
        "DEFAULT_SWAP_TOUCH_CANCEL_EVICT:",
        "DEFAULT_SWAP_EVICT_MARK:",
        "DEFAULT_SWAP_EVICT:",
        "DEFAULT_SWAP_RELEASE:",
        "DEFAULT_SWAP_MANAGED_HOLD:",
        "DEFAULT_SWAP_MANAGED_HOLD_RELEASE:",
        "DEFAULT_SWAP_MANAGED_HOLD_REPLACE:",
        "TEXTURE_DESTROY:",
        "SendToServer: OK",
        "ReadFileViaStorm: read",
        "Worker: processing",
        "Worker: decoded OK",
        "TextureCreate_h: #",
        "PROBE #",
    };

    for (const char *prefix : noisyPrefixes) {
        if (StartsWithLogPrefix(fmt, prefix))
            return true;
    }
    return false;
}

static void LogInit(const char *dllDir) {
    if (s_logFile) return;

    std::string path = std::string(dllDir) + "TexBridge.log";
    std::string fullLogPath = std::string(dllDir) + "TexBridgeFullLog.txt";
    s_fullLogEnabled = (GetFileAttributesA(fullLogPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    fopen_s(&s_logFile, path.c_str(), "w");
    if (s_logFile) {
        fprintf(s_logFile, "[TexBridge] Log started\n");
        fprintf(s_logFile, "[TexBridge] Log mode: %s\n",
                s_fullLogEnabled ? "full" : "compact");
        fflush(s_logFile);
    }
}

static void LogWrite(const char *fmt, ...) {
    if (!s_logFile) return;
    if (ShouldSuppressCompactLog(fmt))
        return;
    AcquireSRWLockExclusive(&s_logLock);
    va_list args;
    va_start(args, fmt);
    fprintf(s_logFile, "[TexBridge] ");
    vfprintf(s_logFile, fmt, args);
    fprintf(s_logFile, "\n");
    va_end(args);
    ReleaseSRWLockExclusive(&s_logLock);
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

static constexpr int    POOL_COUNT = 24;
static constexpr uint32_t POOL_BUF_SIZE = 2u * 1024u * 1024u;

struct BufferPool {
    uint8_t *buffers[POOL_COUNT] = {};
    volatile LONG locks[POOL_COUNT] = {};

    bool Init() {
        if (buffers[0]) return true;  // already initialized
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

// Deferred file reads — TextureCreate_h records filenames here instead of
// doing synchronous MPQ I/O immediately.  OnFrameTick drains a few per frame.
// Both run on the main thread so no lock is needed.
struct DeferredRead {
    char     path[260];
    uint64_t path_hash;
};
static std::deque<DeferredRead> s_deferredReads;
static std::unordered_set<uint64_t> s_deferredReadHashes; // dedup guard
static constexpr int MAX_DEFERRED_READS_PER_FRAME = 1;

// Main-thread only — enqueue a deferred file read with dedup.
static void EnqueueDeferredRead(const char *path, uint64_t pathHash) {
    if (s_deferredReadHashes.find(pathHash) != s_deferredReadHashes.end())
        return;
    DeferredRead dr{};
    strncpy_s(dr.path, sizeof(dr.path), path, _TRUNCATE);
    dr.path_hash = pathHash;
    s_deferredReads.push_back(dr);
    s_deferredReadHashes.insert(pathHash);
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════════
//  Module State
// ══════════════════════════════════════════════════════════════════════

static bool    s_initialized = false;
static bool    s_server_available = false;
static HMODULE s_hModule = nullptr;
static char    s_dllDir[MAX_PATH] = {};
static DWORD   s_swap_enable_tick = 0;
static DWORD   s_swap_world_ready_tick = 0;
static bool    s_swap_world_ready_logged = false;

// Shared memory
static HANDLE   s_shmHeaderMapping = nullptr;
static uint8_t *s_shmHeaderBase = nullptr;
static HANDLE   s_shmDataMappings[TBProto::SHM_WINDOW_COUNT] = {};
static uint8_t *s_shmDataBases[TBProto::SHM_WINDOW_COUNT] = {};

// Buffer pool
static BufferPool s_pool;

// ── Lock ordering (must always nest in this order to prevent deadlock) ──
//   s_cacheLock  →  s_texMapLock        (DecodeWorkerProc cache eviction)
//   s_defaultPoolLock  →  s_texMapLock  (ReleaseTrackedTexture → GetTrackedTexturePath)
// No other nesting is permitted.  All other locks are acquired independently.

// Decode cache
static SRWLOCK s_cacheLock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, CacheEntry> s_cache;

// Async decode queue
static SRWLOCK s_queueLock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_queueCV = CONDITION_VARIABLE_INIT;
static std::deque<DecodeRequest> s_queue;
static std::unordered_set<uint64_t> s_pendingDecodes;
static constexpr int WORKER_COUNT = 4;
static HANDLE  s_workerThreads[WORKER_COUNT] = {};
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
static volatile LONG s_stat_default_reuploads = 0;
static volatile LONG s_stat_decode_dedup_skips = 0;
static volatile LONG64 s_stat_default_swapped_bytes = 0;
static volatile LONG64 s_stat_default_evicted_bytes = 0;
static volatile LONG s_stat_last_runtime_metrics_swaps = 0;
static volatile LONG s_stat_default_touch_logs = 0;

// Per-frame upload throttle — caps D3D9 texture uploads per frame to avoid
// stalling the render thread under DXVK (each upload triggers a Vulkan
// command buffer submit through MoltenVK→Metal).
static volatile LONG s_uploadsThisFrame = 0;
static constexpr LONG MAX_UPLOADS_PER_FRAME = 3;

// Prefetch tracking
static constexpr int PREFETCH_HISTORY = 32;
static uint64_t s_recentDirs[PREFETCH_HISTORY] = {};
static int      s_recentDirIdx = 0;

// Original function pointer for hooked TextureCreate
static Game::TextureCreate_t TextureCreate_o = nullptr;
static Game::TextureDestroy_t TextureDestroy_o = nullptr;
static Game::GxTexOwnerUpdate_t GxTexOwnerUpdate_o = nullptr;
static TextureAllocMain_t TextureAllocMain_o = nullptr;
static TextureAllocFreePayload_t TextureAllocFreePayload_o = nullptr;
static RetainedPayloadRead_t RetainedPayloadRead_o = nullptr;
static ReturnRetainedPayload_t ReturnRetainedPayload_o = nullptr;

struct FocusedMainBranchContext {
    uintptr_t size = 0;
    uintptr_t arg2 = 0;
    uintptr_t arg3 = 0;
    uintptr_t result = 0;
    unsigned depth = 0;
};

static thread_local FocusedMainBranchContext s_focusedMain0573 = {};
static thread_local uintptr_t s_activeTextureAllocKey = 0;
static thread_local uint64_t s_activeTextureAllocPathHash = 0;

struct PayloadRecord {
    uintptr_t texture_key = 0;
    uint64_t  path_hash = 0;
    void     *allocator = nullptr;
    void     *raw_block = nullptr;
    void     *payload = nullptr;
    uint32_t  size_class = 0;
    uint32_t  size = 0;
    uintptr_t arg2 = 0;
    uintptr_t arg3 = 0;
};
using PendingMain0573Payload = PayloadRecord;
using EarlyReleasedPayload   = PayloadRecord;

static SRWLOCK s_main0573PayloadLock = SRWLOCK_INIT;
static std::unordered_map<uintptr_t, PendingMain0573Payload> s_pendingMain0573Payloads;
static std::unordered_map<uintptr_t, EarlyReleasedPayload> s_earlyReleasedPayloads;

struct DefaultPoolEntry {
    uintptr_t texture_key;
    uint64_t  path_hash;
    void     *default_tex;
    void     *managed_tex;
    uint32_t  size_bytes;
    uint8_t   residency_class;
    uint8_t   protect_passes;
    bool      pending_eviction;
    bool      managed_ref_held;
    mutable volatile LONG last_validated_tick;
};

static bool RestoreManagedTextureBinding(const DefaultPoolEntry &entry);
static bool IsLiveTextureBindingUsable(void *liveTex, void *expectedDefaultTex,
                                       void *expectedManagedTex);
static void OnTextureDestroy(Game::HTEXTURE__ *texture);
static void ClearPendingMain0573Payload(uintptr_t textureKey);
static bool IsSwapStartupDeferred();
static bool IsSwapWorldDeferred();

static SRWLOCK s_defaultPoolLock = SRWLOCK_INIT;
static std::list<DefaultPoolEntry> s_defaultPoolLru;
static std::unordered_map<uintptr_t, std::list<DefaultPoolEntry>::iterator> s_defaultPoolMap;
static std::unordered_map<uintptr_t, void *> s_restoredManagedRefs;
static size_t s_defaultPoolBytes = 0;
static size_t s_defaultPoolPeakBytes = 0;
static constexpr size_t DEFAULT_POOL_BUDGET_BYTES = 768u * 1024u * 1024u;
static constexpr LONG RUNTIME_METRICS_SWAP_INTERVAL = 250;
static constexpr DWORD SWAP_STARTUP_GRACE_MS = 15000;
static constexpr DWORD SWAP_WORLD_GRACE_MS = 2000;

// ── Device reset detection ──────────────────────────────────────────
// D3DPOOL_DEFAULT textures are lost on device reset (window maximize/resize).
// Detect via TestCooperativeLevel and flush all DEFAULT textures proactively.
static void *s_cachedD3DDevice = nullptr;
static bool s_wasDeviceLost = false;

// IDirect3DDevice9 vtable index for TestCooperativeLevel
static constexpr uint32_t VTIDX_TESTCOOPERATIVELEVEL = 3;

static void FlushAllDefaultPoolTextures(const char *reason);

// ── D3D9 Device Reset Hook ──────────────────────────────────────────
// Hook IDirect3DDevice9::Reset via vtable patching. This fires BEFORE
// WineD3D processes the reset, giving us a guaranteed window to release
// all D3DPOOL_DEFAULT textures (which are invalidated by Reset).

typedef HRESULT(__stdcall *D3DDeviceReset_fn)(void *pThis, void *pPresentationParameters);
static D3DDeviceReset_fn s_origDeviceReset = nullptr;
static bool s_deviceResetHooked = false;

static HRESULT __stdcall DeviceReset_Hook(void *pThis, void *pPresentationParameters) {
    FlushAllDefaultPoolTextures("device-reset");
    return s_origDeviceReset(pThis, pPresentationParameters);
}

static void HookDeviceReset(void *device) {
    if (s_deviceResetHooked || !device) return;

    uint32_t *vtable = *reinterpret_cast<uint32_t **>(device);
    if (!vtable) return;

    // IDirect3DDevice9::Reset is vtable index 16
    uint32_t *resetSlot = &vtable[16];

    s_origDeviceReset = reinterpret_cast<D3DDeviceReset_fn>(*resetSlot);

    DWORD oldProt = 0;
    if (VirtualProtect(resetSlot, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
        *resetSlot = reinterpret_cast<uint32_t>(DeviceReset_Hook);
        VirtualProtect(resetSlot, sizeof(uint32_t), oldProt, &oldProt);
        s_deviceResetHooked = true;
        LogWrite("D3D9 Device Reset hook installed on vtable[16]=%p", s_origDeviceReset);
    }
}

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
static std::unordered_set<uintptr_t> s_swapQuarantineTex;
static std::unordered_set<uint64_t> s_swapQuarantinePaths;
static std::unordered_set<uint64_t> s_swapInlineWarnedPaths;
static thread_local const char *s_activeTextureAllocPath = nullptr;

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

static bool MeetsSwapSizeThreshold(uint32_t width, uint32_t height) {
    return width >= 256 || height >= 256;
}

static bool ShouldQueueDecodeForPath(const char *path) {
    return IsTargetTexturePath(path);
}

static bool ShouldSwapFormatForPath(const char *path, uint8_t format,
                                    uint32_t width, uint32_t height) {
    if (IsDxtFormat(format))
        return IsTargetTexturePath(path) && MeetsSwapSizeThreshold(width, height);
    if (IsBgraFormat(format))
        return IsTargetTexturePath(path) && MeetsSwapSizeThreshold(width, height);
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

// Validate a COM-style object pointer: readable object, readable vtable, non-null entries.
// Returns the vtable pointer on success, nullptr on failure.
static uint32_t *ValidateD3DObject(void *obj, SIZE_T vtableBytes, const char *op) {
    if (IsBadReadPtr(obj, sizeof(void *))) {
        LogWrite("D3D9_TEXTURE_INVALID: op=%s tex=%p bad-ptr", op, obj);
        return nullptr;
    }
    auto vtable = *reinterpret_cast<uint32_t **>(obj);
    if (!vtable || IsBadReadPtr(vtable, vtableBytes)) {
        LogWrite("D3D9_TEXTURE_INVALID: op=%s tex=%p vtable=%p", op, obj, vtable);
        return nullptr;
    }
    return vtable;
}

static void ReleaseD3DTexture(void *tex) {
    if (!tex) return;
    auto *vtable = ValidateD3DObject(tex, 12, "Release");
    if (!vtable) return;
    if (!vtable[0] || !vtable[1] || !vtable[2]) {
        LogWrite("D3D9_TEXTURE_INVALID: op=Release tex=%p vtable=%p null-entry", tex, vtable);
        return;
    }
    auto fnRelease = reinterpret_cast<D3DTextureRelease_fn>(vtable[2]);
    fnRelease(tex);
}

static ULONG AddRefD3DTexture(void *tex) {
    if (!tex) return 0;
    auto *vtable = ValidateD3DObject(tex, 12, "AddRef");
    if (!vtable) return 0;
    if (!vtable[0] || !vtable[1] || !vtable[2]) {
        LogWrite("D3D9_TEXTURE_INVALID: op=AddRef tex=%p vtable=%p null-entry", tex, vtable);
        return 0;
    }
    auto fnAddRef = reinterpret_cast<D3DTextureAddRef_fn>(vtable[1]);
    return fnAddRef(tex);
}

static std::string GetTrackedTexturePath(uintptr_t textureKey) {
    std::string path;
    AcquireSRWLockShared(&s_texMapLock);
    auto it = s_texPathMap.find(textureKey);
    if (it != s_texPathMap.end())
        path = it->second;
    ReleaseSRWLockShared(&s_texMapLock);
    return path;
}

static const char *GetActiveTextureAllocPathForLog() {
    return (s_activeTextureAllocPath && *s_activeTextureAllocPath) ? s_activeTextureAllocPath
                                                                   : "<none>";
}

static bool IsFocusedMain0573(uintptr_t arg2, uintptr_t arg3) {
    return arg2 == 0x00866650 && arg3 == 0x573;
}

void LogFocusedMain0573Backend(void *allocator, uint32_t sizeClass, uint32_t size,
                               uint32_t commit, void *result) {
    if (!s_server_available || s_focusedMain0573.depth == 0)
        return;

    if (!commit || !result || !s_activeTextureAllocKey)
        return;

    uintptr_t payload = reinterpret_cast<uintptr_t>(result) + 8;
    if (size != s_focusedMain0573.size || payload != s_focusedMain0573.result)
        return;

    PendingMain0573Payload pending{};
    pending.texture_key = s_activeTextureAllocKey;
    pending.path_hash = s_activeTextureAllocPathHash;
    pending.allocator = allocator;
    pending.raw_block = result;
    pending.payload = reinterpret_cast<void *>(payload);
    pending.size_class = sizeClass;
    pending.size = size;
    pending.arg2 = s_focusedMain0573.arg2;
    pending.arg3 = s_focusedMain0573.arg3;

    AcquireSRWLockExclusive(&s_main0573PayloadLock);
    s_pendingMain0573Payloads[s_activeTextureAllocKey] = pending;
    ReleaseSRWLockExclusive(&s_main0573PayloadLock);
}

static uintptr_t __fastcall TextureAllocMain_h(void *thisptr, void * /*edx*/,
                                               uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
    const bool focused0573 = IsFocusedMain0573(arg2, arg3);
    if (focused0573) {
        ++s_focusedMain0573.depth;
        s_focusedMain0573.size = arg1;
        s_focusedMain0573.arg2 = arg2;
        s_focusedMain0573.arg3 = arg3;
    }
    uintptr_t result = TextureAllocMain_o(thisptr, nullptr, arg1, arg2, arg3);
    if (focused0573)
        s_focusedMain0573.result = result;
    if (focused0573 && s_focusedMain0573.depth > 0)
        --s_focusedMain0573.depth;
    return result;
}

static void ReleaseManagedRefHoldLocked(uintptr_t textureKey, const char *reason) {
    auto it = s_restoredManagedRefs.find(textureKey);
    if (it == s_restoredManagedRefs.end())
        return;
    LogWrite("DEFAULT_SWAP_MANAGED_HOLD_RELEASE: reason=%s texture=%p managedTex=%p",
             reason ? reason : "unknown",
             reinterpret_cast<void *>(textureKey),
             it->second);
    ReleaseD3DTexture(it->second);
    s_restoredManagedRefs.erase(it);
}

static void StoreManagedRefHoldLocked(const DefaultPoolEntry &entry, const char *reason) {
    if (!entry.texture_key || !entry.managed_tex || !entry.managed_ref_held)
        return;
    auto it = s_restoredManagedRefs.find(entry.texture_key);
    if (it != s_restoredManagedRefs.end() && it->second != entry.managed_tex) {
        LogWrite("DEFAULT_SWAP_MANAGED_HOLD_REPLACE: reason=%s texture=%p oldManaged=%p newManaged=%p",
                 reason ? reason : "unknown",
                 reinterpret_cast<void *>(entry.texture_key),
                 it->second,
                 entry.managed_tex);
        ReleaseD3DTexture(it->second);
    }
    s_restoredManagedRefs[entry.texture_key] = entry.managed_tex;
    LogWrite("DEFAULT_SWAP_MANAGED_HOLD: reason=%s texture=%p managedTex=%p",
             reason ? reason : "unknown",
             reinterpret_cast<void *>(entry.texture_key),
             entry.managed_tex);
}

// Returns true if the texture was actually released, false if eviction was
// skipped because the managed restore failed (DEFAULT texture still in use).
static bool ReleaseTrackedTexture(DefaultPoolEntry &entry, bool allowRestore,
                                  const char *reason) {
    std::string path = GetTrackedTexturePath(entry.texture_key);
    bool restored = allowRestore && RestoreManagedTextureBinding(entry);

    if (!restored && allowRestore) {
        // Managed restore failed — the D3D pointer in CGxTex no longer matches
        // our DEFAULT texture (LOD transition, game texture reload, etc.).
        // Do NOT release either texture; the DEFAULT is still valid and the
        // game may still be referencing it.  Leave the entry in the LRU so
        // the next eviction pass can retry once the game finishes its update.
        LogWrite("DEFAULT_SWAP_RELEASE_SKIP: reason=%s texture=%p path='%s' defaultTex=%p managedTex=%p",
                 reason ? reason : "unknown",
                 reinterpret_cast<void *>(entry.texture_key),
                 path.empty() ? "<unknown>" : path.c_str(),
                 entry.default_tex,
                 entry.managed_tex);
        entry.pending_eviction = false;
        return false;
    }

    LogWrite("DEFAULT_SWAP_RELEASE: reason=%s texture=%p path='%s' restored=%d defaultTex=%p managedTex=%p size=%u",
             reason ? reason : "unknown",
             reinterpret_cast<void *>(entry.texture_key),
             path.empty() ? "<unknown>" : path.c_str(),
             restored ? 1 : 0,
             entry.default_tex,
             entry.managed_tex,
             static_cast<unsigned>(entry.size_bytes));
    ReleaseD3DTexture(entry.default_tex);
    entry.default_tex = nullptr;
    if (restored && entry.managed_ref_held) {
        StoreManagedRefHoldLocked(entry, reason);
    } else if (entry.managed_ref_held) {
        ReleaseD3DTexture(entry.managed_tex);
    }
    entry.managed_tex = nullptr;
    entry.pending_eviction = false;
    entry.managed_ref_held = false;
    return true;
}

static void ReleaseTrackedTextureOnShutdown(DefaultPoolEntry &entry) {
    std::string path = GetTrackedTexturePath(entry.texture_key);
    bool restored = RestoreManagedTextureBinding(entry);
    LogWrite("DEFAULT_SWAP_RELEASE: reason=shutdown texture=%p path='%s' restored=%d defaultTex=%p managedTex=%p size=%u",
             reinterpret_cast<void *>(entry.texture_key),
             path.empty() ? "<unknown>" : path.c_str(),
             restored ? 1 : 0,
             entry.default_tex,
             entry.managed_tex,
             static_cast<unsigned>(entry.size_bytes));

    if (restored) {
        ReleaseD3DTexture(entry.default_tex);
    } else if (entry.default_tex) {
        LogWrite("DEFAULT_SWAP_SHUTDOWN_KEEP: texture=%p path='%s' defaultTex=%p",
                 reinterpret_cast<void *>(entry.texture_key),
                 path.empty() ? "<unknown>" : path.c_str(),
                 entry.default_tex);
    }

    if (entry.managed_ref_held && entry.managed_tex)
        ReleaseD3DTexture(entry.managed_tex);

    entry.default_tex = nullptr;
    entry.managed_tex = nullptr;
    entry.pending_eviction = false;
    entry.managed_ref_held = false;
}

static void ClearDefaultPoolTracking(uintptr_t textureKey, bool allowRestore, const char *reason) {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    auto poolIt = s_defaultPoolMap.find(textureKey);
    if (poolIt != s_defaultPoolMap.end()) {
        auto entryIt = poolIt->second;
        if (entryIt->size_bytes <= s_defaultPoolBytes)
            s_defaultPoolBytes -= entryIt->size_bytes;
        else
            s_defaultPoolBytes = 0;
        ReleaseTrackedTexture(*entryIt, allowRestore, reason);
        s_defaultPoolMap.erase(poolIt);
        s_defaultPoolLru.erase(entryIt);
    }
    ReleaseManagedRefHoldLocked(textureKey, reason);
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
}

static void ClearTrackedTextureState(uintptr_t textureKey) {
    ClearDefaultPoolTracking(textureKey, true, "clear-tracked-state");
    ClearPendingMain0573Payload(textureKey);

    AcquireSRWLockExclusive(&s_probedSetLock);
    s_probedSet.erase(textureKey);
    ReleaseSRWLockExclusive(&s_probedSetLock);

    AcquireSRWLockExclusive(&s_texMapLock);
    s_texMap.erase(textureKey);
    s_texPathMap.erase(textureKey);
    s_swapQuarantineTex.erase(textureKey);
    ReleaseSRWLockExclusive(&s_texMapLock);
}

static void ClearPendingMain0573Payload(uintptr_t textureKey) {
    if (!textureKey)
        return;
    AcquireSRWLockExclusive(&s_main0573PayloadLock);
    s_pendingMain0573Payloads.erase(textureKey);
    ReleaseSRWLockExclusive(&s_main0573PayloadLock);
}

static bool GetPendingMain0573Payload(uintptr_t textureKey, PendingMain0573Payload &pending) {
    pending = {};
    if (!textureKey)
        return false;
    AcquireSRWLockShared(&s_main0573PayloadLock);
    auto it = s_pendingMain0573Payloads.find(textureKey);
    if (it != s_pendingMain0573Payloads.end())
        pending = it->second;
    ReleaseSRWLockShared(&s_main0573PayloadLock);
    return pending.payload != nullptr;
}

static void RememberEarlyReleasedPayload(const PendingMain0573Payload &pending) {
    if (!pending.payload)
        return;

    EarlyReleasedPayload released = pending;

    AcquireSRWLockExclusive(&s_main0573PayloadLock);
    auto pendingIt = s_pendingMain0573Payloads.find(pending.texture_key);
    if (pendingIt != s_pendingMain0573Payloads.end() &&
        pendingIt->second.payload == pending.payload) {
        s_pendingMain0573Payloads.erase(pendingIt);
    }
    s_earlyReleasedPayloads[reinterpret_cast<uintptr_t>(pending.payload)] = released;
    ReleaseSRWLockExclusive(&s_main0573PayloadLock);
}

static bool EnsureHelperTextureAvailable(const std::string &texturePath, bool inCache) {
    if (!inCache)
        return false;
    return !texturePath.empty() && ShouldQueueDecodeForPath(texturePath.c_str());
}

static void TryReleaseConsumedRetainedPayload(void *streamObj) {
    if (!streamObj || !TextureAllocFreePayload_o || !ReturnRetainedPayload_o)
        return;
    if (IsSwapStartupDeferred() || IsSwapWorldDeferred())
        return;

    auto *base = reinterpret_cast<uint8_t *>(streamObj);
    constexpr uint32_t STREAM_FLAG_TRANSFORMED = 0x01000000u;
    constexpr size_t OFF_OWNER = 0x130;
    constexpr size_t OFF_SIZE = 0x13C;
    constexpr size_t OFF_FLAGS = 0x140;
    constexpr size_t OFF_TRACKED_STATE = 0x18C;
    constexpr size_t OFF_TRACKED_MODE = 0x190;
    constexpr size_t OFF_TRACKED_BYTES = 0x194;
    constexpr size_t OFF_RETAINED = 0x198;
    constexpr uintptr_t ARG2_TEXTURE_TABLE = 0x00866650;
    constexpr uintptr_t ARG3_RETAINED_DIRECT_FREE = 0x446;

    uint32_t flags = *reinterpret_cast<uint32_t *>(base + OFF_FLAGS);
    // 0x6510A0 only reads from the retained +0x198 cache on the non-transformed path.
    if ((flags & STREAM_FLAG_TRANSFORMED) != 0)
        return;
    if (*reinterpret_cast<uintptr_t *>(base + OFF_TRACKED_STATE) == 0)
        return;
    uint32_t trackedMode = *reinterpret_cast<uint32_t *>(base + OFF_TRACKED_MODE);
    uint32_t trackedBytes = *reinterpret_cast<uint32_t *>(base + OFF_TRACKED_BYTES);
    uint32_t totalBytes = *reinterpret_cast<uint32_t *>(base + OFF_SIZE);
    if (trackedBytes != totalBytes)
        return;
    if (trackedMode != 4 && trackedMode != 2)
        return;

    void *payload = *reinterpret_cast<void **>(base + OFF_RETAINED);
    if (!payload)
        return;

    *reinterpret_cast<void **>(base + OFF_RETAINED) = nullptr;

    void *owner = *reinterpret_cast<void **>(base + OFF_OWNER);
    if (!owner) {
        TextureAllocFreePayload_o(reinterpret_cast<void *>(0x00C51C58), nullptr, payload,
                                  ARG2_TEXTURE_TABLE, ARG3_RETAINED_DIRECT_FREE);
        LogWrite("RETAINED_PAYLOAD_RELEASE: stream=%p payload=%p mode=direct state=%u bytes=%u",
                 streamObj, payload, trackedMode, trackedBytes);
        return;
    }

    ReturnRetainedPayload_o(owner, payload);
    LogWrite("RETAINED_PAYLOAD_RELEASE: stream=%p payload=%p mode=return state=%u bytes=%u",
             streamObj, payload, trackedMode, trackedBytes);
}

static void TryReleasePendingMain0573Payload(uintptr_t textureKey, uint64_t pathHash,
                                             const std::string &texturePath, bool helperReady) {
    if (!helperReady || !textureKey || pathHash == 0 || !TextureAllocFreePayload_o)
        return;

    PendingMain0573Payload pending{};
    if (!GetPendingMain0573Payload(textureKey, pending))
        return;
    if (pending.path_hash != 0 && pending.path_hash != pathHash)
        return;
    if (!pending.payload || !pending.allocator)
        return;

    TextureAllocFreePayload_o(pending.allocator, nullptr, pending.payload, pending.arg2, pending.arg3);
    RememberEarlyReleasedPayload(pending);
    LogWrite("TEXTURE_ALLOC_MAIN_0573_RELEASE: path='%s' texture=%p payload=%p raw=%p size=%u class=%u arg2=%p arg3=%p",
             texturePath.empty() ? "<unknown>" : texturePath.c_str(),
             reinterpret_cast<void *>(textureKey),
             pending.payload,
             pending.raw_block,
             pending.size,
             pending.size_class,
             reinterpret_cast<void *>(pending.arg2),
             reinterpret_cast<void *>(pending.arg3));
}

static uintptr_t __fastcall RetainedPayloadRead_h(void *thisptr, void *edx,
                                                  void *dst, uintptr_t size) {
    uintptr_t bytesRead = RetainedPayloadRead_o(thisptr, edx, dst, size);
    TryReleaseConsumedRetainedPayload(thisptr);
    return bytesRead;
}

static bool IsTextureQuarantined(uintptr_t textureKey, uint64_t pathHash) {
    AcquireSRWLockShared(&s_texMapLock);
    bool quarantined = s_swapQuarantineTex.count(textureKey) > 0 ||
                       s_swapQuarantinePaths.count(pathHash) > 0;
    ReleaseSRWLockShared(&s_texMapLock);
    return quarantined;
}

static void QuarantineTextureSwap(uintptr_t textureKey, uint64_t pathHash,
                                  const char *path, const char *reason) {
    AcquireSRWLockExclusive(&s_texMapLock);
    bool insertedTex = s_swapQuarantineTex.insert(textureKey).second;
    bool insertedPath = s_swapQuarantinePaths.insert(pathHash).second;
    ReleaseSRWLockExclusive(&s_texMapLock);

    if (insertedTex || insertedPath) {
        LogWrite("DEFAULT_SWAP_SKIP: quarantined '%s' reason=%s",
                 path ? path : "<null>", reason ? reason : "unknown");
    }
}

static void LogInlinePathMismatchOnce(uint64_t pathHash, const char *path,
                                      const char *inlinePath) {
    AcquireSRWLockExclusive(&s_texMapLock);
    bool inserted = s_swapInlineWarnedPaths.insert(pathHash).second;
    ReleaseSRWLockExclusive(&s_texMapLock);

    if (inserted) {
        LogWrite("DEFAULT_SWAP_NOTE: inline path mismatch stored='%s' inline='%s'",
                 path ? path : "<null>", inlinePath ? inlinePath : "<null>");
    }
}

static bool NormalizeComparablePath(std::string &path) {
    if (path.empty())
        return false;
    for (char &ch : path) {
        if (ch == '/')
            ch = '\\';
        else if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return IsTextureFile(path.c_str());
}

static bool ComparablePathMatchesStored(const std::string &inlinePath,
                                        const char *storedPath) {
    if (!storedPath || inlinePath.empty())
        return false;
    std::string stored(storedPath);
    if (!NormalizeComparablePath(stored))
        return false;
    if (_stricmp(inlinePath.c_str(), stored.c_str()) == 0)
        return true;
    if (inlinePath.size() > stored.size()) {
        const char *suffix = inlinePath.c_str() + (inlinePath.size() - stored.size());
        if (_stricmp(suffix, stored.c_str()) == 0)
            return true;
    }
    if (stored.size() > inlinePath.size()) {
        const char *suffix = stored.c_str() + (stored.size() - inlinePath.size());
        if (_stricmp(suffix, inlinePath.c_str()) == 0)
            return true;
    }
    return false;
}

static bool IsInlinePathChar(char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '_' || ch == '-' || ch == '.' || ch == '\\' || ch == '/';
}

static bool HasTextureExtensionAt(const std::string &raw, size_t pos) {
    if (pos + 4 > raw.size())
        return false;
    return _strnicmp(raw.c_str() + pos, ".blp", 4) == 0 ||
           _strnicmp(raw.c_str() + pos, ".tga", 4) == 0;
}

static bool ReadInlineTexturePath(Game::HTEXTURE__ *texture, const char *storedPath,
                                  std::string &path) {
    path.clear();
    if (!texture)
        return false;
    const char *inlinePath = reinterpret_cast<const char *>(texture) + 0x008;
    size_t len = 0;
    while (len < 260 && inlinePath[len] != '\0')
        ++len;
    if (len == 0 || len >= 260)
        return false;

    std::string raw(inlinePath, len);
    std::string firstValid;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (!HasTextureExtensionAt(raw, i))
            continue;
        size_t start = i;
        while (start > 0 && IsInlinePathChar(raw[start - 1]))
            --start;
        std::string candidate = raw.substr(start, (i + 4) - start);
        if (!NormalizeComparablePath(candidate))
            continue;
        if (storedPath && ComparablePathMatchesStored(candidate, storedPath)) {
            path.swap(candidate);
            return true;
        }
        if (firstValid.empty())
            firstValid.swap(candidate);
    }
    if (firstValid.empty())
        return false;
    path.swap(firstValid);
    return true;
}

// Returns true if the texture is already tracked in the DEFAULT pool with
// matching pathHash (i.e., already swapped and valid — no further work needed).
static bool TouchDefaultPoolEntry(uintptr_t textureKey, uint64_t pathHash) {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    auto it = s_defaultPoolMap.find(textureKey);
    if (it != s_defaultPoolMap.end()) {
        if (it->second->path_hash != pathHash) {
            auto entryIt = it->second;
            if (entryIt->size_bytes <= s_defaultPoolBytes)
                s_defaultPoolBytes -= entryIt->size_bytes;
            else
                s_defaultPoolBytes = 0;
            ReleaseTrackedTexture(*entryIt, false, "touch-path-mismatch");
            s_defaultPoolMap.erase(it);
            s_defaultPoolLru.erase(entryIt);
            ReleaseSRWLockExclusive(&s_defaultPoolLock);
            return false;
        }
        if (it->second->pending_eviction) {
            it->second->pending_eviction = false;
            LogWrite("DEFAULT_SWAP_TOUCH_CANCEL_EVICT: texture=%p pathHash=0x%llX",
                     reinterpret_cast<void *>(textureKey),
                     static_cast<unsigned long long>(pathHash));
        }
        LONG touchCount = InterlockedIncrement(&s_stat_default_touch_logs);
        if (touchCount <= 200 || (touchCount % 500) == 0) {
            LogWrite("DEFAULT_SWAP_TOUCH: texture=%p pathHash=0x%llX defaultTex=%p managedTex=%p size=%u",
                     reinterpret_cast<void *>(textureKey),
                     static_cast<unsigned long long>(pathHash),
                     it->second->default_tex,
                     it->second->managed_tex,
                     static_cast<unsigned>(it->second->size_bytes));
        }
        s_defaultPoolLru.splice(s_defaultPoolLru.begin(), s_defaultPoolLru, it->second);
        ReleaseSRWLockExclusive(&s_defaultPoolLock);
        return true;
    }
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
    return false;
}

static volatile bool s_hasPendingEvictions = false;

static void MarkEvictionsForBudget() {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    size_t projectedBytes = s_defaultPoolBytes;
    if (projectedBytes <= DEFAULT_POOL_BUDGET_BYTES) {
        ReleaseSRWLockExclusive(&s_defaultPoolLock);
        return;
    }

    // Single pass: collect eligible victims by residency class
    using Iter = std::list<DefaultPoolEntry>::iterator;
    static std::vector<Iter> lowClass, highClass;  // static: safe under exclusive lock
    lowClass.clear(); highClass.clear();
    for (auto it = s_defaultPoolLru.end(); it != s_defaultPoolLru.begin();) {
        --it;
        if (it->pending_eviction) continue;
        if (it->protect_passes > 0) { --it->protect_passes; continue; }
        if (it->residency_class >= 2)
            highClass.push_back(it);
        else
            lowClass.push_back(it);
    }

    // Mark low-residency victims first, then high-residency
    auto markFrom = [&](std::vector<Iter> &candidates) {
        for (auto it : candidates) {
            if (projectedBytes <= DEFAULT_POOL_BUDGET_BYTES) break;
            it->pending_eviction = true;
            s_hasPendingEvictions = true;
            LogWrite("DEFAULT_SWAP_EVICT_MARK: texture=%p pathHash=0x%llX defaultTex=%p managedTex=%p size=%u class=%u protect=%u",
                     reinterpret_cast<void *>(it->texture_key),
                     static_cast<unsigned long long>(it->path_hash),
                     it->default_tex,
                     it->managed_tex,
                     static_cast<unsigned>(it->size_bytes),
                     static_cast<unsigned>(it->residency_class),
                     static_cast<unsigned>(it->protect_passes));
            if (it->size_bytes <= projectedBytes)
                projectedBytes -= it->size_bytes;
            else
                projectedBytes = 0;
        }
    };
    markFrom(lowClass);
    markFrom(highClass);

    // Last resort: force-evict tail if still over budget
    if (projectedBytes > DEFAULT_POOL_BUDGET_BYTES && !s_defaultPoolLru.empty()) {
        auto it = std::prev(s_defaultPoolLru.end());
        if (!it->pending_eviction) {
            it->pending_eviction = true;
            s_hasPendingEvictions = true;
        }
    }

    ReleaseSRWLockExclusive(&s_defaultPoolLock);
}

static void ApplyPendingEvictions() {
    if (!s_hasPendingEvictions) return;

    // Two-phase eviction: restore bindings and remove from tracking under the
    // lock (fast), then perform COM Release calls and logging outside the lock
    // (potentially slow under Wine — each COM Release is a D3D vtable call).
    struct EvictedEntry {
        void     *default_tex;
        uintptr_t texture_key;
        uint64_t  path_hash;
        uint32_t  size_bytes;
        uint8_t   residency_class;
    };
    static std::vector<EvictedEntry> evicted; // static: safe, main thread only
    evicted.clear();

    AcquireSRWLockExclusive(&s_defaultPoolLock);
    for (auto it = s_defaultPoolLru.begin(); it != s_defaultPoolLru.end();) {
        if (!it->pending_eviction) {
            ++it;
            continue;
        }
        auto &entry = *it;

        // Restore managed texture binding in CGxTex (pointer write, no COM).
        bool restored = RestoreManagedTextureBinding(entry);
        if (!restored) {
            // Game may still be using the DEFAULT texture — retry next pass.
            entry.pending_eviction = false;
            ++it;
            continue;
        }

        if (entry.managed_ref_held)
            StoreManagedRefHoldLocked(entry, "budget-evict");

        evicted.push_back({entry.default_tex, entry.texture_key, entry.path_hash,
                           entry.size_bytes, entry.residency_class});

        entry.default_tex = nullptr;
        entry.managed_tex = nullptr;
        entry.managed_ref_held = false;
        entry.pending_eviction = false;

        if (entry.size_bytes <= s_defaultPoolBytes)
            s_defaultPoolBytes -= entry.size_bytes;
        else
            s_defaultPoolBytes = 0;
        s_defaultPoolMap.erase(entry.texture_key);
        it = s_defaultPoolLru.erase(it);
    }
    s_hasPendingEvictions = false;
    ReleaseSRWLockExclusive(&s_defaultPoolLock);

    // Phase 2: COM Release, logging, and stats outside the lock.
    for (auto &ev : evicted) {
        std::string path = GetTrackedTexturePath(ev.texture_key);
        LogWrite("DEFAULT_SWAP_EVICT: texture=%p pathHash=0x%llX defaultTex=%p size=%u class=%u path='%s'",
                 reinterpret_cast<void *>(ev.texture_key),
                 static_cast<unsigned long long>(ev.path_hash),
                 ev.default_tex,
                 static_cast<unsigned>(ev.size_bytes),
                 static_cast<unsigned>(ev.residency_class),
                 path.empty() ? "<unknown>" : path.c_str());
        ReleaseD3DTexture(ev.default_tex);
        InterlockedExchangeAdd64(&s_stat_default_evicted_bytes, ev.size_bytes);
        InterlockedIncrement(&s_stat_default_evictions);
    }
}

static void TrackDefaultPoolTexture(uintptr_t textureKey, uint64_t pathHash,
                                    void *defaultTex, void *managedTex,
                                    uint32_t sizeBytes,
                                    uint8_t residencyClass) {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    ReleaseManagedRefHoldLocked(textureKey, "swap-reacquire");
    auto it = s_defaultPoolMap.find(textureKey);
    if (it != s_defaultPoolMap.end()) {
        s_defaultPoolBytes -= it->second->size_bytes;
        ReleaseTrackedTexture(*it->second, false, "swap-overwrite");
        it->second->default_tex = defaultTex;
        it->second->managed_tex = managedTex;
        it->second->size_bytes = sizeBytes;
        it->second->path_hash = pathHash;
        it->second->residency_class = residencyClass;
        it->second->protect_passes = residencyClass >= 2 ? 2 : 1;
        it->second->pending_eviction = false;
        it->second->managed_ref_held = true;
        it->second->last_validated_tick = static_cast<LONG>(GetTickCount());
        s_defaultPoolBytes += sizeBytes;
        s_defaultPoolLru.splice(s_defaultPoolLru.begin(), s_defaultPoolLru, it->second);
        InterlockedIncrement(&s_stat_default_reuploads);
        LogWrite("DEFAULT_SWAP_TRACK: kind=reupload texture=%p pathHash=0x%llX defaultTex=%p managedTex=%p size=%u class=%u",
                 reinterpret_cast<void *>(textureKey),
                 static_cast<unsigned long long>(pathHash),
                 defaultTex,
                 managedTex,
                 static_cast<unsigned>(sizeBytes),
                 static_cast<unsigned>(residencyClass));
    } else {
        s_defaultPoolLru.push_front(DefaultPoolEntry{
            textureKey, pathHash, defaultTex, managedTex, sizeBytes, residencyClass,
            static_cast<uint8_t>(residencyClass >= 2 ? 2 : 1), false, true,
            static_cast<LONG>(GetTickCount())
        });
        s_defaultPoolMap[textureKey] = s_defaultPoolLru.begin();
        s_defaultPoolBytes += sizeBytes;
        LogWrite("DEFAULT_SWAP_TRACK: kind=new texture=%p pathHash=0x%llX defaultTex=%p managedTex=%p size=%u class=%u",
                 reinterpret_cast<void *>(textureKey),
                 static_cast<unsigned long long>(pathHash),
                 defaultTex,
                 managedTex,
                 static_cast<unsigned>(sizeBytes),
                 static_cast<unsigned>(residencyClass));
    }
    if (s_defaultPoolBytes > s_defaultPoolPeakBytes)
        s_defaultPoolPeakBytes = s_defaultPoolBytes;
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
    MarkEvictionsForBudget();
}

static void LogRuntimeMetricsIfNeeded() {
    LONG swaps = InterlockedCompareExchange(&s_stat_default_swaps, 0, 0);
    LONG lastLogged = InterlockedCompareExchange(&s_stat_last_runtime_metrics_swaps, 0, 0);
    if (swaps - lastLogged < RUNTIME_METRICS_SWAP_INTERVAL)
        return;
    if (InterlockedCompareExchange(&s_stat_last_runtime_metrics_swaps, swaps, lastLogged) != lastLogged)
        return;

    LogWrite("RUNTIME_METRICS: defaultPool current=%llu MB peak=%llu MB budget=%llu MB "
             "swaps=%ld evictions=%ld reuploads=%ld dedupSkips=%ld swapped=%llu MB evicted=%llu MB",
             static_cast<unsigned long long>(s_defaultPoolBytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(s_defaultPoolPeakBytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(DEFAULT_POOL_BUDGET_BYTES / (1024ULL * 1024ULL)),
             swaps, s_stat_default_evictions, s_stat_default_reuploads, s_stat_decode_dedup_skips,
             static_cast<unsigned long long>(s_stat_default_swapped_bytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(s_stat_default_evicted_bytes / (1024ULL * 1024ULL)));
}

static void CloseSharedMemory();

static bool OpenSharedMemory() {
    if (s_shmHeaderBase && s_shmHeaderMapping) {
        bool haveAllWindows = true;
        for (uint32_t window = 0; window < TBProto::SHM_WINDOW_COUNT; ++window) {
            if (!s_shmDataBases[window] || !s_shmDataMappings[window]) {
                haveAllWindows = false;
                break;
            }
        }
        if (haveAllWindows)
            return true;
        CloseSharedMemory();
    }

    s_shmHeaderMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, TBProto::SHM_NAME);
    if (!s_shmHeaderMapping) {
        LogWrite("OpenSharedMemory: OpenFileMappingA failed, err=%lu", GetLastError());
        return false;
    }

    s_shmHeaderBase = static_cast<uint8_t *>(
        MapViewOfFile(s_shmHeaderMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!s_shmHeaderBase) {
        LogWrite("OpenSharedMemory: MapViewOfFile failed, err=%lu", GetLastError());
        CloseHandle(s_shmHeaderMapping);
        s_shmHeaderMapping = nullptr;
        return false;
    }

    auto *hdr = reinterpret_cast<const TBProto::ShmHeader *>(s_shmHeaderBase);
    if (hdr->magic != TBProto::SHM_MAGIC || hdr->version != TBProto::SHM_VERSION) {
        LogWrite("OpenSharedMemory: bad magic=%08X version=%u",
                 hdr->magic, hdr->version);
        UnmapViewOfFile(s_shmHeaderBase);
        CloseHandle(s_shmHeaderMapping);
        s_shmHeaderBase = nullptr;
        s_shmHeaderMapping = nullptr;
        return false;
    }

    for (uint32_t window = 0; window < TBProto::SHM_WINDOW_COUNT; ++window) {
        char mappingName[128] = {};
        _snprintf_s(mappingName, _TRUNCATE, "%s%u", TBProto::SHM_DATA_NAME_PREFIX, window);
        s_shmDataMappings[window] = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
        if (!s_shmDataMappings[window]) {
            LogWrite("OpenSharedMemory: OpenFileMappingA data[%u] failed, err=%lu",
                     window, GetLastError());
            CloseSharedMemory();
            return false;
        }

        s_shmDataBases[window] = static_cast<uint8_t *>(
            MapViewOfFile(s_shmDataMappings[window], FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!s_shmDataBases[window]) {
            LogWrite("OpenSharedMemory: MapViewOfFile data[%u] failed, err=%lu",
                     window, GetLastError());
            CloseSharedMemory();
            return false;
        }
    }

    LogWrite("OpenSharedMemory: OK, server PID=%llu, slots=%u",
             static_cast<unsigned long long>(hdr->server_pid), hdr->slot_count);
    return true;
}

static void CloseSharedMemory() {
    for (uint32_t window = 0; window < TBProto::SHM_WINDOW_COUNT; ++window) {
        if (s_shmDataBases[window]) {
            UnmapViewOfFile(s_shmDataBases[window]);
            s_shmDataBases[window] = nullptr;
        }
        if (s_shmDataMappings[window]) {
            CloseHandle(s_shmDataMappings[window]);
            s_shmDataMappings[window] = nullptr;
        }
    }
    if (s_shmHeaderBase)    { UnmapViewOfFile(s_shmHeaderBase); s_shmHeaderBase = nullptr; }
    if (s_shmHeaderMapping) { CloseHandle(s_shmHeaderMapping);  s_shmHeaderMapping = nullptr; }
}

static const TBProto::SlotHeader *GetSlotHeader(int32_t slot) {
    if (!s_shmHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TBProto::SLOT_COUNT))
        return nullptr;
    const uint32_t window = static_cast<uint32_t>(slot) / TBProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TBProto::SLOTS_PER_WINDOW;
    return reinterpret_cast<const TBProto::SlotHeader *>(
        s_shmDataBases[window] +
        static_cast<uint64_t>(slotInWindow) * TBProto::SLOT_TOTAL);
}

static const uint8_t *GetSlotData(int32_t slot) {
    if (!s_shmHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TBProto::SLOT_COUNT))
        return nullptr;
    const uint32_t window = static_cast<uint32_t>(slot) / TBProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TBProto::SLOTS_PER_WINDOW;
    return s_shmDataBases[window] +
           static_cast<uint64_t>(slotInWindow) * TBProto::SLOT_TOTAL +
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
    bool ok = EnsureServerRunning();
    if (!ok) {
        CloseSharedMemory();
        s_server_available = false;
    }
    LogWrite("ReconnectServer: %s", ok ? "OK" : "FAILED");
    return ok;
}

// ── Persistent pipe connection (P8) ─────────────────────────────────

static thread_local HANDLE s_threadPipe = INVALID_HANDLE_VALUE;

static HANDLE& GetThreadPipe() {
    return s_threadPipe;
}

static void CloseIfValid(HANDLE &h) {
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
}

static HANDLE AcquirePersistentPipe() {
    HANDLE &h = GetThreadPipe();
    if (h != INVALID_HANDLE_VALUE)
        return h;
    // Retry pipe open up to 3 times on ERROR_PIPE_BUSY.
    for (int attempt = 0; attempt < 3; ++attempt) {
        h = CreateFileA(TBProto::PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                        0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(TBProto::PIPE_NAME, 500);
            continue;
        }
        break;
    }
    return h;  // may be INVALID_HANDLE_VALUE
}

static void InvalidatePersistentPipe() {
    CloseIfValid(GetThreadPipe());
}

static void ClosePersistentPipes() {
    CloseIfValid(GetThreadPipe());  // main thread's thread-local pipe
    // Worker thread-local pipes are closed when each worker exits
}

static int32_t SendToServer(const char *path, const uint8_t *rawData,
                            uint32_t rawSize, uint8_t priority,
                            TBProto::Response *outResp) {
    HANDLE pipe = AcquirePersistentPipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND && ReconnectServer())
            pipe = AcquirePersistentPipe();
        if (pipe == INVALID_HANDLE_VALUE) {
            LogWrite("SendToServer: pipe open failed, err=%lu", err);
            return -1;
        }
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
        InvalidatePersistentPipe();
        return -1;
    }

    TBProto::Response resp{};
    ok = ReadPipeFull(pipe, &resp, sizeof(resp));

    if (!ok) {
        LogWrite("SendToServer: pipe read failed for '%s'", path);
        InvalidatePersistentPipe();
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

static void NormalizePath(char *path) {
    if (!path) return;
    for (char *p = path; *p; ++p) {
        if (*p == '/')
            *p = '\\';
        else if (*p >= 'A' && *p <= 'Z')
            *p = static_cast<char>(*p + ('a' - 'A'));
    }
}

static bool WasRecentDir(uint64_t dirHash) {
    if (dirHash == 0) return false;
    int recentCount = s_recentDirIdx < PREFETCH_HISTORY ? s_recentDirIdx : PREFETCH_HISTORY;
    for (int i = 0; i < recentCount; ++i) {
        if (s_recentDirs[i] == dirHash)
            return true;
    }
    return false;
}

static uint8_t ResidencyClassForPath(const char *path) {
    if (!path) return 0;
    if (ContainsI(path, "CHARACTER\\") || ContainsI(path, "CREATURE\\"))
        return 3;
    if (ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\CAPE\\") ||
        ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\SHOULDER\\") ||
        ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\WEAPON\\") ||
        ContainsI(path, "ITEM\\OBJECTCOMPONENTS\\SHIELD\\"))
        return 2;
    if (ContainsI(path, "WORLD\\") || ContainsI(path, "XTEXTURES\\"))
        return 1;
    return 0;
}

static bool IsSwapStartupDeferred() {
    if (s_swap_enable_tick == 0)
        return false;
    return static_cast<LONG>(GetTickCount() - s_swap_enable_tick) < 0;
}

static bool IsSwapWorldDeferred() {
    uint64_t activePlayer = Game::ClntObjMgrGetActivePlayer();
    if (activePlayer == 0) {
        s_swap_world_ready_tick = 0;
        s_swap_world_ready_logged = false;
        return true;
    }

    DWORD now = GetTickCount();
    if (s_swap_world_ready_tick == 0) {
        s_swap_world_ready_tick = now + SWAP_WORLD_GRACE_MS;
        if (!s_swap_world_ready_logged) {
            LogWrite("SWAP_WORLD_GATE: active player detected, deferring live texture mutation for %lu ms",
                     static_cast<unsigned long>(SWAP_WORLD_GRACE_MS));
            s_swap_world_ready_logged = true;
        }
        return true;
    }

    return static_cast<LONG>(now - s_swap_world_ready_tick) < 0;
}

// ══════════════════════════════════════════════════════════════════════
//  Async Decode Worker Thread
// ══════════════════════════════════════════════════════════════════════

static DWORD WINAPI DecodeWorkerProc(LPVOID /*param*/) {
    LogWrite("Worker thread %lu started", GetCurrentThreadId());
    while (s_workerRunning) {
        DecodeRequest req;

        {
            AcquireSRWLockExclusive(&s_queueLock);
            while (s_queue.empty() && s_workerRunning) {
                SleepConditionVariableSRW(&s_queueCV, &s_queueLock, INFINITE, 0);
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
            s_pendingDecodes.erase(req.path_hash);
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
            // Evict when full — prefer entries whose texture is no longer
            // tracked, since those won't trigger the sync fallback.
            // Inline scan under both locks avoids heap-allocating a snapshot.
            // (Lock nesting s_cacheLock → s_texMapLock is the documented safe order.)
            static constexpr size_t CACHE_MAX_ENTRIES = 4096;
            static constexpr int EVICTION_SCAN_LIMIT = 8;
            if (s_cache.size() > CACHE_MAX_ENTRIES) {
                bool evicted = false;
                AcquireSRWLockShared(&s_texMapLock);
                int checked = 0;
                for (auto it = s_cache.begin(); it != s_cache.end() && checked < EVICTION_SCAN_LIMIT; ++it, ++checked) {
                    bool tracked = false;
                    for (const auto &tm : s_texMap) {
                        if (tm.second == it->first) { tracked = true; break; }
                    }
                    if (!tracked) {
                        s_cache.erase(it);
                        evicted = true;
                        break;
                    }
                }
                ReleaseSRWLockShared(&s_texMapLock);
                if (!evicted)
                    s_cache.erase(s_cache.begin());
            }
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
    CloseIfValid(s_threadPipe); // Clean up this thread's persistent pipe
    LogWrite("Worker thread %lu exiting", GetCurrentThreadId());
    return 0;
}

static unsigned __stdcall DecodeWorkerEntry(void *) {
    return DecodeWorkerProc(nullptr);
}

static void StartWorker() {
    if (s_workerThreads[0]) return;
    s_workerRunning = true;
    for (int i = 0; i < WORKER_COUNT; ++i) {
        s_workerThreads[i] = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, DecodeWorkerEntry, nullptr, 0, nullptr));
    }
    LogWrite("StartWorker: %d worker threads launched", WORKER_COUNT);
}

static void StopWorker() {
    if (!s_workerThreads[0]) return;
    s_workerRunning = false;
    WakeAllConditionVariable(&s_queueCV);
    // Cancel any pending synchronous pipe I/O so workers blocked in
    // ReadFile/WriteFile can observe s_workerRunning==false and exit.
    for (int i = 0; i < WORKER_COUNT; ++i) {
        if (s_workerThreads[i])
            CancelSynchronousIo(s_workerThreads[i]);
    }
    for (int i = 0; i < WORKER_COUNT; ++i) {
        if (s_workerThreads[i]) {
            WaitForSingleObject(s_workerThreads[i], 3000);
            CloseHandle(s_workerThreads[i]);
            s_workerThreads[i] = nullptr;
        }
    }
    ClosePersistentPipes();

    AcquireSRWLockExclusive(&s_queueLock);
    while (!s_queue.empty()) {
        s_pool.Release(s_queue.front().buf_idx);
        s_pendingDecodes.erase(s_queue.front().path_hash);
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
        queued < 0 ||
        static_cast<uint32_t>(queued) + rawSize >
            TBProto::MAX_QUEUE_MB * 1024u * 1024u)
    {
        InterlockedIncrement(&s_stat_bp_rejects);
        LogWrite("QueueDecode: back-pressure reject for '%s'", path);
        return false;
    }

    DecodeRequest req{};
    strncpy_s(req.path, sizeof(req.path), path, _TRUNCATE);
    NormalizePath(req.path);
    req.buf_idx   = bufIdx;
    req.raw_size  = rawSize;
    req.priority  = priority;
    req.path_hash = TBProto::HashPath(path);
    uint64_t dirHash = DirHash(path);
    if (WasRecentDir(dirHash) && req.priority > 32)
        req.priority = 32;

    AcquireSRWLockExclusive(&s_queueLock);
    if (s_pendingDecodes.find(req.path_hash) != s_pendingDecodes.end()) {
        ReleaseSRWLockExclusive(&s_queueLock);
        InterlockedIncrement(&s_stat_decode_dedup_skips);
        return false;
    }
    s_pendingDecodes.insert(req.path_hash);
    InterlockedExchangeAdd(&s_queuedBytes, static_cast<LONG>(rawSize));
    auto insertIt = s_queue.end();
    for (auto it = s_queue.begin(); it != s_queue.end(); ++it) {
        if (req.priority < it->priority) {
            insertIt = it;
            break;
        }
    }
    s_queue.insert(insertIt, req);
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

static bool RestoreManagedTextureBinding(const DefaultPoolEntry &entry) {
    if (!entry.texture_key || !entry.default_tex || !entry.managed_tex)
        return false;
    auto *texture = reinterpret_cast<Game::HTEXTURE__ *>(entry.texture_key);
    const uint32_t *htexWords = reinterpret_cast<const uint32_t *>(texture);
    auto *gxTex = reinterpret_cast<Game::CGxTex *>(htexWords[OFF_HTEX_GXTEX / 4]);
    if (!gxTex)
        return false;
    uint32_t *gxWords = reinterpret_cast<uint32_t *>(gxTex);
    if (reinterpret_cast<void *>(gxWords[OFF_GX_D3DTEX / 4]) != entry.default_tex)
        return false;
    gxWords[OFF_GX_D3DTEX / 4] = reinterpret_cast<uint32_t>(entry.managed_tex);
    return true;
}

// Full COM validation interval in milliseconds.
// Between validations, trust the fast pointer comparison.
static constexpr DWORD STALE_VALIDATE_INTERVAL_MS = 900;

static bool IsTrackedBindingStale(Game::HTEXTURE__ *texture, Game::CGxTex *gxTex,
                                  uint64_t pathHash, DWORD now) {
    if (!texture || !gxTex)
        return true;
    const uint32_t *htexWords = reinterpret_cast<const uint32_t *>(texture);
    if (htexWords[OFF_HTEX_GXTEX / 4] != reinterpret_cast<uint32_t>(gxTex))
        return true;

    AcquireSRWLockShared(&s_defaultPoolLock);
    auto it = s_defaultPoolMap.find(reinterpret_cast<uintptr_t>(texture));
    bool stale = false;
    if (it != s_defaultPoolMap.end()) {
        const DefaultPoolEntry &entry = *it->second;
        if (entry.path_hash != pathHash) {
            stale = true;
        } else {
            const uint32_t *gxWords = reinterpret_cast<const uint32_t *>(gxTex);
            void *liveTex = reinterpret_cast<void *>(gxWords[OFF_GX_D3DTEX / 4]);
            // Fast path: pointer still matches what we installed → not stale.
            if (liveTex != entry.default_tex && liveTex != entry.managed_tex) {
                stale = true;
            } else {
                // Full COM validation every ~1 second to catch rare D3D invalidation.
                if (static_cast<LONG>(now - static_cast<DWORD>(entry.last_validated_tick)) >=
                    static_cast<LONG>(STALE_VALIDATE_INTERVAL_MS)) {
                    stale = !IsLiveTextureBindingUsable(liveTex, entry.default_tex, entry.managed_tex);
                    if (!stale) {
                        InterlockedExchange(&entry.last_validated_tick, static_cast<LONG>(now));
                    }
                }
            }
        }
    }
    ReleaseSRWLockShared(&s_defaultPoolLock);
    return stale;
}

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

static bool DescribeD3DTexture(void *tex, D3DSurfDesc *outDesc, const char *reason) {
    if (!tex)
        return false;

    const char *tag = reason ? reason : "unknown";
    auto *vtable = ValidateD3DObject(tex, (VTIDX_GETLEVELDESC + 1) * sizeof(uint32_t), tag);
    if (!vtable) return false;
    if (!vtable[VTIDX_GETTYPE] || !vtable[VTIDX_GETLEVELDESC]) {
        LogWrite("D3D9_TEXTURE_INVALID: reason=%s tex=%p vtable=%p null-entry", tag, tex, vtable);
        return false;
    }

    auto fnGetType = reinterpret_cast<GetType_fn>(vtable[VTIDX_GETTYPE]);
    auto fnGetLevelDesc = reinterpret_cast<GetLevelDesc_fn>(vtable[VTIDX_GETLEVELDESC]);
    uint32_t resourceType = fnGetType(tex);
    D3DSurfDesc desc{};
    HRESULT hr = fnGetLevelDesc(tex, 0, &desc);
    if (resourceType != D3DRTYPE_TEXTURE || FAILED(hr) || desc.Width == 0 || desc.Height == 0) {
        LogWrite("D3D9_TEXTURE_INVALID: reason=%s tex=%p type=%u hr=0x%08lX dims=%ux%u pool=%u",
                 tag,
                 tex,
                 static_cast<unsigned>(resourceType),
                 static_cast<unsigned long>(hr),
                 static_cast<unsigned>(desc.Width),
                 static_cast<unsigned>(desc.Height),
                 static_cast<unsigned>(desc.Pool));
        return false;
    }

    if (outDesc)
        *outDesc = desc;
    return true;
}

static bool IsLiveTextureBindingUsable(void *liveTex, void *expectedDefaultTex,
                                       void *expectedManagedTex) {
    if (liveTex != expectedDefaultTex && liveTex != expectedManagedTex)
        return false;

    // Validate pointer before calling COM methods — DEFAULT pool textures
    // become invalid after D3D device reset (e.g., window maximize).
    if (IsBadReadPtr(liveTex, sizeof(void *)))
        return false;
    auto *vtable = *reinterpret_cast<uint32_t **>(liveTex);
    if (!vtable || IsBadReadPtr(vtable, (VTIDX_GETLEVELDESC + 1) * sizeof(uint32_t)))
        return false;

    D3DSurfDesc desc{};
    if (!DescribeD3DTexture(liveTex, &desc,
                            liveTex == expectedDefaultTex
                                ? "stale-live-default"
                                : "stale-live-managed")) {
        return false;
    }

    if (liveTex == expectedDefaultTex)
        return desc.Pool == D3DPOOL_DEFAULT;
    return desc.Pool == D3DPOOL_MANAGED;
}

static const char *PoolName(uint32_t pool) {
    switch (pool) {
    case 0: return "DEFAULT";
    case 1: return "MANAGED";
    case 2: return "SYSTEMMEM";
    case 3: return "SCRATCH";
    default: return "UNKNOWN";
    }
}

static void FlushAllDefaultPoolTextures(const char *reason) {
    AcquireSRWLockExclusive(&s_defaultPoolLock);
    size_t count = s_defaultPoolLru.size();
    for (auto &entry : s_defaultPoolLru) {
        // Restore managed texture binding in CGxTex
        if (entry.texture_key && entry.managed_tex) {
            auto *texture = reinterpret_cast<Game::HTEXTURE__ *>(entry.texture_key);
            const uint32_t *htexWords = reinterpret_cast<const uint32_t *>(texture);
            auto *gxTex = reinterpret_cast<Game::CGxTex *>(htexWords[OFF_HTEX_GXTEX / 4]);
            if (gxTex) {
                uint32_t *gxWords = reinterpret_cast<uint32_t *>(gxTex);
                gxWords[OFF_GX_D3DTEX / 4] = reinterpret_cast<uint32_t>(entry.managed_tex);
            }
        }
        // Release the DEFAULT texture (still alive — this runs BEFORE Reset)
        if (entry.default_tex)
            ReleaseD3DTexture(entry.default_tex);
        // Release our AddRef on the managed texture
        if (entry.managed_ref_held && entry.managed_tex)
            ReleaseD3DTexture(entry.managed_tex);
        entry.default_tex = nullptr;
        entry.managed_tex = nullptr;
        entry.managed_ref_held = false;
    }
    s_defaultPoolMap.clear();
    s_defaultPoolLru.clear();
    s_defaultPoolBytes = 0;
    for (auto &held : s_restoredManagedRefs) {
        ReleaseD3DTexture(held.second);
    }
    s_restoredManagedRefs.clear();
    ReleaseSRWLockExclusive(&s_defaultPoolLock);
    LogWrite("DEVICE_RESET_FLUSH: reason=%s flushed=%zu DEFAULT pool textures",
             reason ? reason : "unknown", count);
}

static bool TrySwapToDefaultPool(Game::HTEXTURE__ *texture, Game::CGxTex *gxTex,
                                 uint64_t pathHash, const char *path) {
    if (!texture || !gxTex || !path)
        return false;
    if (InterlockedCompareExchange(&s_uploadsThisFrame, 0, 0) >= MAX_UPLOADS_PER_FRAME)
        return false;
    const uintptr_t textureKey = reinterpret_cast<uintptr_t>(texture);
    if (IsTextureQuarantined(textureKey, pathHash))
        return false;

    const uint32_t *htexWords = reinterpret_cast<const uint32_t *>(texture);
    const uint32_t width = htexWords[OFF_HTEX_WIDTH / 4];
    const uint32_t height = htexWords[OFF_HTEX_HEIGHT / 4];
    if (!MeetsSwapSizeThreshold(width, height))
        return false;
    const uint32_t htexGxPtr = htexWords[OFF_HTEX_GXTEX / 4];
    if (htexGxPtr != reinterpret_cast<uint32_t>(gxTex)) {
        QuarantineTextureSwap(textureKey, pathHash, path, "gx-backptr-mismatch");
        return false;
    }

    std::string inlinePath;
    if (path && *path && ReadInlineTexturePath(texture, path, inlinePath) &&
        !ComparablePathMatchesStored(inlinePath, path)) {
        LogInlinePathMismatchOnce(pathHash, path, inlinePath.c_str());
        QuarantineTextureSwap(textureKey, pathHash, path, "inline-path-mismatch");
        return false;
    }

    const uint32_t *gxWords = reinterpret_cast<const uint32_t *>(gxTex);
    void *managedTex = reinterpret_cast<void *>(gxWords[OFF_GX_D3DTEX / 4]);
    if (!managedTex)
        return false;

    D3DSurfDesc currentDesc{};
    if (!DescribeD3DTexture(managedTex, &currentDesc, "swap-live-check"))
        return false;
    auto texVtable = *reinterpret_cast<uint32_t **>(managedTex);
    if (currentDesc.Pool == D3DPOOL_DEFAULT) {
        TouchDefaultPoolEntry(textureKey, pathHash);
        return true;
    }

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
    if (cached.width != width || cached.height != height) {
        QuarantineTextureSwap(textureKey, pathHash, path, "cache-dimension-mismatch");
        return false;
    }

    DecodedInfo info{};
    if (!GetDecodedTexture(path, nullptr, 0, info)) {
        LogWrite("DEFAULT_SWAP_FAIL: cache fetch miss '%s'", path);
        return false;
    }

    if (!ShouldSwapFormatForPath(path, info.format, width, height) ||
        info.width != width || info.height != height) {
        QuarantineTextureSwap(textureKey, pathHash, path, "payload-mismatch");
        ReleaseSlot(info.slot);
        return false;
    }

    auto fnGetDevice = reinterpret_cast<D3DTextureGetDevice_fn>(texVtable[3]);
    void *device = nullptr;
    HRESULT getDeviceHr = fnGetDevice(managedTex, &device);
    if (getDeviceHr == D3D_OK && device)
        s_cachedD3DDevice = device; // Cache for TestCooperativeLevel
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

    AddRefD3DTexture(managedTex);
    uint32_t *mutableGxWords = const_cast<uint32_t *>(gxWords);
    mutableGxWords[OFF_GX_D3DTEX / 4] = reinterpret_cast<uint32_t>(defaultTex);
    ReleaseSlot(info.slot);

    uint32_t trackedBytes = ComputeTextureBytes(info.width, info.height, info.format,
                                                static_cast<uint8_t>(levels));
    uint8_t residencyClass = ResidencyClassForPath(path);
    LogWrite("DEFAULT_SWAP_INSTALL: texture=%p gxTex=%p path='%s' managedTex=%p defaultTex=%p size=%u class=%u dims=%ux%u mips=%u",
             reinterpret_cast<void *>(textureKey),
             gxTex,
             path,
             managedTex,
             defaultTex,
             static_cast<unsigned>(trackedBytes),
             static_cast<unsigned>(residencyClass),
             static_cast<unsigned>(info.width),
             static_cast<unsigned>(info.height),
             static_cast<unsigned>(levels));
    TrackDefaultPoolTexture(textureKey, pathHash, defaultTex, managedTex,
                            trackedBytes, residencyClass);
    InterlockedIncrement(&s_stat_default_swaps);
    InterlockedIncrement(&s_uploadsThisFrame);
    InterlockedExchangeAdd64(&s_stat_default_swapped_bytes, trackedBytes);
    LogRuntimeMetricsIfNeeded();
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

    {
        if (IsBadReadPtr(tex, OFF_HTEX_ASYNC_REQ + 4)) {
            LogWrite("PROBE: bad-ptr tex=%p", tex);
            return;
        }
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
}

static Game::CGxTex * __fastcall TextureGetGxTex_h(
    Game::HTEXTURE__ *texture,
    int edx,                    // unused (fastcall padding for thiscall)
    Game::CStatus *status)
{
    InterlockedIncrement(&s_stat_gxtex_calls);

    // Per-frame work — runs once per GetTickCount tick (~15ms) since
    // OnFrameTick has no external caller.  One GetTickCount per call,
    // cached as tickNow for reuse in IsTrackedBindingStale below.
    static DWORD s_lastTick = 0;
    const DWORD tickNow = GetTickCount();
    if (tickNow != s_lastTick) {
        s_lastTick = tickNow;
        OnFrameTick();
    }

    uintptr_t textureKey = texture ? reinterpret_cast<uintptr_t>(texture) : 0;

    // Single lock scope: read pathHash, path pointer, and quarantine status
    // together to avoid multiple futex round-trips under Wine.
    // trackedPathPtr points into s_texPathMap — valid after lock release because
    // s_texPathMap is only modified on the main thread (TextureCreate_h, Shutdown).
    uint64_t pathHash = 0;
    const std::string *trackedPathPtr = nullptr;
    bool quarantined = false;
    if (textureKey) {
        AcquireSRWLockShared(&s_texMapLock);
        auto hashIt = s_texMap.find(textureKey);
        if (hashIt != s_texMap.end()) {
            pathHash = hashIt->second;
            auto pathIt = s_texPathMap.find(textureKey);
            if (pathIt != s_texPathMap.end())
                trackedPathPtr = &pathIt->second;
            quarantined = s_swapQuarantineTex.count(textureKey) > 0 ||
                          s_swapQuarantinePaths.count(pathHash) > 0;
        }
        ReleaseSRWLockShared(&s_texMapLock);
    }

    // Untracked textures: call original and return immediately (majority of calls)
    if (pathHash == 0) {
        return TextureGetGxTex_o(texture, edx, status);
    }

    // Hook D3D9 Device::Reset on first tracked-texture call (device exists by now)
    if (!s_deviceResetHooked && s_cachedD3DDevice)
        HookDeviceReset(s_cachedD3DDevice);

    // Call original — this triggers decode + D3D upload if not already done.
    s_activeTextureAllocKey = textureKey;
    s_activeTextureAllocPathHash = pathHash;
    s_activeTextureAllocPath = (trackedPathPtr && !trackedPathPtr->empty())
                                   ? trackedPathPtr->c_str() : nullptr;
    Game::CGxTex *gxTex = TextureGetGxTex_o(texture, edx, status);
    s_activeTextureAllocPath = nullptr;
    s_activeTextureAllocKey = 0;
    s_activeTextureAllocPathHash = 0;

    // Skip if Phase 2 not active, texture invalid, or not loaded yet.
    if (!s_initialized || !s_server_available || !texture || !gxTex)
        return gxTex;

    if (quarantined)
        return gxTex;

    if (IsSwapStartupDeferred() || IsSwapWorldDeferred())
        return gxTex;

    static const std::string s_emptyStr;
    const std::string &trackedPath = trackedPathPtr ? *trackedPathPtr : s_emptyStr;

    bool inCache = false;
    {
        AcquireSRWLockShared(&s_cacheLock);
        auto it = s_cache.find(pathHash);
        inCache = (it != s_cache.end() && it->second.valid);
        ReleaseSRWLockShared(&s_cacheLock);
    }
    TryReleasePendingMain0573Payload(textureKey, pathHash, trackedPath,
                                     EnsureHelperTextureAvailable(trackedPath, inCache));

    if (IsTrackedBindingStale(texture, gxTex, pathHash, tickNow)) {
        LogWrite("DEFAULT_SWAP_STALE_TRACK: texture=%p gxTex=%p pathHash=0x%llX",
                 reinterpret_cast<void *>(textureKey),
                 gxTex,
                 static_cast<unsigned long long>(pathHash));
        ClearDefaultPoolTracking(textureKey, true, "stale-track");
        return gxTex;
    }

    // Touch the LRU entry and check if already in DEFAULT pool.
    // If already swapped, skip TrySwapToDefaultPool (avoids redundant
    // quarantine check, DescribeD3DTexture COM calls, and double-touch).
    if (TouchDefaultPoolEntry(textureKey, pathHash))
        return gxTex;

    if (!trackedPath.empty() && inCache) {
        const uint32_t *gxWords = reinterpret_cast<const uint32_t *>(gxTex);
        void *managedTex = reinterpret_cast<void *>(gxWords[OFF_GX_D3DTEX / 4]);
        if (managedTex) {
            TrySwapToDefaultPool(texture, gxTex, pathHash, trackedPath.c_str());
        } else {
            AcquireSRWLockExclusive(&s_probedSetLock);
            s_probedSet.erase(textureKey);
            ReleaseSRWLockExclusive(&s_probedSetLock);
            return gxTex;
        }
    }

    // Only probe each HTEXTURE once.
    bool alreadyProbed = false;
    {
        AcquireSRWLockShared(&s_probedSetLock);
        alreadyProbed = s_probedSet.count(textureKey) > 0;
        ReleaseSRWLockShared(&s_probedSetLock);
    }
    if (alreadyProbed)
        return gxTex;

    // Mark as probed.
    {
        AcquireSRWLockExclusive(&s_probedSetLock);
        s_probedSet.insert(textureKey);
        ReleaseSRWLockExclusive(&s_probedSetLock);
    }

    // Probe if still within limit and texture is in our decode cache.
    if (s_probedCount < MAX_STRUCT_SCAN && inCache) {
        ProbeTextureStruct(texture, pathHash, gxTex);
    }

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
    const bool shouldTrackCpu = s_initialized && s_server_available && IsTextureFile(filename) &&
                                ShouldQueueDecodeForPath(filename);

    uint64_t pathHash = 0;

    // Quick checks before any work.
    if (shouldTrackCpu) {

        InterlockedIncrement(&s_stat_intercepted);

        // Log first 50 intercepts, then every 100th.
        LONG count = s_stat_intercepted;
        if (count <= 50 || (count % 100) == 0)
            LogWrite("TextureCreate_h: #%ld '%s'", count, filename);

        // Already cached?
        pathHash = TBProto::HashPath(filename);
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
            // Defer the synchronous MPQ read to OnFrameTick so zone transitions
            // don't block the main thread with our redundant file I/O.
            EnqueueDeferredRead(filename, pathHash);
        }
    }

    // ALWAYS call original — game must create its HTEXTURE regardless.
    Game::HTEXTURE__ *htex = TextureCreate_o(filename, status, texFlags, unkParam1, unkParam2);

    // Phase 2: record HTEXTURE* → pathHash for struct probing and lifecycle tracking.
    if (htex && shouldTrackCpu) {
        uint64_t ph = pathHash;
        uintptr_t textureKey = reinterpret_cast<uintptr_t>(htex);
        bool pathChanged = false;
        AcquireSRWLockShared(&s_texMapLock);
        auto existing = s_texMap.find(textureKey);
        pathChanged = (existing != s_texMap.end() && existing->second != ph);
        ReleaseSRWLockShared(&s_texMapLock);
        if (pathChanged)
            ClearTrackedTextureState(textureKey);

        AcquireSRWLockExclusive(&s_texMapLock);
        s_texMap[textureKey] = ph;
        s_texPathMap[textureKey] = filename;
        ReleaseSRWLockExclusive(&s_texMapLock);
    }

    return htex;
}

static void OnTextureDestroy(Game::HTEXTURE__ *texture) {
    if (!texture)
        return;

    uintptr_t textureKey = reinterpret_cast<uintptr_t>(texture);
    bool tracked = false;
    std::string path;

    AcquireSRWLockShared(&s_texMapLock);
    auto it = s_texMap.find(textureKey);
    if (it != s_texMap.end()) {
        tracked = true;
        auto pathIt = s_texPathMap.find(textureKey);
        if (pathIt != s_texPathMap.end())
            path = pathIt->second;
    }
    ReleaseSRWLockShared(&s_texMapLock);

    if (!tracked)
        return;

    LogWrite("TEXTURE_DESTROY: texture=%p path='%s'",
             texture,
             path.empty() ? "<unknown>" : path.c_str());
    ClearTrackedTextureState(textureKey);
}

static int __fastcall GxTexOwnerUpdate_h(void *pThis, int /*edx*/, void *entry,
                                         void *bounds, void *notify) {
    if (!pThis)
        return 0;

    if (notify) {
        void **vtable = *reinterpret_cast<void ***>(pThis);
        void *slot0 = vtable ? vtable[0] : nullptr;
        if (!slot0) {
            LogWrite("GXTEX_OWNER_UPDATE_SKIP_NULL_VCALL: this=%p entry=%p bounds=%p notify=%p",
                     pThis, entry, bounds, notify);
            notify = nullptr;
        }
    }

    return GxTexOwnerUpdate_o(pThis, entry, bounds, notify);
}

static void __fastcall TextureAllocFreePayload_h(void *thisptr, void * /*edx*/, void *payload,
                                                 uintptr_t arg2, uintptr_t arg3) {
    if (payload) {
        EarlyReleasedPayload released{};
        bool skip = false;
        AcquireSRWLockExclusive(&s_main0573PayloadLock);
        auto it = s_earlyReleasedPayloads.find(reinterpret_cast<uintptr_t>(payload));
        if (it != s_earlyReleasedPayloads.end() &&
            it->second.allocator == thisptr &&
            it->second.arg2 == arg2 &&
            it->second.arg3 == arg3) {
            released = it->second;
            s_earlyReleasedPayloads.erase(it);
            skip = true;
        }
        ReleaseSRWLockExclusive(&s_main0573PayloadLock);
        if (skip) {
            LogWrite("TEXTURE_ALLOC_MAIN_0573_DUPFREE_SKIP: texture=%p payload=%p raw=%p size=%u class=%u arg2=%p arg3=%p",
                     reinterpret_cast<void *>(released.texture_key),
                     released.payload,
                     released.raw_block,
                     released.size,
                     released.size_class,
                     reinterpret_cast<void *>(released.arg2),
                     reinterpret_cast<void *>(released.arg3));
            return;
        }
    }

    TextureAllocFreePayload_o(thisptr, nullptr, payload, arg2, arg3);
}

#ifdef _MSC_VER
static __declspec(naked) void TextureDestroy_h() {
    __asm {
        pushad
        pushfd
        push edi
        call OnTextureDestroy
        add esp, 4
        popfd
        popad
        jmp dword ptr [TextureDestroy_o]
    }
}
#else
__attribute__((naked)) static void TextureDestroy_h() {
    asm volatile(
        ".intel_syntax noprefix\n\t"
        "pushad\n\t"
        "pushfd\n\t"
        "push edi\n\t"
        "call %P0\n\t"
        "add esp, 4\n\t"
        "popfd\n\t"
        "popad\n\t"
        "jmp dword ptr [%1]\n\t"
        ".att_syntax\n\t"
        :: "i"(OnTextureDestroy),
           "m"(TextureDestroy_o)
    );
}
#endif

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

    bool exeFound = (GetFileAttributesA(exePath.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (!exeFound) {
        // Fallback: try parent directory (game root, one level up from mods/)
        std::string parentDir(s_dllDir);
        size_t lastSlash = parentDir.find_last_of("\\/", parentDir.size() - 2);
        if (lastSlash != std::string::npos) {
            parentDir = parentDir.substr(0, lastSlash + 1);
            std::string parentExe = parentDir + "TextureServer64.exe";
            LogWrite("EnsureServerRunning: trying parent dir '%s'", parentExe.c_str());
            if (GetFileAttributesA(parentExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                exePath = parentExe;
                exeFound = true;
            }
        }
    }
    if (!exeFound) {
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

    // Try event-based wait first (fast path for new server).
    bool connected = false;
    HANDLE hReady = nullptr;
    for (int retry = 0; retry < 10 && !hReady; ++retry) {
        hReady = OpenEventA(SYNCHRONIZE, FALSE, "VH_TexServer_ShmReady");
        if (!hReady) Sleep(50);
    }
    if (hReady) {
        if (WaitForSingleObject(hReady, 3000) == WAIT_OBJECT_0)
            connected = OpenSharedMemory();
        CloseHandle(hReady);
    }
    // Fallback: polling for older server versions.
    if (!connected) {
        for (int i = 0; i < 30 && !connected; i++) {
            Sleep(100);
            connected = OpenSharedMemory();
        }
    }
    if (connected) {
        s_server_available = true;
        StartWorker();
        LogWrite("EnsureServerRunning: SHM connected");
        return true;
    }
    LogWrite("EnsureServerRunning: TIMEOUT waiting for SHM");
    return false;
}

static bool s_hooksInstalled = false;

bool InstallHooks() {
    LogWrite("InstallHooks: initialized=%d server_available=%d hooksInstalled=%d",
             s_initialized, s_server_available, s_hooksInstalled);

    if (s_hooksInstalled)
        return true;

    if (!s_initialized || !s_server_available) {
        LogWrite("InstallHooks: skipping (no server)");
        return true;
    }

    // Hook TextureCreate — entry point for ALL texture loading.
    HOOK_FUNCTION(Offsets::FUN_TEXTURE_CREATE, TextureCreate_h, TextureCreate_o);
    LogWrite("InstallHooks: TextureCreate hooked OK, original=%p", TextureCreate_o);

    HOOK_FUNCTION(Offsets::FUN_TEXTURE_DESTROY, TextureDestroy_h, TextureDestroy_o);
    LogWrite("InstallHooks: TextureDestroy hooked OK, original=%p", TextureDestroy_o);

    HOOK_FUNCTION(Offsets::FUN_GX_TEX_OWNER_UPDATE, GxTexOwnerUpdate_h, GxTexOwnerUpdate_o);
    LogWrite("InstallHooks: GxTexOwnerUpdate hooked OK, original=%p", GxTexOwnerUpdate_o);

    HOOK_FUNCTION(Offsets::FUN_TEXTURE_ALLOC_MAIN, TextureAllocMain_h, TextureAllocMain_o);
    LogWrite("InstallHooks: TextureAllocMain hooked OK, original=%p", TextureAllocMain_o);

    HOOK_FUNCTION(Offsets::FUN_SMEM_FREE_PAYLOAD, TextureAllocFreePayload_h, TextureAllocFreePayload_o);
    LogWrite("InstallHooks: TextureAllocFreePayload hooked OK, original=%p", TextureAllocFreePayload_o);

    ReturnRetainedPayload_o =
        reinterpret_cast<ReturnRetainedPayload_t>(Offsets::FUN_RETURN_RETAINED_PAYLOAD);
    HOOK_FUNCTION(Offsets::FUN_RETAINED_PAYLOAD_READ, RetainedPayloadRead_h, RetainedPayloadRead_o);
    LogWrite("InstallHooks: RetainedPayloadRead hooked OK, original=%p", RetainedPayloadRead_o);

    // Phase 2: Hook TextureGetGxTex — fires after async decode + D3D upload.
    // Used to probe HTEXTURE struct layout and (once offsets are confirmed)
    // free CPU-side pixel buffers after the GPU copy is complete.
    HOOK_FUNCTION(Offsets::FUN_TEXTURE_GET_GX_TEX, TextureGetGxTex_h, TextureGetGxTex_o);
    LogWrite("InstallHooks: TextureGetGxTex hooked OK, original=%p", TextureGetGxTex_o);
    s_swap_enable_tick = GetTickCount() + SWAP_STARTUP_GRACE_MS;
    s_swap_world_ready_tick = 0;
    s_swap_world_ready_logged = false;
    LogWrite("InstallHooks: deferring live swap/probe work for %lu ms",
             static_cast<unsigned long>(SWAP_STARTUP_GRACE_MS));

    s_hooksInstalled = true;
    return true;
}

void Shutdown(bool terminateServer) {
    LogWrite("Shutdown: terminateServer=%d", terminateServer ? 1 : 0);
    if (terminateServer) {
        LogWrite("Shutdown: stopping worker...");
        StopWorker();  // also closes persistent pipes

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
    } else {
        LogWrite("Shutdown: preserving worker/server state for reload");
    }

    AcquireSRWLockExclusive(&s_defaultPoolLock);
    for (auto &entry : s_defaultPoolLru) {
        ReleaseTrackedTextureOnShutdown(entry);
    }
    s_defaultPoolMap.clear();
    s_defaultPoolLru.clear();
    for (auto &held : s_restoredManagedRefs) {
        LogWrite("DEFAULT_SWAP_MANAGED_HOLD_RELEASE: reason=shutdown texture=%p managedTex=%p",
                 reinterpret_cast<void *>(held.first),
                 held.second);
        ReleaseD3DTexture(held.second);
    }
    s_restoredManagedRefs.clear();
    s_defaultPoolBytes = 0;
    ReleaseSRWLockExclusive(&s_defaultPoolLock);

    // Phase 2: clear texture tracking maps.
    AcquireSRWLockExclusive(&s_texMapLock);
    s_texMap.clear();
    s_texPathMap.clear();
    s_swapQuarantineTex.clear();
    s_swapQuarantinePaths.clear();
    s_swapInlineWarnedPaths.clear();
    ReleaseSRWLockExclusive(&s_texMapLock);

    AcquireSRWLockExclusive(&s_probedSetLock);
    s_probedSet.clear();
    ReleaseSRWLockExclusive(&s_probedSetLock);

    AcquireSRWLockExclusive(&s_main0573PayloadLock);
    s_pendingMain0573Payloads.clear();
    s_earlyReleasedPayloads.clear();
    ReleaseSRWLockExclusive(&s_main0573PayloadLock);

    s_activeTextureAllocKey = 0;
    s_activeTextureAllocPathHash = 0;
    s_focusedMain0573 = {};

    s_swap_enable_tick = GetTickCount() + SWAP_STARTUP_GRACE_MS;
    s_swap_world_ready_tick = 0;
    s_swap_world_ready_logged = false;
    s_deferredReads.clear();
    s_deferredReadHashes.clear();

    if (terminateServer) {
        AcquireSRWLockExclusive(&s_cacheLock);
        s_cache.clear();
        ReleaseSRWLockExclusive(&s_cacheLock);

        CloseSharedMemory();
        s_pool.Destroy();
        s_server_available = false;
        s_initialized = false;
        s_swap_enable_tick = 0;
    } else {
        LogWrite("Shutdown: re-arming live swap/probe work for %lu ms after reload",
                 static_cast<unsigned long>(SWAP_STARTUP_GRACE_MS));
    }

    // Report Phase 2 struct discovery results.
    LogWrite("Shutdown: Phase2 probed=%ld gxtexCalls=%ld freed=%ld",
             s_stat_probed, s_stat_gxtex_calls,
             s_stat_freed_texbufs);
    LogWrite("Shutdown: D3D managed textures=%ld total_system_mem=%lld MB",
             static_cast<long>(s_total_d3d_managed_count),
             static_cast<long long>(s_total_d3d_managed_bytes / (1024 * 1024)));
    LogWrite("Shutdown: defaultPool current=%llu MB peak=%llu MB budget=%llu MB "
             "swapped=%llu MB evicted=%llu MB swaps=%ld evictions=%ld reuploads=%ld",
             static_cast<unsigned long long>(s_defaultPoolBytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(s_defaultPoolPeakBytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(DEFAULT_POOL_BUDGET_BYTES / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(s_stat_default_swapped_bytes / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(s_stat_default_evicted_bytes / (1024ULL * 1024ULL)),
             s_stat_default_swaps, s_stat_default_evictions, s_stat_default_reuploads);

    LogWrite("Shutdown: complete. Stats: intercepted=%ld queued=%ld done=%ld "
             "cacheHits=%ld syncFallback=%ld bpRejects=%ld poolMisses=%ld",
             s_stat_intercepted, s_stat_queued, s_stat_done,
             s_stat_cache_hits, s_stat_sync_fallback,
             s_stat_bp_rejects, s_stat_pool_misses);
    LogClose();
}

void OnFrameTick() {
    InterlockedExchange(&s_uploadsThisFrame, 0);
    ApplyPendingEvictions();
    MarkEvictionsForBudget();

    // Process deferred file reads — spreads MPQ I/O across frames instead of
    // bursting during zone transitions where it compounds WoW's own loading stall.
    // Time-budgeted (2ms) to prevent audio hitching when MAX_DEFERRED_READS > 1.
    if (!s_deferredReads.empty()) {
        static LONGLONG s_qpcBudgetTicks = 0;
        if (s_qpcBudgetTicks == 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            s_qpcBudgetTicks = freq.QuadPart / 500; // 2ms
        }
        LARGE_INTEGER qpcStart;
        QueryPerformanceCounter(&qpcStart);

        for (int i = 0; i < MAX_DEFERRED_READS_PER_FRAME && !s_deferredReads.empty(); ++i) {
            DeferredRead dr = s_deferredReads.front();
            s_deferredReads.pop_front();

            {
                AcquireSRWLockShared(&s_cacheLock);
                auto it = s_cache.find(dr.path_hash);
                bool cached = (it != s_cache.end() && it->second.valid);
                ReleaseSRWLockShared(&s_cacheLock);
                if (cached) { s_deferredReadHashes.erase(dr.path_hash); continue; }
            }

            LARGE_INTEGER qpcNow;
            QueryPerformanceCounter(&qpcNow);
            if (qpcNow.QuadPart - qpcStart.QuadPart >= s_qpcBudgetTicks) {
                s_deferredReads.push_front(dr);
                break;
            }

            int bufIdx = s_pool.Acquire();
            if (bufIdx < 0) { s_deferredReads.push_front(dr); break; }
            s_deferredReadHashes.erase(dr.path_hash);

            uint8_t *buf = s_pool.Get(bufIdx);
            uint32_t rawSize = ReadFileViaStorm(dr.path, buf, POOL_BUF_SIZE);
            if (rawSize == 0) {
                s_pool.Release(bufIdx);
                continue;
            }

            uint64_t dh = DirHash(dr.path);
            if (dh != 0) {
                s_recentDirs[s_recentDirIdx % PREFETCH_HISTORY] = dh;
                s_recentDirIdx++;
            }

            if (!QueueDecode(dr.path, bufIdx, rawSize, 128))
                s_pool.Release(bufIdx);
        }
    }

    // Flush log periodically rather than every frame — fflush is a synchronous
    // kernel write-through that costs 0.1-2ms per call under Wine.
    static int s_flushCounter = 0;
    if (s_logFile && ++s_flushCounter >= 60) {
        fflush(s_logFile);
        s_flushCounter = 0;
    }
}

static bool FillDecodedInfo(DecodedInfo &info, int32_t slot) {
    const TBProto::SlotHeader *sh = GetSlotHeader(slot);
    const uint8_t *pixels = GetSlotData(slot);
    if (!sh || !pixels || sh->state != TBProto::STATE_READY)
        return false;
    info.slot       = slot;
    info.width      = sh->width;
    info.height     = sh->height;
    info.data_size  = sh->data_size;
    info.format     = sh->format;
    info.mip_levels = sh->mip_levels;
    info.pixels     = pixels;
    return true;
}

bool GetDecodedTexture(const char *path, const void *rawData, uint32_t rawSize,
                       DecodedInfo &info) {
    if (!s_initialized || !s_server_available) return false;
    if (!IsTextureFile(path)) return false;

    uint64_t pathHash = TBProto::HashPath(path);
    const uint8_t *rawBytes = static_cast<const uint8_t *>(rawData);

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
            if (FillDecodedInfo(info, slot))
                return true;
            ReleaseSlot(slot);
        }

        if (resp.status == TBProto::STATUS_NOT_FOUND) {
            AcquireSRWLockExclusive(&s_cacheLock);
            auto it = s_cache.find(pathHash);
            if (it != s_cache.end()) {
                it->second.valid = false;
            }
            ReleaseSRWLockExclusive(&s_cacheLock);

            // Queue an async re-decode instead of blocking the main thread.
            // The texture stays in MANAGED pool this frame; the DEFAULT swap
            // will succeed on a future frame once the worker completes.
            EnqueueDeferredRead(path, pathHash);
            LogWrite("GetDecodedTexture: stale hint for '%s', queued async re-decode", path);
            InterlockedIncrement(&s_stat_sync_fallback);
            return false;
        }
        // Fall through to sync fallback only when caller provided raw data.
    }

    // 2. Synchronous fallback — only reached when the caller already has raw
    //    bytes (not from the main-thread TrySwapToDefaultPool path, which
    //    passes nullptr).  For the common case the async re-queue above
    //    avoids this entirely.
    if (!rawBytes || rawSize == 0) {
        InterlockedIncrement(&s_stat_sync_fallback);
        return false;
    }

    InterlockedIncrement(&s_stat_sync_fallback);

    TBProto::Response resp{};
    int32_t slot = SendToServer(path,
                                rawBytes,
                                rawSize, 128, &resp);
    if (slot < 0) return false;

    if (!FillDecodedInfo(info, slot)) { ReleaseSlot(slot); return false; }

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
    if (!s_shmHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TBProto::SLOT_COUNT))
        return;

    const uint32_t window = static_cast<uint32_t>(slot) / TBProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TBProto::SLOTS_PER_WINDOW;
    volatile LONG *state = reinterpret_cast<volatile LONG *>(
        s_shmDataBases[window] +
        static_cast<uint64_t>(slotInWindow) * TBProto::SLOT_TOTAL);

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

// ── Protocol drift guard ─────────────────────────────────────────────────
// TBProto (above) mirrors TexProto (Protocol.h). These static_asserts catch
// any divergence at compile time.
#include "../../TextureServer64/shared/Protocol.h"
static_assert(TBProto::SLOT_COUNT == TexProto::SLOT_COUNT, "SLOT_COUNT mismatch");
static_assert(TBProto::SLOT_DATA_SIZE == TexProto::SLOT_DATA_SIZE, "SLOT_DATA_SIZE mismatch");
static_assert(TBProto::SLOT_HEADER_SIZE == TexProto::SLOT_HEADER, "SLOT_HEADER size mismatch");
static_assert(TBProto::SLOT_TOTAL == TexProto::SLOT_TOTAL, "SLOT_TOTAL mismatch");
static_assert(TBProto::SHM_WINDOW_COUNT == TexProto::SHM_WINDOW_COUNT, "SHM_WINDOW_COUNT mismatch");
static_assert(TBProto::SLOTS_PER_WINDOW == TexProto::SLOTS_PER_WINDOW, "SLOTS_PER_WINDOW mismatch");
static_assert(TBProto::SHM_HEADER_SIZE == TexProto::SHM_HEADER, "SHM_HEADER size mismatch");
static_assert(TBProto::SHM_MAGIC == TexProto::SHM_MAGIC, "SHM_MAGIC mismatch");
static_assert(TBProto::SHM_VERSION == TexProto::SHM_VERSION, "SHM_VERSION mismatch");
static_assert(sizeof(TBProto::Request) == sizeof(TexProto::Request), "Request struct size mismatch");
static_assert(sizeof(TBProto::Response) == sizeof(TexProto::Response), "Response struct size mismatch");
static_assert(sizeof(TBProto::SlotHeader) == sizeof(TexProto::SlotHeader), "SlotHeader struct size mismatch");
static_assert(sizeof(TBProto::ShmHeader) == sizeof(TexProto::ShmHeader), "ShmHeader struct size mismatch");
