#include "SharedMemory.h"

#include <cstring>

namespace TexServer {

SharedMemory::~SharedMemory() {
    Destroy();
}

bool SharedMemory::Create() {
    // Avoid double-create.
    if (m_pBase) return false;

    // SHM_TOTAL_SIZE can exceed 4 GiB, so split into high/low DWORD.
    DWORD sizeHigh = static_cast<DWORD>(TexProto::SHM_TOTAL_SIZE >> 32);
    DWORD sizeLow  = static_cast<DWORD>(TexProto::SHM_TOTAL_SIZE & 0xFFFFFFFF);

    m_hMapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   // backed by system paging file
        nullptr,                // default security
        PAGE_READWRITE,
        sizeHigh,
        sizeLow,
        TexProto::SHM_NAME
    );

    if (!m_hMapping) return false;

    m_pBase = static_cast<uint8_t*>(MapViewOfFile(
        m_hMapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        0  // map entire object
    ));

    if (!m_pBase) {
        CloseHandle(m_hMapping);
        m_hMapping = nullptr;
        return false;
    }

    // Zero the entire region (MapViewOfFile from the paging file is
    // zero-initialized by Windows, but be explicit for clarity).
    std::memset(m_pBase, 0, static_cast<size_t>(TexProto::SHM_TOTAL_SIZE));

    // Initialize the global header.
    auto* hdr = GetHeader();
    hdr->magic          = TexProto::SHM_MAGIC;
    hdr->version        = TexProto::SHM_VERSION;
    hdr->slot_count     = TexProto::SLOT_COUNT;
    hdr->slot_data_size = TexProto::SLOT_DATA_SIZE;
    hdr->server_pid     = static_cast<uint64_t>(GetCurrentProcessId());
    hdr->sequence       = 0;

    // All slot states are already 0 (free) from the memset.
    return true;
}

void SharedMemory::Destroy() {
    if (m_pBase) {
        UnmapViewOfFile(m_pBase);
        m_pBase = nullptr;
    }
    if (m_hMapping) {
        CloseHandle(m_hMapping);
        m_hMapping = nullptr;
    }
}

bool SharedMemory::IsValid() const {
    return m_pBase != nullptr;
}

TexProto::ShmHeader* SharedMemory::GetHeader() {
    if (!m_pBase) return nullptr;
    return reinterpret_cast<TexProto::ShmHeader*>(m_pBase);
}

TexProto::SlotHeader* SharedMemory::GetSlotHeader(int32_t slot) {
    if (!m_pBase || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT))
        return nullptr;

    // Layout: [ShmHeader (4096 bytes)] [slot0 (SLOT_TOTAL)] [slot1] ...
    // Each slot starts at: SHM_HEADER + slot * SLOT_TOTAL
    uint8_t* slotBase = m_pBase
        + TexProto::SHM_HEADER
        + static_cast<uint64_t>(slot) * TexProto::SLOT_TOTAL;

    return reinterpret_cast<TexProto::SlotHeader*>(slotBase);
}

uint8_t* SharedMemory::GetSlotData(int32_t slot) {
    if (!m_pBase || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT))
        return nullptr;

    // Data region starts SLOT_HEADER bytes after the slot base.
    uint8_t* slotBase = m_pBase
        + TexProto::SHM_HEADER
        + static_cast<uint64_t>(slot) * TexProto::SLOT_TOTAL;

    return slotBase + TexProto::SLOT_HEADER;
}

int32_t SharedMemory::AllocateSlot() {
    if (!m_pBase) return -1;

    for (int32_t i = 0; i < static_cast<int32_t>(TexProto::SLOT_COUNT); ++i) {
        auto* sh = GetSlotHeader(i);
        // Atomic CAS: if state == Empty(0), set to Reading(1).
        LONG prev = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&sh->state),
            static_cast<LONG>(TexProto::SlotState::Reading),
            static_cast<LONG>(TexProto::SlotState::Empty)
        );
        if (prev == 0) {
            return i;
        }
    }
    return -1;  // all slots in use
}

void SharedMemory::MarkSlotReady(int32_t slot) {
    auto* sh = GetSlotHeader(slot);
    if (!sh) return;

    // Set state to Ready (3) — matches SlotState::Ready in Protocol.h.
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&sh->state),
        static_cast<LONG>(TexProto::SlotState::Ready)
    );

    // Increment the global sequence counter.
    auto* hdr = GetHeader();
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&hdr->sequence));
}

void SharedMemory::FreeSlot(int32_t slot) {
    auto* sh = GetSlotHeader(slot);
    if (!sh) return;

    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&sh->state),
        static_cast<LONG>(TexProto::SlotState::Empty)
    );
}

} // namespace TexServer
