#include "Server.h"

#include "ServerLog.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace TexServer {

// ── Helpers ────────────────────────────────────────────────────────────────

static auto EndsWith(const std::string& str, const std::string& suffix) -> bool {
    if (suffix.size() > str.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

static auto NormalizePathKey(const std::string& path) -> std::string {
    std::string key = path;
    std::ranges::transform(key, key.begin(), [](unsigned char c) {
        if (c == '/') {
            return '\\';
        }
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

// ── Construction / destruction ─────────────────────────────────────────────

Server::Server(const ServerConfig& config)
    : config_(config), pool_(config.thread_count), cache_(config.cache_max_bytes) {}

Server::~Server() {
    Stop();
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

auto Server::Start() -> bool {
    ServerLog(
        "Creating shared memory header (%u bytes = %.1f KiB)...",
        TexProto::SHM_HEADER,
        static_cast<double>(TexProto::SHM_HEADER) / 1024.0
    );
    ServerLog(
        "Creating %u data windows (%llu bytes each = %.1f MiB, total %.1f MiB)...",
        TexProto::SHM_WINDOW_COUNT,
        static_cast<unsigned long long>(TexProto::SHM_DATA_WINDOW_SIZE),
        static_cast<double>(TexProto::SHM_DATA_WINDOW_SIZE) / (1024.0 * 1024.0),
        static_cast<double>(TexProto::SHM_WINDOW_COUNT) *
            (static_cast<double>(TexProto::SHM_DATA_WINDOW_SIZE) / (1024.0 * 1024.0))
    );

    if (!shm_.Create()) {
        ServerLogError("Failed to create shared memory. GetLastError=%lu", GetLastError());
        return false;
    }

    running_.store(true, std::memory_order_release);

    // Signal client-side event-based wait (M2).
    shm_ready_event_.reset(CreateEventA(nullptr, TRUE, FALSE, "VH_TexServer_ShmReady"));
    if (shm_ready_event_) {
        SetEvent(shm_ready_event_.get());
    }

    ServerLog(
        "Shared memory created OK. Slots=%u, SlotDataSize=%u KiB, Windows=%u, SlotsPerWindow=%u",
        TexProto::SLOT_COUNT,
        TexProto::SLOT_DATA_SIZE / 1024,
        TexProto::SHM_WINDOW_COUNT,
        TexProto::SLOTS_PER_WINDOW
    );
    ServerLog(
        "Started. PID=%lu, threads=%u, cache_max=%.1f MiB",
        static_cast<unsigned long>(GetCurrentProcessId()),
        pool_.WorkerCount(),
        static_cast<double>(config_.cache_max_bytes) / (1024.0 * 1024.0)
    );
    return true;
}

void Server::Stop() {
    bool const was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
        shm_ready_event_.reset();
        // Unblock ConnectNamedPipe on the accept thread.
        HANDLE dummy =
            CreateFileA(TexProto::PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (dummy != INVALID_HANDLE_VALUE) {
            CloseHandle(dummy);
        }
    }
}

// ── Named-pipe accept loop ─────────────────────────────────────────────────

void Server::Run() {
    while (running_.load(std::memory_order_acquire)) {
        // Create a new named pipe instance for the next client.
        HANDLE pipe = CreateNamedPipeA(
            TexProto::PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            64 * 1024, // out buffer
            64 * 1024, // in buffer
            0,         // default timeout
            nullptr    // default security
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            if (running_.load(std::memory_order_acquire)) {
                ServerLogError("CreateNamedPipe failed: %lu", GetLastError());
                Sleep(100);
            }
            continue;
        }

        // Wait for a client to connect.  ConnectNamedPipe blocks.
        BOOL const connected =
            (ConnectNamedPipe(pipe, nullptr) != 0) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

        if ((connected == 0) || !running_.load(std::memory_order_acquire)) {
            ServerLog("ConnectNamedPipe failed or shutting down. err=%lu", GetLastError());
            CloseHandle(pipe);
            continue;
        }

        pool_.Submit([this, pipe]() {
            HandleClient(pipe);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        });
    }

    ServerLog("Shutting down. Waiting for in-flight decodes...");
    pool_.WaitIdle();
    shm_.Destroy();
    ServerLog("Shutdown complete.");
}

// ── Per-client request loop ────────────────────────────────────────────────

void Server::HandleClient(HANDLE pipe) {
    while (running_.load(std::memory_order_acquire)) {
        // Read the 8-byte request header.
        TexProto::Request req{};
        if (!ReadPipe(pipe, &req, sizeof(req))) {
            break; // client disconnected or error
        }

        total_requests_.fetch_add(1, std::memory_order_relaxed);

        // Handle Shutdown.
        if (req.cmd == TexProto::Cmd::Shutdown) {
            ServerLog("Received Shutdown command.");
            Stop();
            TexProto::Response resp{};
            resp.status = TexProto::Status::Ok;
            SendResponse(pipe, resp);
            break;
        }

        // Handle Stats (no payload).
        if (req.cmd == TexProto::Cmd::Stats) {
            HandleStats(pipe);
            continue;
        }

        // All remaining commands need a path.
        if (req.path_len == 0 || req.path_len > 260) {
            TexProto::Response resp{};
            resp.status = TexProto::Status::ServerError;
            SendResponse(pipe, resp);
            continue;
        }

        // Read the path.
        std::string path(req.path_len, '\0');
        if (!ReadPipe(pipe, path.data(), req.path_len)) {
            break;
        }

        // For Load/Prefetch, also read the raw file data.
        std::vector<uint8_t> raw_data;
        if ((req.cmd == TexProto::Cmd::Load || req.cmd == TexProto::Cmd::Prefetch) && req.data_size > 0) {
            // Sanity check: reject absurdly large payloads (16 MiB).
            if (req.data_size > 16U * 1024U * 1024U) {
                TexProto::Response resp{};
                resp.status = TexProto::Status::ServerError;
                SendResponse(pipe, resp);
                continue;
            }
            raw_data.resize(req.data_size);
            if (!ReadPipe(pipe, raw_data.data(), req.data_size)) {
                break;
            }
        }

        // Dispatch by command type.
        switch (req.cmd) {
            case TexProto::Cmd::Load:
            case TexProto::Cmd::Prefetch:
                HandleLoad(pipe, path, raw_data, req.priority);
                break;

            case TexProto::Cmd::Query:
                HandleQuery(pipe, path);
                break;

            case TexProto::Cmd::Evict:
                // Evict is a no-op for now (client manages slot freeing via
                // SlotState::Uploaded -> Empty).  Acknowledge it.
                {
                    TexProto::Response resp{};
                    resp.status = TexProto::Status::Ok;
                    SendResponse(pipe, resp);
                }
                break;

            default: {
                TexProto::Response resp{};
                resp.status = TexProto::Status::ServerError;
                SendResponse(pipe, resp);
            } break;
        }
    }
}

// ── Load / Prefetch ────────────────────────────────────────────────────────

void Server::HandleLoad(HANDLE pipe, const std::string& path, const std::vector<uint8_t>& raw_data, uint8_t priority) {
    const std::string cacheKey = NormalizePathKey(path);

    // 1. Check LRU cache.  Get returns a shared_ptr — zero-copy, safe to use
    //    after the cache lock is released.
    auto cached = cache_.Get(cacheKey);
    if (cached) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);

        // Place cached pixels into a shared memory slot.
        int32_t const slot = PlaceInSharedMemory(*cached, path);
        if (slot < 0) {
            TexProto::Response resp{};
            resp.status = TexProto::Status::NoSlot;
            SendResponse(pipe, resp);
            return;
        }

        TexProto::Response resp{};
        resp.status = TexProto::Status::Cached;
        resp.slot_id = static_cast<uint16_t>(slot);
        resp.width = cached->width;
        resp.height = cached->height;
        resp.data_size = static_cast<uint32_t>(cached->pixels.size());
        resp.format = cached->format;
        resp.mip_levels = cached->mip_levels;
        SendResponse(pipe, resp);
        return;
    }

    // 2. Back-pressure: check inflight limits.
    uint32_t const inflight = inflight_decodes_.load(std::memory_order_acquire);
    uint64_t const queued = queued_bytes_.load(std::memory_order_acquire);
    if (inflight >= TexProto::MAX_INFLIGHT_DECODES ||
        queued + raw_data.size() > static_cast<uint64_t>(TexProto::MAX_DECODE_QUEUE_MB) * 1024ULL * 1024ULL) {
        TexProto::Response resp{};
        resp.status = TexProto::Status::NoSlot; // signal back-pressure
        SendResponse(pipe, resp);
        return;
    }

    // 3. Validate we have raw data.
    if (raw_data.empty()) {
        TexProto::Response resp{};
        resp.status = TexProto::Status::NotFound;
        SendResponse(pipe, resp);
        return;
    }

    // 4. Decode synchronously on this thread (the pipe is per-client, so we
    //    block only this client).  For Prefetch, we could dispatch to the
    //    thread pool, but for simplicity and correct pipe response ordering
    //    we decode inline.
    inflight_decodes_.fetch_add(1, std::memory_order_acq_rel);
    queued_bytes_.fetch_add(raw_data.size(), std::memory_order_acq_rel);

    DecodedTexture decoded;
    bool const ok = DecodeRawData(path, raw_data, decoded);

    queued_bytes_.fetch_sub(raw_data.size(), std::memory_order_acq_rel);
    inflight_decodes_.fetch_sub(1, std::memory_order_acq_rel);

    if (!ok) {
        ServerLogError("DECODE FAILED: '%s' (%u raw bytes)", path.c_str(), static_cast<unsigned>(raw_data.size()));
        decode_failures_.fetch_add(1, std::memory_order_relaxed);
        TexProto::Response resp{};
        resp.status = TexProto::Status::DecodeFail;
        SendResponse(pipe, resp);
        return;
    }

    // 5. Place in shared memory.
    int32_t const slot = PlaceInSharedMemory(decoded, path);
    if (slot < 0) {
        TexProto::Response resp{};
        resp.status = TexProto::Status::NoSlot;
        SendResponse(pipe, resp);
        return;
    }

    // 6. Cache the decoded texture for future requests.
    TexProto::Response resp{};
    resp.status = TexProto::Status::Ok;
    resp.slot_id = static_cast<uint16_t>(slot);
    resp.width = decoded.width;
    resp.height = decoded.height;
    resp.data_size = static_cast<uint32_t>(decoded.pixels.size());
    resp.format = decoded.format;
    resp.mip_levels = decoded.mip_levels;

    // Cache a copy before we send the response (Put takes by value/move).
    cache_.Put(cacheKey, std::move(decoded));

    SendResponse(pipe, resp);
}

// ── Query ──────────────────────────────────────────────────────────────────

void Server::HandleQuery(HANDLE pipe, const std::string& path) {
    const std::string cacheKey = NormalizePathKey(path);
    auto cached = cache_.Get(cacheKey);

    TexProto::Response resp{};
    if (cached) {
        resp.status = TexProto::Status::Cached;
        resp.width = cached->width;
        resp.height = cached->height;
        resp.data_size = static_cast<uint32_t>(cached->pixels.size());
        resp.format = cached->format;
        resp.mip_levels = cached->mip_levels;
    } else {
        resp.status = TexProto::Status::NotCached;
    }
    SendResponse(pipe, resp);
}

// ── Stats ──────────────────────────────────────────────────────────────────

void Server::HandleStats(HANDLE pipe) {
    TexProto::Response resp{};
    resp.status = TexProto::Status::Ok;
    // Repurpose numeric fields to convey stats:
    //   width      = total requests (low 32 bits)
    //   height     = cache entries
    //   data_size  = cache bytes (MiB, truncated to 32 bits)
    //   mip_levels = inflight decodes (clamped to 255)
    resp.width = static_cast<uint32_t>(total_requests_.load(std::memory_order_relaxed));
    resp.height = static_cast<uint32_t>(cache_.EntryCount());
    resp.data_size = static_cast<uint32_t>(cache_.CurrentBytes() / (1024 * 1024));
    resp.mip_levels = static_cast<uint8_t>(std::min<uint32_t>(inflight_decodes_.load(std::memory_order_relaxed), 255U));
    SendResponse(pipe, resp);
}

// ── Decode dispatch ────────────────────────────────────────────────────────

auto Server::DecodeRawData(const std::string& path, const std::vector<uint8_t>& raw_data, DecodedTexture& result) -> bool {
    if (raw_data.empty()) {
        return false;
    }

    if (EndsWith(path, ".blp")) {
        return BlpDecoder::Decode(raw_data.data(), raw_data.size(), result);
    }

    if (EndsWith(path, ".tga")) {
        return TexServer::TgaDecoder::Decode(raw_data.data(), raw_data.size(), result);
    }

    // Unknown format -- try BLP first (most WoW textures are BLP).
    if (BlpDecoder::Decode(raw_data.data(), raw_data.size(), result)) {
        return true;
    }

    return TexServer::TgaDecoder::Decode(raw_data.data(), raw_data.size(), result);
}

// ── Shared memory placement ────────────────────────────────────────────────

auto Server::PlaceInSharedMemory(const DecodedTexture& tex, const std::string& path) -> int32_t {
    // Check that the decoded data fits in a slot.
    if (tex.pixels.size() > TexProto::SLOT_DATA_SIZE) {
        return -1;
    }

    // Allocate a free slot (CAS: Empty -> Reading).
    int32_t const slot = shm_.AllocateSlot();
    if (slot < 0) {
        return -1;
    }

    // Fill the slot header.
    auto* sh = shm_.GetSlotHeader(slot);
    sh->width = tex.width;
    sh->height = tex.height;
    sh->data_size = static_cast<uint32_t>(tex.pixels.size());
    sh->format = tex.format;
    sh->mip_levels = tex.mip_levels;
    sh->path_hash = TexProto::HashPath(path.c_str());

    // Copy pixel data.
    uint8_t* dst = shm_.GetSlotData(slot);
    std::memcpy(dst, tex.pixels.data(), tex.pixels.size());

    // Transition: Reading -> Ready (via MarkSlotReady).
    shm_.MarkSlotReady(slot);

    return slot;
}

// ── Pipe I/O helpers ───────────────────────────────────────────────────────

auto Server::SendResponse(HANDLE pipe, const TexProto::Response& resp) -> bool {
    DWORD written = 0;
    return (WriteFile(pipe, &resp, sizeof(resp), &written, nullptr) != 0) && written == sizeof(resp);
}

auto Server::ReadPipe(HANDLE pipe, void* buf, DWORD size) -> bool {
    DWORD total_read = 0;
    auto* p = static_cast<uint8_t*>(buf);

    while (total_read < size) {
        DWORD bytes_read = 0;
        BOOL const ok = ReadFile(pipe, p + total_read, size - total_read, &bytes_read, nullptr);
        if ((ok == 0) || bytes_read == 0) {
            return false;
        }
        total_read += bytes_read;
    }
    return true;
}

} // namespace TexServer
