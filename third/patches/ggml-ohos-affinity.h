/* Thread affinity in ggml on OHOS, without patching ggml.

   examples/harmony/build.py force-includes this (-include) into every C file
   it compiles from llama.cpp's ggml, in the llama and qtts lanes alike. It
   replaced llama-ggml-ohos-affinity.patch: a diff fits the lines around it,
   and so roughly one commit; this fits any ggml that still uses the names
   below, and leaves the checkout as upstream wrote it.

   ggml picks its affinity code by platform, and OHOS falls into the __linux__
   branch. Two things there do not hold on this sysroot:

     cpu_set_t, CPU_ZERO, CPU_SET   the libc has them behind _GNU_SOURCE. ggml's
                                    CMakeLists defines that on Linux and Android
                                    only, and _XOPEN_SOURCE=600 everywhere, which
                                    hides them. Defined here, for every file, as
                                    ggml itself has it on Linux.
     pthread_setaffinity_np         no header declares it, and the libc.so a
                                    device loads does not export it -- libc.a
                                    does, which is no help to a shared library.
                                    sched_setaffinity is in both, and ggml
                                    already calls it on Android, the other
                                    platform with this gap.

   sched_setaffinity(0, ...) pins the calling thread, and ggml makes this call
   on pthread_self(), so the two agree. The macro yields 0 or an errno value,
   as the pthread function returns. The system headers come first, so a libc
   that does declare the function keeps its declaration intact.

   What affinity is worth on this device has not been measured; the thread
   count sweep found 4/6/8 threads within 2%. This is "do not silently drop
   upstream's behavior", not "known to be faster". */

#if defined(__OHOS__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>

#define pthread_setaffinity_np(thread, size, set) \
    (sched_setaffinity(0, (size), (set)) == 0 ? 0 : errno)

#endif
