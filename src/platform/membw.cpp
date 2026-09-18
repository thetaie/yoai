#include "platform/membw.hpp"

#include "platform/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#endif

namespace bodyclock {
namespace platform {

namespace {

typedef std::chrono::steady_clock clk;

// Where the sums go to die. A read loop whose result is unused is a loop the
// compiler is entitled to delete, and at -O2 it will: the whole benchmark
// becomes a timer around nothing and reports an absurd number.
//
// Atomic rather than volatile, because several threads add to it and a
// volatile read-modify-write from more than one thread is a data race -- which
// is undefined behaviour even when the value is never read back. One relaxed
// add per thread, against tens of millions of loads.
std::atomic<uint64_t> g_sink(0);

// Chunks are claimed, not assigned, and that is the whole design.
//
// The first version of this gave each thread an equal slice of the buffer. On a
// phone that measures the slowest core: this SoC is one big, three mid and four
// little, the timer stops when the last thread finishes, and the fast cores sit
// idle waiting. It showed up as SIX threads being slower than four (15.3 against
// 17.9 GB/s, five runs each) -- with six, two little cores each carry a sixth of
// the buffer and everything waits for them. That number is not the machine's
// bandwidth, it is the bandwidth of an unlucky split.
//
// A shared counter fixes it: a fast core simply comes back for more. The chunk
// is 4 MiB, large enough that one atomic increment per chunk is nothing against
// half a million loads, small enough that the tail is short.
const size_t kChunkWords = (4u * 1024u * 1024u) / sizeof(uint64_t);

// FOUR accumulators, not one. A single running sum makes each add wait for the
// previous one, and that dependency chain, not the memory, becomes the limit --
// which would measure the ALU and report it as bandwidth. Four independent
// chains keep enough loads in flight for the memory system to be what runs out.
// `total` spans every pass, not one, so the threads are created once for the
// whole measurement rather than once per pass. Spawning is tens of microseconds
// against tens of milliseconds of reading -- negligible once, but four passes
// times eight threads inside the timer is several percent of the answer.
void read_chunks(const uint64_t* data, size_t words, size_t total,
                 std::atomic<size_t>* next) {
    uint64_t a = 0, b = 0, c = 0, d = 0;
    for (;;) {
        const size_t claimed = next->fetch_add(kChunkWords);
        if (claimed >= total) {
            break;
        }
        // Wrap to the start of the buffer for the second and later passes. The
        // caller guarantees words is a whole number of chunks, so this never
        // straddles the end and every claimed chunk is read in full.
        const size_t begin = claimed % words;
        const size_t end   = begin + kChunkWords;
        size_t i = begin;
        for (; i + 4 <= end; i += 4) {
            a += data[i];
            b += data[i + 1];
            c += data[i + 2];
            d += data[i + 3];
        }
        for (; i < end; ++i) {
            a += data[i];
        }
    }
    g_sink.fetch_add(a + b + c + d, std::memory_order_relaxed);
}

}  // namespace

bool measure_memory_bandwidth(int threads, int mib, int passes, MemoryBandwidth* out) {
    if (!out) {
        return false;
    }
    *out = MemoryBandwidth();

    if (threads <= 0) {
        threads = 1;
    }
    if (mib <= 0) {
        mib = 256;
    }
    if (passes <= 0) {
        passes = 4;
    }
    // Rounded UP to a whole number of 4 MiB chunks, which read_chunks relies on
    // to wrap between passes without straddling the end of the buffer. Rounded
    // rather than rejected, and the size actually used is reported back, so a
    // caller that asks for 100 is told it measured 100 rather than believing it.
    if (mib % 4 != 0) {
        mib += 4 - (mib % 4);
    }

    const size_t bytes = static_cast<size_t>(mib) * 1024u * 1024u;
    const size_t words = bytes / sizeof(uint64_t);

    // **Huge pages, asked for explicitly**, because the first version of this
    // measured the page tables as much as the memory.
    //
    // 256 MiB of ordinary 4 KiB pages is 65,536 translations, far past the few
    // thousand a phone's L2 TLB holds, so a streaming read takes a page walk
    // every 512 loads. The engines this is meant to be compared against do not
    // pay that: their weights arrive through a file mapping that the kernel can
    // back with 2 MiB pages, which is 512x fewer translations for the same
    // bytes. Comparing a malloc'd buffer against that measures the difference in
    // page size and calls it bandwidth.
    //
    // Aligned to 2 MiB because MADV_HUGEPAGE only collapses regions that are.
    // Advisory: if the kernel declines, the read still works and the number is
    // simply the small-page one, so the log says which was asked for.
    std::vector<uint64_t> fallback;
    uint64_t* data = 0;
    bool huge = false;

#if !defined(_WIN32)
    void* raw = 0;
    if (posix_memalign(&raw, 2u * 1024u * 1024u, bytes) == 0 && raw) {
        data = static_cast<uint64_t*>(raw);
#if defined(MADV_HUGEPAGE)
        huge = madvise(raw, bytes, MADV_HUGEPAGE) == 0;
#endif
    }
#endif

    if (!data) {
        try {
            fallback.resize(words);
        } catch (...) {
            log_error("membw: could not allocate %d MiB", mib);
            return false;
        }
        data = &fallback[0];
    }

    // Written before timing, and not with zeros from the allocator. A fresh
    // mapping is lazily faulted, so the first read of each page would be a page
    // fault rather than a memory access -- timing that measures the kernel.
    // Distinct values also mean a compiler that tried to constant-fold the sum
    // has nothing to fold.
    for (size_t i = 0; i < words; ++i) {
        data[i] = static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull;
    }

    // Timed around spawn and join together, once for the whole measurement.
    const size_t total = words * static_cast<size_t>(passes);
    const clk::time_point started = clk::now();
    {
        std::atomic<size_t> next(0);
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            workers.push_back(std::thread(read_chunks, data, words, total, &next));
        }
        for (size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
    }
    const double elapsed = std::chrono::duration<double>(clk::now() - started).count();

    out->threads = threads;
    out->bytes   = bytes;
    out->passes  = passes;
    out->seconds = elapsed;
    out->gb_per_s =
        elapsed > 0.0 ? (static_cast<double>(bytes) * passes) / elapsed / 1e9 : 0.0;

    log_info("membw: %d thread(s), %d MiB x %d passes in %.3f s = %.1f GB/s (%s pages)",
             threads, mib, passes, elapsed, out->gb_per_s,
             huge ? "huge" : "default");

#if !defined(_WIN32)
    if (fallback.empty() && data) {
        std::free(data);
    }
#endif
    return true;
}

}  // namespace platform
}  // namespace bodyclock
