// How fast this machine can read memory, measured without a language model.
//
// This exists to settle one number. design/voice.md argues that on-device
// decode is bandwidth-bound: a model of size S cannot exceed bandwidth/S tokens
// per second, so nothing but a smaller model helps. The constant in that
// argument, 19.5 GB/s, was only ever derived from llama.cpp's own throughput --
// and then MNN's thinker was measured moving 27.6 GB/s on the same device,
// which is either a faster engine or a wrong constant.
//
// A language model cannot answer that, because every figure from one is that
// engine's arithmetic and cache behaviour as much as the chip's. This reads a
// large buffer and divides. If it lands near 19.5 the wall is the bus and the
// conclusions stand; if it lands well above, the wall was llama.cpp's and
// "no other compute unit moves it" was tested only against engines with the
// same access pattern.
//
// **Sequential, not strided**, which is deliberate. Decode streams whole weight
// tensors; a strided read would measure the TLB and the prefetcher failing,
// which is a different chip property and not the one in the argument.

#pragma once

#include <cstddef>
#include <string>

namespace bodyclock {
namespace platform {

struct MemoryBandwidth {
    int    threads  = 0;
    size_t bytes    = 0;   // buffer size, per pass
    int    passes   = 0;
    double seconds  = 0.0;
    double gb_per_s = 0.0; // DECIMAL GB, because 19.5 is quoted in those
};

// Read `mib` MiB `passes` times, split across `threads` workers, and time it.
//
//   threads  <= 0 means one. The caller sweeps this; nothing here guesses.
//   mib      <= 0 selects 256, which is far past any phone's last-level cache
//            and small enough to allocate beside a resident model.
//   passes   <= 0 selects 4. More than one because the buffer is faulted in and
//            written before timing starts, and a single pass over a freshly
//            touched buffer measures the page cache warming as much as the bus.
//
// BLOCKS. Returns false only if the buffer could not be allocated.
bool measure_memory_bandwidth(int threads, int mib, int passes, MemoryBandwidth* out);

}  // namespace platform
}  // namespace bodyclock
