// Every scenario once: its outcome checked, and what it cost in copies and
// moves, frame storage and heap allocations. Built with SLICE_COUNT, under
// ASan and UBSan where the compiler has them.
#include "scenarios.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

static long g_heap = 0;
void* operator new(std::size_t n) {
    ++g_heap;
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace scn;

struct counts {
    long kc, km, vc, vm, heap, slabs;
    std::size_t frames;
};
static counts snapshot() {
#ifdef SLICE_COUNT
    return {str::key_copies, str::key_moves, str::val_copies, str::val_moves,
            g_heap, th::g_slab_allocs, th::g_frame_high};
#else
    return {0, 0, 0, 0, g_heap, 0, 0};
#endif
}
static void reset() {
#ifdef SLICE_COUNT
    str::reset();
    th::g_slab_allocs = 0;
    th::g_frame_high  = 0;
#endif
    g_heap = 0;
}

template <class S>
static void measure_async(S&& s) {
    run_slice(std::forward<S>(s), [](auto&) {});
}
template <class T>
static void measure_sync(T&&) {}

#define MEASURE_A(expr) measure_async(expr)
#define MEASURE_S(expr) measure_sync(expr)
#define CALL_A(expr) call(expr)
#define CALL_S(expr) text(expr)

static const char* mode_name() {
    static const char* modes[] = {"EAGER", "ERASED", "TYPED", "VARIANT"};
    static const char* dests[] = {"CHAIN", "ONCE", "RECORD", "OUT"};
    static const char* allocs[] = {"connect", "start"};
    static char        buf[64];
    std::snprintf(buf, sizeof buf, "%s %s alloc-at-%s", modes[TH_MODE], dests[SLICE_DEST],
                  allocs[SLICE_ALLOC]);
    return buf;
}

int main() {
    world w;
    int   failed = 0;
    std::printf("%s\n", mode_name());
    std::printf("%-26s %-6s %9s %9s %7s %6s %5s\n", "scenario", "ok", "key c/m", "val c/m",
                "frames", "slabs", "heap");
#define X(name, expect, kind, expr)                                                     \
    {                                                                                   \
        std::string got = CALL_##kind(expr);                                            \
        bool        ok  = got == expect;                                                \
        if (!ok) {                                                                      \
            ++failed;                                                                   \
            std::printf("FAIL %s: got %s, expected %s\n", #name, got.c_str(), expect); \
        }                                                                               \
        reset();                                                                        \
        MEASURE_##kind(expr);                                                           \
        counts c = snapshot();                                                          \
        std::printf("%-26s %-6s %4ld/%-4ld %4ld/%-4ld %6zuB %6ld %5ld\n", #name,         \
                    ok ? "yes" : "NO", c.kc, c.km, c.vc, c.vm, c.frames, c.slabs,       \
                    c.heap);                                                            \
    }
    SLICE_SCENARIOS(X)
#undef X
#ifdef SLICE_COUNT
    if (th::g_frame_bytes != 0) {
        std::printf("FAIL: %zu bytes of frames left on the strand\n", th::g_frame_bytes);
        ++failed;
    }
#endif
    std::printf(failed ? "%d FAILED\n" : "all pass\n", failed);
    return failed ? 1 : 0;
}
