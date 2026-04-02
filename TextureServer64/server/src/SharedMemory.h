#pragma once
// SharedMemory.h - Manages the shared memory region between the 64-bit
// texture server and the 32-bit WoW client.

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../shared/Protocol.h"

namespace TexServer {

class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();

    // Non-copyable, non-movable.
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    /// Create the shared memory mapping and initialize the header.
    bool Create();

    /// Unmap and close all handles.
    void Destroy();

    /// Returns true if the mapping is valid and usable.
    bool IsValid() const;

    /// Access the global header at offset 0.
    TexProto::ShmHeader* GetHeader();

    /// Access the slot header for the given slot index (bounds-checked).
    /// Returns nullptr if slot is out of range.
    TexProto::SlotHeader* GetSlotHeader(int32_t slot);

    /// Access the data region for the given slot index (bounds-checked).
    /// Returns nullptr if slot is out of range.
    uint8_t* GetSlotData(int32_t slot);

    /// Atomically allocate a free slot (state 0 -> 1).
    /// Returns the slot index, or -1 if all slots are in use.
    int32_t AllocateSlot();

    /// Mark a slot as ready for client consumption (state -> 2)
    /// and increment the global sequence counter.
    void MarkSlotReady(int32_t slot);

    /// Free a slot (state -> 0).
    void FreeSlot(int32_t slot);

private:
    HANDLE  m_hMapping = nullptr;
    uint8_t* m_pBase   = nullptr;
};

} // namespace TexServer
