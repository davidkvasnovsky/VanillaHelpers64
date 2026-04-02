#pragma once
// LruCache.h - Thread-safe LRU cache for decoded textures.
// Part of the TextureServer64 server component.

#include "BlpDecoder.h"  // for DecodedTexture
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace TexServer {

class LruCache {
public:
    explicit LruCache(size_t max_bytes = 4ULL * 1024 * 1024 * 1024);

    /// Look up a decoded texture by normalized path.
    /// Returns pointer to cached texture (valid until next Put/eviction), or nullptr.
    /// Moves the entry to MRU position on hit.
    const DecodedTexture* Get(const std::string& path);

    /// Insert (or replace) a decoded texture in the cache.
    /// Evicts LRU entries if the cache exceeds the byte budget.
    void Put(const std::string& path, DecodedTexture tex);

    /// Current memory usage in bytes (approximate: counts pixel data only).
    size_t CurrentBytes() const;

    /// Number of entries in the cache.
    size_t EntryCount() const;

private:
    struct Entry {
        std::string    path;
        DecodedTexture texture;
    };

    void EvictToFit(size_t needed);

    mutable std::mutex mutex_;
    std::list<Entry> order_;  // front = MRU, back = LRU
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    size_t max_bytes_;
    size_t current_bytes_ = 0;
};

// ── Inline implementation ──────────────────────────────────────────────────

inline LruCache::LruCache(size_t max_bytes)
    : max_bytes_(max_bytes)
{}

inline const DecodedTexture* LruCache::Get(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(path);
    if (it == map_.end())
        return nullptr;

    // Move to front (MRU).
    order_.splice(order_.begin(), order_, it->second);
    return &it->second->texture;
}

inline void LruCache::Put(const std::string& path, DecodedTexture tex) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t tex_bytes = tex.pixels.size();

    // If the entry already exists, remove it first.
    auto existing = map_.find(path);
    if (existing != map_.end()) {
        current_bytes_ -= existing->second->texture.pixels.size();
        order_.erase(existing->second);
        map_.erase(existing);
    }

    // Don't cache textures larger than the entire budget.
    if (tex_bytes > max_bytes_)
        return;

    // Evict LRU entries until we have room.
    EvictToFit(tex_bytes);

    // Insert at front (MRU).
    order_.push_front(Entry{path, std::move(tex)});
    map_[path] = order_.begin();
    current_bytes_ += tex_bytes;
}

inline size_t LruCache::CurrentBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bytes_;
}

inline size_t LruCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

inline void LruCache::EvictToFit(size_t needed) {
    // Called with mutex_ held.
    while (current_bytes_ + needed > max_bytes_ && !order_.empty()) {
        auto& lru = order_.back();
        current_bytes_ -= lru.texture.pixels.size();
        map_.erase(lru.path);
        order_.pop_back();
    }
}

} // namespace TexServer
