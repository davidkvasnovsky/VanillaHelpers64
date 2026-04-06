#pragma once
// WinHandle.h - RAII wrappers for Win32 HANDLE and MapViewOfFile pointers.
// Server-side only.  The DLL side intentionally avoids RAII for module-level
// handles because DLL_PROCESS_DETACH cleanup order is fragile.

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <cstdint>
#include <utility> // std::exchange
#include <windows.h>

namespace TexServer {

// RAII wrapper for HANDLE objects closed with CloseHandle().
// Sentinel is the "null" value: nullptr for most, INVALID_HANDLE_VALUE for files/pipes.
template <HANDLE Sentinel = nullptr>
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(UniqueHandle&& o) noexcept : h_(std::exchange(o.h_, Sentinel)) {}
    auto operator=(UniqueHandle&& o) noexcept -> UniqueHandle& {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, Sentinel);
        }
        return *this;
    }

    UniqueHandle(const UniqueHandle&) = delete;
    auto operator=(const UniqueHandle&) -> UniqueHandle& = delete;

    [[nodiscard]] auto get() const noexcept -> HANDLE { return h_; }
    explicit operator bool() const noexcept { return h_ != Sentinel; }

    auto release() noexcept -> HANDLE { return std::exchange(h_, Sentinel); }

    void reset(HANDLE h = Sentinel) noexcept {
        if (h_ != Sentinel) {
            CloseHandle(h_);
        }
        h_ = h;
    }

private:
    HANDLE h_ = Sentinel;
};

// RAII wrapper for memory-mapped views (UnmapViewOfFile on destruction).
class UniqueMapView {
public:
    UniqueMapView() noexcept = default;
    explicit UniqueMapView(void* p) noexcept : p_(static_cast<uint8_t*>(p)) {}
    ~UniqueMapView() noexcept { reset(); }

    UniqueMapView(UniqueMapView&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    auto operator=(UniqueMapView&& o) noexcept -> UniqueMapView& {
        if (this != &o) {
            reset();
            p_ = std::exchange(o.p_, nullptr);
        }
        return *this;
    }

    UniqueMapView(const UniqueMapView&) = delete;
    auto operator=(const UniqueMapView&) -> UniqueMapView& = delete;

    [[nodiscard]] auto get() const noexcept -> uint8_t* { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    auto release() noexcept -> uint8_t* { return std::exchange(p_, nullptr); }

    void reset(void* p = nullptr) noexcept {
        if (p_ != nullptr) {
            UnmapViewOfFile(p_);
        }
        p_ = static_cast<uint8_t*>(p);
    }

private:
    uint8_t* p_ = nullptr;
};

} // namespace TexServer
