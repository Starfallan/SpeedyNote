#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>

// MuPDF allocator callbacks for opt-in viewport providers. Counts all live
// allocations (including our aligned headers); the temporary limit is used
// only while recording a display list, never to shrink the raster pixel cap.
class PdfCacheAllocator {
    struct alignas(std::max_align_t) Header { std::size_t bytes; };
    std::atomic<std::size_t> m_bytes{0};
    std::atomic<std::size_t> m_limit{std::numeric_limits<std::size_t>::max()};
    std::atomic<bool> m_rejected{false};

    bool reserve(std::size_t amount) noexcept {
        auto current = m_bytes.load(std::memory_order_relaxed);
        const auto limit = m_limit.load(std::memory_order_relaxed);
        for (;;) {
            if (current > limit || amount > limit - current) {
                m_rejected.store(true, std::memory_order_relaxed);
                return false;
            }
            if (m_bytes.compare_exchange_weak(current, current + amount, std::memory_order_relaxed))
                return true;
        }
    }

public:
    std::size_t bytes() const noexcept { return m_bytes.load(std::memory_order_relaxed); }
    bool rejected() const noexcept { return m_rejected.load(std::memory_order_relaxed); }
    void limit(std::size_t bytes) noexcept {
        m_rejected.store(false, std::memory_order_relaxed);
        m_limit.store(bytes, std::memory_order_relaxed);
    }
    void unlimit() noexcept { m_limit.store(std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed); }

    static void* allocate(void* user, std::size_t size) noexcept {
        auto& state = *static_cast<PdfCacheAllocator*>(user);
        if (size > std::numeric_limits<std::size_t>::max() - sizeof(Header)) {
            state.m_rejected.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        const auto amount = sizeof(Header) + size;
        if (!state.reserve(amount)) return nullptr;
        auto* header = static_cast<Header*>(std::malloc(amount));
        if (!header) {
            state.m_bytes.fetch_sub(amount, std::memory_order_relaxed);
            state.m_rejected.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        header->bytes = amount;
        return header + 1;
    }
    static void release(void* user, void* ptr) noexcept {
        if (!ptr) return;
        auto* header = static_cast<Header*>(ptr) - 1;
        static_cast<PdfCacheAllocator*>(user)->m_bytes.fetch_sub(header->bytes, std::memory_order_relaxed);
        std::free(header);
    }
    static void* reallocate(void* user, void* ptr, std::size_t size) noexcept {
        if (!ptr) return allocate(user, size);
        if (!size) { release(user, ptr); return nullptr; }
        auto& state = *static_cast<PdfCacheAllocator*>(user);
        if (size > std::numeric_limits<std::size_t>::max() - sizeof(Header)) {
            state.m_rejected.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        auto* oldHeader = static_cast<Header*>(ptr) - 1;
        const auto before = oldHeader->bytes;
        const auto after = sizeof(Header) + size;
        if (after > before && !state.reserve(after - before)) return nullptr;
        auto* header = static_cast<Header*>(std::realloc(oldHeader, after));
        if (!header) {
            if (after > before) state.m_bytes.fetch_sub(after - before, std::memory_order_relaxed);
            state.m_rejected.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        if (after < before) state.m_bytes.fetch_sub(before - after, std::memory_order_relaxed);
        header->bytes = after;
        return header + 1;
    }
};
