#include "SharedMemory.h"

#include <cstring>
#include <string>

namespace TexServer {

SharedMemory::~SharedMemory() {
    Destroy();
}

bool SharedMemory::Create() {
    // Avoid double-create.
    if (m_pHeaderBase) return false;

    m_hHeaderMapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   // backed by system paging file
        nullptr,                // default security
        PAGE_READWRITE,
        0,
        TexProto::SHM_HEADER,
        TexProto::SHM_NAME
    );

    if (!m_hHeaderMapping) return false;

    m_pHeaderBase = static_cast<uint8_t*>(MapViewOfFile(
        m_hHeaderMapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        0  // map entire object
    ));

    if (!m_pHeaderBase) {
        CloseHandle(m_hHeaderMapping);
        m_hHeaderMapping = nullptr;
        return false;
    }

    std::memset(m_pHeaderBase, 0, TexProto::SHM_HEADER);

    for (uint32_t window = 0; window < TexProto::SHM_WINDOW_COUNT; ++window) {
        const std::string mappingName = std::string(TexProto::SHM_DATA_NAME_PREFIX) + std::to_string(window);
        DWORD sizeHigh = static_cast<DWORD>(TexProto::SHM_DATA_WINDOW_SIZE >> 32);
        DWORD sizeLow  = static_cast<DWORD>(TexProto::SHM_DATA_WINDOW_SIZE & 0xFFFFFFFF);

        m_hDataMappings[window] = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            sizeHigh,
            sizeLow,
            mappingName.c_str()
        );
        if (!m_hDataMappings[window]) {
            Destroy();
            return false;
        }

        m_pDataBases[window] = static_cast<uint8_t*>(MapViewOfFile(
            m_hDataMappings[window],
            FILE_MAP_ALL_ACCESS,
            0, 0,
            0
        ));
        if (!m_pDataBases[window]) {
            Destroy();
            return false;
        }

        std::memset(m_pDataBases[window], 0, static_cast<size_t>(TexProto::SHM_DATA_WINDOW_SIZE));
    }

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
    for (uint32_t window = 0; window < TexProto::SHM_WINDOW_COUNT; ++window) {
        if (m_pDataBases[window]) {
            UnmapViewOfFile(m_pDataBases[window]);
            m_pDataBases[window] = nullptr;
        }
        if (m_hDataMappings[window]) {
            CloseHandle(m_hDataMappings[window]);
            m_hDataMappings[window] = nullptr;
        }
    }

    if (m_pHeaderBase) {
        UnmapViewOfFile(m_pHeaderBase);
        m_pHeaderBase = nullptr;
    }
    if (m_hHeaderMapping) {
        CloseHandle(m_hHeaderMapping);
        m_hHeaderMapping = nullptr;
    }
}

bool SharedMemory::IsValid() const {
    return m_pHeaderBase != nullptr;
}

TexProto::ShmHeader* SharedMemory::GetHeader() {
    if (!m_pHeaderBase) return nullptr;
    return reinterpret_cast<TexProto::ShmHeader*>(m_pHeaderBase);
}

TexProto::SlotHeader* SharedMemory::GetSlotHeader(int32_t slot) {
    if (!m_pHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT))
        return nullptr;

    const uint32_t window = static_cast<uint32_t>(slot) / TexProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TexProto::SLOTS_PER_WINDOW;
    uint8_t* slotBase = m_pDataBases[window]
        + static_cast<uint64_t>(slotInWindow) * TexProto::SLOT_TOTAL;

    return reinterpret_cast<TexProto::SlotHeader*>(slotBase);
}

uint8_t* SharedMemory::GetSlotData(int32_t slot) {
    if (!m_pHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT))
        return nullptr;

    const uint32_t window = static_cast<uint32_t>(slot) / TexProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TexProto::SLOTS_PER_WINDOW;
    uint8_t* slotBase = m_pDataBases[window]
        + static_cast<uint64_t>(slotInWindow) * TexProto::SLOT_TOTAL;

    return slotBase + TexProto::SLOT_HEADER;
}

int32_t SharedMemory::AllocateSlot() {
    if (!m_pHeaderBase) return -1;

    // Fast path: find an Empty slot.
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

    // Slow path: reclaim stale Ready slots.  If the client crashed without
    // calling ReleaseSlot, slots can get stuck in Ready state permanently.
    // Reclaim the first Ready slot we find — the server's LRU cache still
    // holds the decoded pixels, so the data is not lost.
    for (int32_t i = 0; i < static_cast<int32_t>(TexProto::SLOT_COUNT); ++i) {
        auto* sh = GetSlotHeader(i);
        LONG prev = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&sh->state),
            static_cast<LONG>(TexProto::SlotState::Reading),
            static_cast<LONG>(TexProto::SlotState::Ready)
        );
        if (prev == static_cast<LONG>(TexProto::SlotState::Ready)) {
            return i;
        }
    }

    return -1;  // all slots in use (Reading or Decoding — actively in flight)
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
