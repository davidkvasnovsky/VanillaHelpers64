// TexClient.cpp - 32-bit client implementation for TextureServer64 IPC.

#include "TexClient.h"

#include <cstring>
#include <string>

namespace TexClient {

// ── Module state ────────────────────────────────────────────────────────
static HANDLE s_shm_header_handle = nullptr;
static uint8_t* s_shm_header_base = nullptr;

static HANDLE s_shm_data_handles[TexProto::SHM_WINDOW_COUNT] = {};
static uint8_t* s_shm_data_bases[TexProto::SHM_WINDOW_COUNT] = {};

// ── Helpers ─────────────────────────────────────────────────────────────

static const TexProto::ShmHeader* ShmHeader() {
    return reinterpret_cast<const TexProto::ShmHeader*>(s_shm_header_base);
}

/// Return pointer to the SlotHeader for a given slot index (windowed layout).
static TexProto::SlotHeader* SlotHeaderAt(int32_t slot) {
    const uint32_t window = static_cast<uint32_t>(slot) / TexProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TexProto::SLOTS_PER_WINDOW;
    return reinterpret_cast<TexProto::SlotHeader*>(
        s_shm_data_bases[window] + static_cast<uint64_t>(slotInWindow) * TexProto::SLOT_TOTAL
    );
}

/// Return pointer to the pixel data region for a given slot index (windowed layout).
static const uint8_t* SlotDataAt(int32_t slot) {
    const uint32_t window = static_cast<uint32_t>(slot) / TexProto::SLOTS_PER_WINDOW;
    const uint32_t slotInWindow = static_cast<uint32_t>(slot) % TexProto::SLOTS_PER_WINDOW;
    return s_shm_data_bases[window] +
           static_cast<uint64_t>(slotInWindow) * TexProto::SLOT_TOTAL +
           TexProto::SLOT_HEADER;
}

/// Helper: open the named pipe, send a complete message, receive a Response.
/// Returns true on success and fills `resp`.
static bool PipeTransaction(
    const TexProto::Request& req, const char* path, const uint8_t* data, uint32_t data_size, TexProto::Response& resp
) {
    // Open the named pipe
    HANDLE pipe = CreateFileA(
        TexProto::PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,       // no sharing
        nullptr, // default security
        OPEN_EXISTING,
        0, // default attributes
        nullptr
    );

    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Set pipe to message mode for reads
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    bool ok = true;
    DWORD written = 0;

    // 1) Send the 8-byte request header
    ok = ok && WriteFile(pipe, &req, sizeof(req), &written, nullptr) && (written == sizeof(req));

    // 2) Send path bytes (if any)
    if (ok && req.path_len > 0) {
        ok = WriteFile(pipe, path, req.path_len, &written, nullptr) && (written == req.path_len);
    }

    // 3) Send raw file data (if any)
    if (ok && data_size > 0 && data != nullptr) {
        // Write in chunks to handle large payloads
        uint32_t remaining = data_size;
        const uint8_t* ptr = data;
        while (ok && remaining > 0) {
            DWORD chunk = (remaining > 65'536) ? 65'536 : remaining;
            ok = WriteFile(pipe, ptr, chunk, &written, nullptr) && (written == chunk);
            ptr += written;
            remaining -= written;
        }
    }

    // Flush to ensure all bytes are sent before reading
    if (ok) {
        FlushFileBuffers(pipe);
    }

    // 4) Read the response header
    if (ok) {
        DWORD bytesRead = 0;
        DWORD totalRead = 0;
        auto* respBuf = reinterpret_cast<uint8_t*>(&resp);

        while (ok && totalRead < sizeof(resp)) {
            ok = ReadFile(pipe, respBuf + totalRead, sizeof(resp) - totalRead, &bytesRead, nullptr) != FALSE;
            if (ok) {
                totalRead += bytesRead;
            }
        }
        ok = ok && (totalRead == sizeof(resp));
    }

    CloseHandle(pipe);
    return ok;
}

// ── Public API ──────────────────────────────────────────────────────────

bool Initialize() {
    if (s_shm_header_base) {
        return true; // already initialised
    }

    // Open the header mapping.
    s_shm_header_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, TexProto::SHM_NAME);
    if (!s_shm_header_handle) {
        return false;
    }

    s_shm_header_base = static_cast<uint8_t*>(MapViewOfFile(s_shm_header_handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));

    if (!s_shm_header_base) {
        CloseHandle(s_shm_header_handle);
        s_shm_header_handle = nullptr;
        return false;
    }

    // Validate the SHM header
    const auto* hdr = ShmHeader();
    if (hdr->magic != TexProto::SHM_MAGIC || hdr->version != TexProto::SHM_VERSION) {
        UnmapViewOfFile(s_shm_header_base);
        s_shm_header_base = nullptr;
        CloseHandle(s_shm_header_handle);
        s_shm_header_handle = nullptr;
        return false;
    }

    // Open the windowed data mappings (must match server's SharedMemory::Create).
    for (uint32_t w = 0; w < TexProto::SHM_WINDOW_COUNT; ++w) {
        std::string name = std::string(TexProto::SHM_DATA_NAME_PREFIX) + std::to_string(w);
        s_shm_data_handles[w] = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!s_shm_data_handles[w]) {
            Shutdown();
            return false;
        }
        s_shm_data_bases[w] = static_cast<uint8_t*>(MapViewOfFile(s_shm_data_handles[w], FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!s_shm_data_bases[w]) {
            Shutdown();
            return false;
        }
    }

    return true;
}

void Shutdown() {
    for (uint32_t w = 0; w < TexProto::SHM_WINDOW_COUNT; ++w) {
        if (s_shm_data_bases[w]) {
            UnmapViewOfFile(s_shm_data_bases[w]);
            s_shm_data_bases[w] = nullptr;
        }
        if (s_shm_data_handles[w]) {
            CloseHandle(s_shm_data_handles[w]);
            s_shm_data_handles[w] = nullptr;
        }
    }
    if (s_shm_header_base) {
        UnmapViewOfFile(s_shm_header_base);
        s_shm_header_base = nullptr;
    }
    if (s_shm_header_handle) {
        CloseHandle(s_shm_header_handle);
        s_shm_header_handle = nullptr;
    }
}

int32_t RequestDecode(const char* path, const uint8_t* raw_data, uint32_t raw_size, uint8_t priority) {
    if (!s_shm_header_base || !path || !raw_data || raw_size == 0) {
        return -1;
    }

    const auto path_len = static_cast<uint16_t>(strlen(path));

    TexProto::Request req{};
    req.cmd = TexProto::Cmd::Load;
    req.priority = priority;
    req.path_len = path_len;
    req.data_size = raw_size;

    TexProto::Response resp{};
    if (!PipeTransaction(req, path, raw_data, raw_size, resp)) {
        return -1;
    }

    if (resp.status != TexProto::Status::Ok && resp.status != TexProto::Status::Cached) {
        return -1;
    }

    return static_cast<int32_t>(resp.slot_id);
}

void ReleaseSlot(int32_t slot) {
    if (!s_shm_header_base || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT)) {
        return;
    }

    TexProto::SlotHeader* hdr = SlotHeaderAt(slot);

    // Atomically set slot state to Empty so the server can reclaim it.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->state), static_cast<LONG>(TexProto::SlotState::Empty));
}

bool IsTextureCached(const char* path) {
    if (!s_shm_header_base || !path) {
        return false;
    }

    const auto path_len = static_cast<uint16_t>(strlen(path));

    TexProto::Request req{};
    req.cmd = TexProto::Cmd::Query;
    req.priority = 0;
    req.path_len = path_len;
    req.data_size = 0;

    TexProto::Response resp{};
    if (!PipeTransaction(req, path, nullptr, 0, resp)) {
        return false;
    }

    return resp.status == TexProto::Status::Cached;
}

bool ReadSlot(int32_t slot, SlotView& view) {
    if (!s_shm_header_base || slot < 0 || slot >= static_cast<int32_t>(TexProto::SLOT_COUNT)) {
        return false;
    }

    const TexProto::SlotHeader* hdr = SlotHeaderAt(slot);

    // Only read if the slot is in Ready state (decoded pixels available)
    if (static_cast<TexProto::SlotState>(hdr->state) != TexProto::SlotState::Ready) {
        return false;
    }

    view.pixels = SlotDataAt(slot);
    view.width = hdr->width;
    view.height = hdr->height;
    view.data_size = hdr->data_size;
    view.format = hdr->format;

    return true;
}

bool IsServerAlive() {
    if (!s_shm_header_base) {
        return false;
    }

    const auto* hdr = ShmHeader();
    DWORD pid = static_cast<DWORD>(hdr->server_pid);
    if (pid == 0) {
        return false;
    }

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }

    DWORD exitCode = 0;
    BOOL result = GetExitCodeProcess(proc, &exitCode);
    CloseHandle(proc);

    // STILL_ACTIVE (259) means the process is running
    return result && (exitCode == STILL_ACTIVE);
}

} // namespace TexClient
