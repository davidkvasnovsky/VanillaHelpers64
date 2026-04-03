// This file is part of VanillaHelpers.
//
// TexBridge: Bridges VanillaHelpers to the 64-bit TextureServer64 process.
// Hooks SFile_Open for .blp/.tga textures, reads raw bytes via a second
// file handle, queues async decode on the 64-bit server, and serves
// decoded pixels back from shared memory.
//
// Pipeline stages:
//   SFile_Open hook → BufferPool read → AsyncDecodeQueue → Pipe to server
//   → SharedMemory slot → DecodeCache → GetDecodedTexture()
//
// If the server is unavailable, all hooks are transparent pass-throughs.

#pragma once

#include <cstdint>
#include <windows.h>

namespace TexBridge {

// ── Lifecycle (called from DllMain) ───────────────────────────────────

// Call from DLL_PROCESS_ATTACH after MH_Initialize.
// Stores the DLL handle for locating TextureServer64.exe.
bool Initialize(HMODULE hVanillaHelpers);

// Install SFile_Open hook via MinHook.
bool InstallHooks();

// Call from CGGameUI_Shutdown hook before the original with terminateServer=false
// so reloads preserve the helper process. Call with terminateServer=true on
// final process detach to stop the server and tear down shared memory.
void Shutdown(bool terminateServer = true);

// Called once per frame for LRU eviction of the 32-bit working set.
void OnFrameTick();

// Launch TextureServer64.exe if not already running.
// Blocks up to ~3 seconds waiting for shared memory to appear.
bool EnsureServerRunning();

// ── Decoded texture query ─────────────────────────────────────────────

struct DecodedInfo {
    int32_t  slot;       // shared memory slot index
    uint32_t width;
    uint32_t height;
    uint32_t data_size;
    uint8_t  format;     // PixelFormat enum from Protocol.h
    uint8_t  mip_levels;
    const uint8_t *pixels; // pointer into mapped shared memory
};

// Check if a decoded texture is available (from async pipeline or cache).
// If the path was pre-decoded by the SFile_Open hook, returns immediately.
// Otherwise sends a synchronous decode request as fallback.
// Caller must call ReleaseSlot() after copying pixels to D3D9.
bool GetDecodedTexture(const char *path, const void *rawData, uint32_t rawSize,
                       DecodedInfo &info);

// Release a shared memory slot after D3D9 upload.
void ReleaseSlot(int32_t slot);

// Focused logging for the dominant 0x645640 / 0x00866650 / 0x573 branch.
void LogFocusedMain0573Backend(void *allocator, uint32_t sizeClass, uint32_t size,
                               uint32_t commit, void *result);

// ── Pipeline stats (for diagnostics) ──────────────────────────────────

struct PipelineStats {
    // Phase 1 — async decode pipeline
    uint32_t textures_intercepted;  // TextureCreate hook hits
    uint32_t async_decodes_queued;  // requests pushed to decode queue
    uint32_t async_decodes_done;    // completed async decodes
    uint32_t cache_hits;            // GetDecodedTexture served from cache
    uint32_t sync_fallbacks;        // GetDecodedTexture did synchronous decode
    uint32_t back_pressure_rejects; // requests dropped due to back-pressure
    uint32_t buffer_pool_misses;    // no free buffer available

    // Phase 2 — HTEXTURE struct discovery & pixel buffer reclaim
    uint32_t gxtex_calls;           // TextureGetGxTex hook calls
    uint32_t struct_probes;         // HTEXTURE structs scanned
    int32_t  discovered_width_off;  // discovered HTEXTURE width field offset (-1=unknown)
    int32_t  discovered_height_off; // discovered HTEXTURE height field offset (-1=unknown)
    int32_t  discovered_pixbuf_off; // discovered pixel buffer pointer offset (-1=unknown)
    uint32_t freed_texbufs;         // pixel buffers freed after D3D upload
};
void GetStats(PipelineStats &out);

} // namespace TexBridge
