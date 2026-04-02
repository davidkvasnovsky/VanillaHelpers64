#pragma once
// Server.h - Main server class for TextureServer64.
// Manages the named pipe accept loop, request dispatch, decode pipeline,
// LRU cache, and shared memory placement.

#include "BlpDecoder.h"
#include "LruCache.h"
#include "SharedMemory.h"
#include "TgaDecoder.h"
#include "ThreadPool.h"
#include "../../shared/Protocol.h"

#include <atomic>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace TexServer {

struct ServerConfig {
    uint32_t thread_count    = 0;   // 0 = auto (hardware_concurrency)
    size_t   cache_max_bytes = 0;   // 0 = auto-size from physical RAM
};

class Server {
public:
    explicit Server(const ServerConfig& config);
    ~Server();

    // Non-copyable.
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Initialize shared memory and thread pool. Returns false on failure.
    bool Start();

    /// Block in the named-pipe accept loop until Stop() is called.
    void Run();

    /// Signal the server to shut down (safe to call from any thread / signal).
    void Stop();

private:
    /// Handle a single pipe connection (runs on the accept thread).
    void HandleClient(HANDLE pipe);

    /// Process a Load/Prefetch request: decode raw_data and place in shared memory.
    void HandleLoad(HANDLE pipe, const std::string& path,
                    const std::vector<uint8_t>& raw_data, uint8_t priority);

    /// Process a Query request: check if a texture is in the LRU cache.
    void HandleQuery(HANDLE pipe, const std::string& path);

    /// Process a Stats request: return cache/pool statistics.
    void HandleStats(HANDLE pipe);

    /// Decode raw BLP or TGA data based on file extension.
    bool DecodeRawData(const std::string& path,
                       const std::vector<uint8_t>& raw_data,
                       DecodedTexture& result);

    /// Allocate a shared-memory slot, copy decoded pixels, fill slot header.
    /// Returns the slot index, or -1 on failure (no free slots, data too large).
    int32_t PlaceInSharedMemory(const DecodedTexture& tex,
                                const std::string& path);

    /// Send a Response struct over the pipe.
    static bool SendResponse(HANDLE pipe, const TexProto::Response& resp);

    /// Read exactly `size` bytes from the pipe into `buf`. Returns false on error/EOF.
    static bool ReadPipe(HANDLE pipe, void* buf, DWORD size);

    ServerConfig              config_;
    ThreadPool                pool_;
    SharedMemory              shm_;
    LruCache                  cache_;
    BlpDecoder                blp_decoder_;
    TgaDecoder                tga_decoder_;
    std::atomic<bool>         running_{false};
    std::atomic<uint32_t>     inflight_decodes_{0};
    std::atomic<uint64_t>     queued_bytes_{0};

    // Stats counters.
    std::atomic<uint64_t>     total_requests_{0};
    std::atomic<uint64_t>     cache_hits_{0};
    std::atomic<uint64_t>     decode_failures_{0};
};

} // namespace TexServer
