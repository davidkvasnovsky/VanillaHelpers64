// WorkingSet.cpp - LRU working set implementation.

#include "WorkingSet.h"
#include <algorithm>

namespace TexClient {

WorkingSet::WorkingSet(const WorkingSetConfig& config)
    : config_(config)
{}

bool WorkingSet::Track(const std::string& path, void* gpu_handle, uint32_t size_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Reject duplicates
    if (map_.find(path) != map_.end())
        return false;

    // Push to front (most recently used)
    order_.push_front(Entry{ path, gpu_handle, size_bytes });
    map_[path] = order_.begin();
    current_bytes_ += size_bytes;

    return true;
}

void WorkingSet::Touch(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(path);
    if (it == map_.end())
        return;

    // Splice the entry to the front (MRU position) without copy/alloc
    order_.splice(order_.begin(), order_, it->second);
}

void WorkingSet::EvictToFit() {
    std::lock_guard<std::mutex> lock(mutex_);

    const size_t budget = static_cast<size_t>(config_.budget_mb) * 1024u * 1024u;

    while (current_bytes_ > budget && !order_.empty()) {
        EvictOne();
    }
}

bool WorkingSet::Evict(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(path);
    if (it == map_.end())
        return false;

    auto list_it = it->second;

    // Invoke release callback so the caller can free the D3D9 resource
    if (config_.release_fn)
        config_.release_fn(list_it->gpu_handle, config_.release_user_data);

    current_bytes_ -= list_it->size_bytes;
    order_.erase(list_it);
    map_.erase(it);

    return true;
}

uint32_t WorkingSet::CurrentMB() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(current_bytes_ / (1024u * 1024u));
}

uint32_t WorkingSet::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(map_.size());
}

// ── Private ─────────────────────────────────────────────────────────────

void WorkingSet::EvictOne() {
    // Caller must hold mutex_.  Evicts the LRU entry (back of list).
    if (order_.empty())
        return;

    auto& entry = order_.back();

    if (config_.release_fn)
        config_.release_fn(entry.gpu_handle, config_.release_user_data);

    current_bytes_ -= entry.size_bytes;
    map_.erase(entry.path);
    order_.pop_back();
}

} // namespace TexClient
