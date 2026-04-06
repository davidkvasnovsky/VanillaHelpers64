#pragma once
// TexClient.h - 32-bit client API for TextureServer64 shared memory IPC.
// Runs inside WoW.exe (via VanillaHelpers.dll).

#include "../../shared/Protocol.h"

#include <cstdint>
#include <windows.h>

namespace TexClient {

/// Open shared memory mapping and verify the SHM header (magic + version).
/// Returns true on success.
bool Initialize();

/// Unmap shared memory view and close all handles.
void Shutdown();

/// Send raw BLP/TGA file bytes to the 64-bit server for decoding.
/// @param path       Texture path used as cache key (e.g. "Textures\\Foo.blp").
/// @param raw_data   Pointer to raw file bytes (BLP/TGA).
/// @param raw_size   Size of raw_data in bytes.
/// @param priority   Decode priority (0 = highest, default 128).
/// @return           Slot index [0..SLOT_COUNT) on success, or -1 on failure.
int32_t RequestDecode(const char* path, const uint8_t* raw_data, uint32_t raw_size, uint8_t priority = 128);

/// Release a shared memory slot after the client has copied pixels to D3D9.
/// Sets the slot state to Empty so the server can reuse it.
void ReleaseSlot(int32_t slot);

/// Query the server to check if a texture path is already cached.
bool IsTextureCached(const char* path);

/// Read decoded pixel data from a shared-memory slot.
struct SlotView {
    const uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t data_size;
    TexProto::PixelFormat format;
};
bool ReadSlot(int32_t slot, SlotView& view);

/// Check if the 64-bit server process is still alive (via SHM server_pid).
bool IsServerAlive();

} // namespace TexClient
