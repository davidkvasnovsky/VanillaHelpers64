#pragma once
// WorkingSet.h - LRU-based GPU texture working set for the 32-bit client.
// Tracks which textures are currently uploaded to D3D9 and evicts the oldest
// when the memory budget is exceeded.

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace TexClient {

/// Callback invoked when a texture is evicted from the working set.
/// @param gpu_handle  Opaque handle (e.g. IDirect3DTexture9*) to release.
/// @param user_data   User-provided context pointer from WorkingSetConfig.
using ReleaseCallback = void (*)(void* gpu_handle, void* user_data);

struct WorkingSetConfig {
    uint32_t budget_mb = 512;
    ReleaseCallback release_fn = nullptr;
    void* release_user_data = nullptr;
};

class WorkingSet {
public:
    explicit WorkingSet(const WorkingSetConfig& config);

    /// Start tracking a texture.  Returns false if already tracked.
    bool Track(const std::string& path, void* gpu_handle, uint32_t size_bytes);

    /// Mark a texture as recently used (move to front of LRU).
    void Touch(const std::string& path);

    /// Evict oldest entries until current usage fits within the budget.
    void EvictToFit();

    /// Explicitly evict a single texture by path. Returns true if found.
    bool Evict(const std::string& path);

    /// Current memory usage in MiB.
    uint32_t CurrentMB() const;

    /// Number of tracked textures.
    uint32_t Count() const;

private:
    struct Entry {
        std::string path;
        void* gpu_handle;
        uint32_t size_bytes;
    };

    WorkingSetConfig config_;
    std::list<Entry> order_; // front = MRU, back = LRU
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    size_t current_bytes_ = 0;
    mutable std::mutex mutex_;

    void EvictOne(); // Pop the LRU entry (back) and invoke release callback
};

} // namespace TexClient
