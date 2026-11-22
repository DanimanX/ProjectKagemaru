// Portable Linux pthread core-pinning helper for 4-core ARM64 handhelds
// (Allwinner H700: Core 0 main, Core 1 emu, Core 2 sound, Core 3 audio/aux).
#pragma once

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace arm64 {

inline int cpu_count() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? int(n) : 1;
}

inline int pin_current_thread(int core) {
    if (core < 0) return -1;
    const int n = cpu_count();
    if (n <= 1 || core >= n) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}

inline void set_current_thread_name(const char *name) {
#if defined(__linux__)
    if (name) ::pthread_setname_np(::pthread_self(), name);
#else
    (void)name;
#endif
}

} // namespace arm64
