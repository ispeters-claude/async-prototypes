// Scenarios to count instructions in, one build per configuration.
//
//   bench SCENARIO N     run SCENARIO N times
//   bench list           print the scenarios' names
//   bench time N         nanoseconds per call of every scenario: the best of
//                        seven runs of N calls
//
// cg.sh runs each twice under Callgrind and reports the difference per run.
// The scenarios are scenarios.hpp's; here each run keeps only which
// completion arrived, so that the root's use of the result costs little.
#include "scenarios.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace scn;

volatile int sink;

template <class S>
TH_INLINE void bench_async(S&& s) {
    run_slice(std::forward<S>(s), [](auto& st) { sink = st.which; });
}
template <class T>
TH_INLINE void bench_sync(T&& r) {
    if constexpr (requires { bool(r); }) {
        sink = bool(r);
    } else {
        sink = int(r.size());
    }
}
#define BENCH_A(expr) bench_async(expr)
#define BENCH_S(expr) bench_sync(expr)

#define X(name, expect, kind, expr) \
    [[gnu::noinline]] void name(world& w) { BENCH_##kind(expr); }
SLICE_SCENARIOS(X)
#undef X

struct entry {
    const char* name;
    void (*run)(world&);
};
#define X(name, expect, kind, expr) {#name, &name},
static const entry entries[] = {SLICE_SCENARIOS(X)};
#undef X

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "list") == 0) {
        for (const entry& e : entries) {
            std::printf("%s\n", e.name);
        }
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "time") == 0) {
        world w;
        long  n = std::atol(argv[2]);
        for (const entry& e : entries) {
            double best = 1e30;
            for (int r = 0; r < 7; ++r) {
                auto t0 = std::chrono::steady_clock::now();
                for (long i = 0; i < n; ++i) {
                    e.run(w);
                }
                auto t1 = std::chrono::steady_clock::now();
                double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(n);
                best      = ns < best ? ns : best;
            }
            std::printf("%-26s %7.2f\n", e.name, best);
        }
        return 0;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: bench SCENARIO N | bench list\n");
        return 2;
    }
    world w;
    long  n = std::atol(argv[2]);
    for (const entry& e : entries) {
        if (std::strcmp(e.name, argv[1]) == 0) {
            for (long i = 0; i < n; ++i) {
                e.run(w);
            }
            return 0;
        }
    }
    std::fprintf(stderr, "no scenario %s\n", argv[1]);
    return 2;
}
