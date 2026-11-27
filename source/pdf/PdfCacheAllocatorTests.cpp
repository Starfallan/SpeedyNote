// Standalone C++17 checks; no Qt/MuPDF dependencies and no CI upload gate.
#include "PdfCacheAllocator.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    PdfCacheAllocator state;
    void* data = PdfCacheAllocator::allocate(&state, 64);
    check(data != nullptr, "initial allocation");
    if (!data) return 1;
    check(reinterpret_cast<std::uintptr_t>(data) % alignof(std::max_align_t) == 0, "payload alignment");
    std::memset(data, 0x5a, 64);
    const auto original = state.bytes();
    check(original >= 64, "headers counted");
    state.limit(original);
    check(!PdfCacheAllocator::allocate(&state, 1) && state.rejected(), "limit rejects another allocation");
    check(!PdfCacheAllocator::reallocate(&state, data, 65), "limited realloc fails");
    check(state.bytes() == original && static_cast<unsigned char*>(data)[63] == 0x5a,
          "failed realloc preserves bytes and data");
    state.unlimit();
    void* larger = PdfCacheAllocator::reallocate(&state, data, 128);
    check(larger != nullptr, "realloc grows");
    if (!larger) { PdfCacheAllocator::release(&state, data); return 1; }
    data = larger;
    check(state.bytes() == original + 64 && static_cast<unsigned char*>(data)[63] == 0x5a, "growth preserves data/accounting");
    void* smaller = PdfCacheAllocator::reallocate(&state, data, 16);
    check(smaller != nullptr, "realloc shrinks");
    if (!smaller) { PdfCacheAllocator::release(&state, data); return 1; }
    data = smaller;
    check(state.bytes() == original - 48 && static_cast<unsigned char*>(data)[15] == 0x5a, "shrink returns capacity");
    const auto remaining = state.bytes();
    check(!PdfCacheAllocator::reallocate(&state, data, std::numeric_limits<std::size_t>::max()), "realloc overflow rejected");
    check(state.bytes() == remaining, "overflow preserves old allocation");
    check(!PdfCacheAllocator::reallocate(&state, data, 0) && state.bytes() == 0, "zero realloc frees");
    check(!PdfCacheAllocator::allocate(&state, std::numeric_limits<std::size_t>::max()) && state.bytes() == 0,
          "allocation overflow rejected");
    PdfCacheAllocator::release(&state, nullptr);
    state.limit(4096);
    std::atomic<bool> exceeded{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int n = 0; n < 2000; ++n) {
            void* p = PdfCacheAllocator::reallocate(&state, nullptr, 1024);
            if (state.bytes() > 4096) exceeded = true;
            PdfCacheAllocator::release(&state, p);
        }
    });
    for (auto& worker : workers) worker.join();
    check(!exceeded && state.bytes() == 0, "concurrent reservations obey budget and all free");
    std::cout << "PdfCacheAllocator: " << failures << " failures\n";
    return failures ? 1 : 0;
}
