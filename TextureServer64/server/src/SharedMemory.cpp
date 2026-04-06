#include "SharedMemory.h"

#include <cstring>
#include <string>

namespace TexServer {

SharedMemory::~SharedMemory() {
    Destroy();
}

auto SharedMemory::Create() -> bool {
    // Avoid double-create.
    if (m_pHeaderBase) {
        return false;
    }

    m_hHeaderMapping.reset(CreateFileMappingA(
        INVALID_HANDLE_VALUE, // backed by system paging file
        nullptr,              // default security
        PAGE_READWRITE,
        0,
        TexProto::SHM_HEADER,
        TexProto::SHM_NAME
    ));

    if (!m_hHeaderMapping) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another server already owns this mapping — do not zero live memory.
        m_hHeaderMapping.reset();
        return false;
    }

    m_pHeaderBase.reset(MapViewOfFile(
        m_hHeaderMapping.get(),
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        0 // map entire object
    ));

    if (!m_pHeaderBase) {
        return false;
    }

    std::memset(m_pHeaderBase.get(), 0, TexProto::SHM_HEADER);

    for (uint32_t window = 0; window < TexProto::SHM_WINDOW_COUNT; ++window) {
        const std::string mappingName = std::string(TexProto::SHM_DATA_NAME_PREFIX) + std::to_string(window);
        auto const sizeHigh = static_cast<DWORD>(TexProto::SHM_DATA_WINDOW_SIZE >> 32);
        auto const sizeLow = static_cast<DWORD>(TexProto::SHM_DATA_WINDOW_SIZE & 0xFFFFFFFF);

        m_hDataMappings[window].reset(
            CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, sizeHigh, sizeLow, mappingName.c_str())
        );
        if (!m_hDataMappings[window]) {
            Destroy();
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Destroy();
            return false;
        }

        m_pDataBases[window].reset(MapViewOfFile(m_hDataMappings[window].get(), FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!m_pDataBases[window]) {
            Destroy();
            return false;
        }

        std::memset(m_pDataBases[window].get(), 0, static_cast<size_t>(TexProto::SHM_DATA_WINDOW_SIZE));
    }

    // Initialize the global header.
    auto* hdr = GetHeader();
    hdr->magic = TexProto::SHM_MAGIC;
    hdr->version = TexProto::SHM_VERSION;
    hdr->slot_count = TexProto::SLOT_COUNT;
    hdr->slot_data_size = TexProto::SLOT_DATA_SIZE;
    hdr->server_pid = static_cast<uint64_t>(GetCurrentProcessId());
    hdr->sequence = 0;

    // All slot states are already 0 (free) from the memset.
    return true;
}

void SharedMemory::Destroy() {
    for (uint32_t window = 0; window < TexProto::SHM_WINDOW_COUNT; ++window) {
        m_pDataBases[window].reset();
        m_hDataMappings[window].reset();
    }
    m_pHeaderBase.reset();
    m_hHeaderMapping.reset();
}

auto SharedMemory::IsValid() const -> bool {
    return static_cast<bool>(m_pHeaderBase);
}

auto SharedMemory::GetHeader() -> TexProto::ShmHeader* {
    if (!m_pHeaderBase) {
        return nullptr;
    }
    return reinterpret_cast<TexProto::ShmHeader*>(m_pHeaderBase.get());
}

auto SharedMemory::GetSlotHeader(int32_t slot) -> TexProto::SlotHeader* {
    if (!m_pHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT)) {
        return nullptr;
    }

    const uint32_t window = static_cast<uint32_t>(slot) / TexProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TexProto::SLOTS_PER_WINDOW;
    uint8_t* slotBase = m_pDataBases[window].get() + (static_cast<uint64_t>(slotInWindow) * TexProto::SLOT_TOTAL);

    return reinterpret_cast<TexProto::SlotHeader*>(slotBase);
}

auto SharedMemory::GetSlotData(int32_t slot) -> uint8_t* {
    if (!m_pHeaderBase || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT)) {
        return nullptr;
    }

    const uint32_t window = static_cast<uint32_t>(slot) / TexProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TexProto::SLOTS_PER_WINDOW;
    uint8_t* slotBase = m_pDataBases[window].get() + (static_cast<uint64_t>(slotInWindow) * TexProto::SLOT_TOTAL);

    return slotBase + TexProto::SLOT_HEADER;
}

auto SharedMemory::TryReclaimSlotInState(TexProto::SlotState target, DWORD leaseMs, DWORD now) -> int32_t {
    for (int32_t i = 0; i < static_cast<int32_t>(TexProto::SLOT_COUNT); ++i) {
        auto* sh = GetSlotHeader(i);
        if (sh->state == static_cast<uint32_t>(target)) {
            // m_readyTick records when the slot became Ready (set in MarkSlotReady),
            // not when it transitioned to the current state.  For Uploaded slots
            // this is a conservative lower bound — the actual Uploaded age is shorter.
            auto const readyAt = static_cast<DWORD>(InterlockedCompareExchange(&m_readyTick[i], 0, 0));
            if (static_cast<LONG>(now - readyAt) >= static_cast<LONG>(leaseMs)) {
                LONG const prev = InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(&sh->state),
                    static_cast<LONG>(TexProto::SlotState::Reading),
                    static_cast<LONG>(target)
                );
                if (prev == static_cast<LONG>(target)) {
                    return i;
                }
            }
        }
    }
    return -1;
}

auto SharedMemory::AllocateSlot() -> int32_t {
    if (!m_pHeaderBase) {
        return -1;
    }

    // Fast path: find an Empty slot.
    for (int32_t i = 0; i < static_cast<int32_t>(TexProto::SLOT_COUNT); ++i) {
        auto* sh = GetSlotHeader(i);
        // Atomic CAS: if state == Empty(0), set to Reading(1).
        LONG const prev = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&sh->state),
            static_cast<LONG>(TexProto::SlotState::Reading),
            static_cast<LONG>(TexProto::SlotState::Empty)
        );
        if (prev == 0) {
            return i;
        }
    }

    // Slow path: reclaim stale slots.  Ready slots are reclaimed after 5s
    // (the client CASes Ready→Uploaded before reading; the lease prevents
    // the server from recycling during that window).  Uploaded slots are
    // reclaimed after 30s (the client should release to Empty within a few
    // frames; stuck means crashed or lost).
    static constexpr DWORD READY_LEASE_MS = 5000;
    static constexpr DWORD UPLOADED_LEASE_MS = 30'000;
    DWORD const now = GetTickCount();

    int32_t slot = TryReclaimSlotInState(TexProto::SlotState::Ready, READY_LEASE_MS, now);
    if (slot >= 0) {
        return slot;
    }
    slot = TryReclaimSlotInState(TexProto::SlotState::Uploaded, UPLOADED_LEASE_MS, now);
    if (slot >= 0) {
        return slot;
    }

    return -1; // all slots in use (Reading or Decoding — actively in flight)
}

void SharedMemory::MarkSlotReady(int32_t slot) {
    auto* sh = GetSlotHeader(slot);
    if (sh == nullptr) {
        return;
    }

    // Write the ready tick BEFORE publishing Ready state.
    // Another thread's AllocateSlot reads m_readyTick after seeing state == Ready;
    // if we wrote the tick after the state transition, it could see Ready with a
    // stale/zero tick and reclaim the slot immediately.
    InterlockedExchange(&m_readyTick[slot], static_cast<LONG>(GetTickCount()));

    // Set state to Ready (3) — matches SlotState::Ready in Protocol.h.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&sh->state), static_cast<LONG>(TexProto::SlotState::Ready));

    // Increment the global sequence counter.
    auto* hdr = GetHeader();
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&hdr->sequence));
}

void SharedMemory::FreeSlot(int32_t slot) {
    auto* sh = GetSlotHeader(slot);
    if (sh == nullptr) {
        return;
    }

    InterlockedExchange(reinterpret_cast<volatile LONG*>(&sh->state), static_cast<LONG>(TexProto::SlotState::Empty));
}

} // namespace TexServer
