#pragma once
// SharedMemory.h - Manages the shared memory region between the 64-bit
// texture server and the 32-bit WoW client.

#include "../../shared/Protocol.h"
#include "WinHandle.h"

#include <array>
#include <cstdint>

namespace TexServer {

class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();

    // Non-copyable, non-movable.
    SharedMemory(const SharedMemory&) = delete;
    auto operator=(const SharedMemory&) -> SharedMemory& = delete;

    /// Create the shared memory mapping and initialize the header.
    auto Create() -> bool;

    /// Unmap and close all handles.
    void Destroy();

    /// Returns true if the mapping is valid and usable.
    [[nodiscard]] auto IsValid() const -> bool;

    /// Access the global header at offset 0.
    auto GetHeader() -> TexProto::ShmHeader*;

    /// Access the slot header for the given slot index (bounds-checked).
    /// Returns nullptr if slot is out of range.
    auto GetSlotHeader(int32_t slot) -> TexProto::SlotHeader*;

    /// Access the data region for the given slot index (bounds-checked).
    /// Returns nullptr if slot is out of range.
    auto GetSlotData(int32_t slot) -> uint8_t*;

    /// Atomically allocate a free slot (state 0 -> 1).
    /// Returns the slot index, or -1 if all slots are in use.
    auto AllocateSlot() -> int32_t;

    /// Mark a slot as ready for client consumption (state -> 2)
    /// and increment the global sequence counter.
    void MarkSlotReady(int32_t slot);

    /// Free a slot (state -> 0).
    void FreeSlot(int32_t slot);

private:
    // Declaration order matters: views must appear AFTER their handles so that
    // the destructor (reverse order) unmaps views before closing handles.
    UniqueHandle<> m_hHeaderMapping;
    UniqueMapView m_pHeaderBase;
    std::array<UniqueHandle<>, TexProto::SHM_WINDOW_COUNT> m_hDataMappings;
    std::array<UniqueMapView, TexProto::SHM_WINDOW_COUNT> m_pDataBases;
};

} // namespace TexServer
