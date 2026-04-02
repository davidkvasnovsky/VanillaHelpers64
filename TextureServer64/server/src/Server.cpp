#include "Server.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace TexServer {

// ── Helpers ────────────────────────────────────────────────────────────────

static bool EndsWith(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
}

// ── Construction / destruction ─────────────────────────────────────────────

Server::Server(const ServerConfig& config)
    : config_(config)
    , pool_(config.thread_count)
    , cache_(config.cache_max_bytes)
{}

Server::~Server() {
    Stop();
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

bool Server::Start() {
    printf("[TextureServer] Creating shared memory (%llu bytes = %.1f MiB)...\n",
           static_cast<unsigned long long>(TexProto::SHM_TOTAL_SIZE),
           static_cast<double>(TexProto::SHM_TOTAL_SIZE) / (1024.0 * 1024.0));
    fflush(stdout);

    if (!shm_.Create()) {
        fprintf(stderr, "[TextureServer] ERROR: Failed to create shared memory. GetLastError=%lu\n",
                GetLastError());
        fflush(stderr);
        return false;
    }

    running_.store(true, std::memory_order_release);
    printf("[TextureServer] Shared memory created OK. Slots=%u, SlotDataSize=%u KiB\n",
           TexProto::SLOT_COUNT, TexProto::SLOT_DATA_SIZE / 1024);
    printf("[TextureServer] Started. PID=%lu, threads=%u, cache_max=%.1f MiB\n",
           static_cast<unsigned long>(GetCurrentProcessId()),
           pool_.WorkerCount(),
           static_cast<double>(config_.cache_max_bytes) / (1024.0 * 1024.0));
    fflush(stdout);
    return true;
}

void Server::Stop() {
    running_.store(false, std::memory_order_release);
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
            64 * 1024,   // out buffer
            64 * 1024,   // in buffer
            0,           // default timeout
            nullptr      // default security
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            if (running_.load(std::memory_order_acquire)) {
                fprintf(stderr, "[TextureServer] CreateNamedPipe failed: %lu\n",
                        GetLastError());
                Sleep(100);
            }
            continue;
        }

        // Wait for a client to connect.  ConnectNamedPipe blocks.
        printf("[TextureServer] Waiting for pipe connection...\n");
        fflush(stdout);

        BOOL connected = ConnectNamedPipe(pipe, nullptr)
                       ? TRUE
                       : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

        if (!connected || !running_.load(std::memory_order_acquire)) {
            printf("[TextureServer] ConnectNamedPipe failed or shutting down. err=%lu\n",
                   GetLastError());
            fflush(stdout);
            CloseHandle(pipe);
            continue;
        }

        printf("[TextureServer] Client connected!\n");
        fflush(stdout);

        // Handle all requests from this client on the accept thread.
        HandleClient(pipe);

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    printf("[TextureServer] Shutting down. Waiting for in-flight decodes...\n");
    pool_.WaitIdle();
    shm_.Destroy();
    printf("[TextureServer] Shutdown complete.\n");
}

// ── Per-client request loop ────────────────────────────────────────────────

void Server::HandleClient(HANDLE pipe) {
    while (running_.load(std::memory_order_acquire)) {
        // Read the 8-byte request header.
        TexProto::Request req{};
        if (!ReadPipe(pipe, &req, sizeof(req)))
            break;  // client disconnected or error

        total_requests_.fetch_add(1, std::memory_order_relaxed);

        // Handle Shutdown.
        if (req.cmd == TexProto::Cmd::Shutdown) {
            printf("[TextureServer] Received Shutdown command.\n");
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
        if (!ReadPipe(pipe, &path[0], req.path_len))
            break;

        // For Load/Prefetch, also read the raw file data.
        std::vector<uint8_t> raw_data;
        if ((req.cmd == TexProto::Cmd::Load || req.cmd == TexProto::Cmd::Prefetch)
            && req.data_size > 0)
        {
            // Sanity check: reject absurdly large payloads (16 MiB).
            if (req.data_size > 16u * 1024u * 1024u) {
                TexProto::Response resp{};
                resp.status = TexProto::Status::ServerError;
                SendResponse(pipe, resp);
                continue;
            }
            raw_data.resize(req.data_size);
            if (!ReadPipe(pipe, raw_data.data(), req.data_size))
                break;
        }

        printf("[TextureServer] Request: cmd=%u path='%s' data_size=%u\n",
               static_cast<unsigned>(req.cmd), path.c_str(),
               static_cast<unsigned>(raw_data.size()));
        fflush(stdout);

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

        default:
            {
                TexProto::Response resp{};
                resp.status = TexProto::Status::ServerError;
                SendResponse(pipe, resp);
            }
            break;
        }
    }
}

// ── Load / Prefetch ────────────────────────────────────────────────────────

void Server::HandleLoad(HANDLE pipe, const std::string& path,
                        const std::vector<uint8_t>& raw_data, uint8_t priority)
{
    // 1. Check LRU cache.
    const DecodedTexture* cached = cache_.Get(path);
    if (cached) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);

        // Place cached pixels into a shared memory slot.
        int32_t slot = PlaceInSharedMemory(*cached, path);
        if (slot < 0) {
            TexProto::Response resp{};
            resp.status = TexProto::Status::NoSlot;
            SendResponse(pipe, resp);
            return;
        }

        TexProto::Response resp{};
        resp.status     = TexProto::Status::Cached;
        resp.slot_id    = static_cast<uint16_t>(slot);
        resp.width      = cached->width;
        resp.height     = cached->height;
        resp.data_size  = static_cast<uint32_t>(cached->pixels.size());
        resp.format     = cached->format;
        resp.mip_levels = cached->mip_levels;
        SendResponse(pipe, resp);
        return;
    }

    // 2. Back-pressure: check inflight limits.
    uint32_t inflight = inflight_decodes_.load(std::memory_order_acquire);
    uint64_t queued   = queued_bytes_.load(std::memory_order_acquire);
    if (inflight >= TexProto::MAX_INFLIGHT_DECODES ||
        queued + raw_data.size() > static_cast<uint64_t>(TexProto::MAX_DECODE_QUEUE_MB) * 1024ULL * 1024ULL)
    {
        TexProto::Response resp{};
        resp.status = TexProto::Status::NoSlot;  // signal back-pressure
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
    bool ok = DecodeRawData(path, raw_data, decoded);

    queued_bytes_.fetch_sub(raw_data.size(), std::memory_order_acq_rel);
    inflight_decodes_.fetch_sub(1, std::memory_order_acq_rel);

    if (!ok) {
        printf("[TextureServer] DECODE FAILED: '%s' (%u raw bytes)\n",
               path.c_str(), static_cast<unsigned>(raw_data.size()));
        fflush(stdout);
        decode_failures_.fetch_add(1, std::memory_order_relaxed);
        TexProto::Response resp{};
        resp.status = TexProto::Status::DecodeFail;
        SendResponse(pipe, resp);
        return;
    }

    printf("[TextureServer] Decoded: '%s' -> %ux%u, %u bytes, fmt=%u\n",
           path.c_str(), decoded.width, decoded.height,
           static_cast<unsigned>(decoded.pixels.size()), decoded.format);
    fflush(stdout);

    // 5. Place in shared memory.
    int32_t slot = PlaceInSharedMemory(decoded, path);
    if (slot < 0) {
        printf("[TextureServer] No free slot for '%s'\n", path.c_str());
        fflush(stdout);
        TexProto::Response resp{};
        resp.status = TexProto::Status::NoSlot;
        SendResponse(pipe, resp);
        return;
    }

    printf("[TextureServer] Placed in slot %d\n", slot);
    fflush(stdout);

    // 6. Cache the decoded texture for future requests.
    TexProto::Response resp{};
    resp.status     = TexProto::Status::Ok;
    resp.slot_id    = static_cast<uint16_t>(slot);
    resp.width      = decoded.width;
    resp.height     = decoded.height;
    resp.data_size  = static_cast<uint32_t>(decoded.pixels.size());
    resp.format     = decoded.format;
    resp.mip_levels = decoded.mip_levels;

    // Cache a copy before we send the response (Put takes by value/move).
    cache_.Put(path, std::move(decoded));

    SendResponse(pipe, resp);
}

// ── Query ──────────────────────────────────────────────────────────────────

void Server::HandleQuery(HANDLE pipe, const std::string& path) {
    const DecodedTexture* cached = cache_.Get(path);

    TexProto::Response resp{};
    if (cached) {
        resp.status     = TexProto::Status::Cached;
        resp.width      = cached->width;
        resp.height     = cached->height;
        resp.data_size  = static_cast<uint32_t>(cached->pixels.size());
        resp.format     = cached->format;
        resp.mip_levels = cached->mip_levels;
    } else {
        resp.status = TexProto::Status::NotCached;
    }
    SendResponse(pipe, resp);
}

// ── Stats ──────────────────────────────────────────────────────────────────

void Server::HandleStats(HANDLE pipe) {
    TexProto::Response resp{};
    resp.status    = TexProto::Status::Ok;
    // Repurpose numeric fields to convey stats:
    //   width      = total requests (low 32 bits)
    //   height     = cache entries
    //   data_size  = cache bytes (MiB, truncated to 32 bits)
    //   mip_levels = inflight decodes (clamped to 255)
    resp.width      = static_cast<uint32_t>(total_requests_.load(std::memory_order_relaxed));
    resp.height     = static_cast<uint32_t>(cache_.EntryCount());
    resp.data_size  = static_cast<uint32_t>(cache_.CurrentBytes() / (1024 * 1024));
    resp.mip_levels = static_cast<uint8_t>(
        std::min<uint32_t>(inflight_decodes_.load(std::memory_order_relaxed), 255u));
    SendResponse(pipe, resp);
}

// ── Decode dispatch ────────────────────────────────────────────────────────

bool Server::DecodeRawData(const std::string& path,
                           const std::vector<uint8_t>& raw_data,
                           DecodedTexture& result)
{
    if (raw_data.empty())
        return false;

    if (EndsWith(path, ".blp")) {
        return blp_decoder_.Decode(raw_data.data(), raw_data.size(), result);
    }

    if (EndsWith(path, ".tga")) {
        return tga_decoder_.Decode(raw_data.data(), raw_data.size(), result);
    }

    // Unknown format -- try BLP first (most WoW textures are BLP).
    if (blp_decoder_.Decode(raw_data.data(), raw_data.size(), result))
        return true;

    return tga_decoder_.Decode(raw_data.data(), raw_data.size(), result);
}

// ── Shared memory placement ────────────────────────────────────────────────

int32_t Server::PlaceInSharedMemory(const DecodedTexture& tex,
                                    const std::string& path)
{
    // Check that the decoded data fits in a slot.
    if (tex.pixels.size() > TexProto::SLOT_DATA_SIZE)
        return -1;

    // Allocate a free slot (CAS: Empty -> Reading).
    int32_t slot = shm_.AllocateSlot();
    if (slot < 0)
        return -1;

    // Fill the slot header.
    auto* sh = shm_.GetSlotHeader(slot);
    sh->width      = tex.width;
    sh->height     = tex.height;
    sh->data_size  = static_cast<uint32_t>(tex.pixels.size());
    sh->format     = tex.format;
    sh->mip_levels = tex.mip_levels;
    sh->path_hash  = TexProto::HashPath(path.c_str());

    // Copy pixel data.
    uint8_t* dst = shm_.GetSlotData(slot);
    std::memcpy(dst, tex.pixels.data(), tex.pixels.size());

    // Transition: Reading -> Ready (via MarkSlotReady).
    shm_.MarkSlotReady(slot);

    return slot;
}

// ── Pipe I/O helpers ───────────────────────────────────────────────────────

bool Server::SendResponse(HANDLE pipe, const TexProto::Response& resp) {
    DWORD written = 0;
    return WriteFile(pipe, &resp, sizeof(resp), &written, nullptr)
        && written == sizeof(resp);
}

bool Server::ReadPipe(HANDLE pipe, void* buf, DWORD size) {
    DWORD total_read = 0;
    auto* p = static_cast<uint8_t*>(buf);

    while (total_read < size) {
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(pipe, p + total_read, size - total_read,
                           &bytes_read, nullptr);
        if (!ok || bytes_read == 0)
            return false;
        total_read += bytes_read;
    }
    return true;
}

} // namespace TexServer
