// What erase_handles does to build time: a chain of DEPTH `then`s whose
// functions may throw, so that every level is a place where two outcomes
// meet (in VARIANT mode a choice, in TYPED mode an erased handle), with an
// erase_handles every FIREWALL levels (0 for none). Compiled once per point
// by build_time.sh, which times the compiler; run, it checks the answer.
#include "function.hpp"

#include <cstdio>
#include <stdexcept>

#ifndef DEPTH
    #define DEPTH 16
#endif
#ifndef FIREWALL
    #define FIREWALL 0
#endif

using namespace th;

struct inc {
    int operator()(int v) const {
        if (v < 0) {
            throw std::runtime_error("negative");
        }
        return v + 1;
    }
};

template <int D, class S>
auto chain(S&& s) {
    if constexpr (D == 0) {
        return std::forward<S>(s);
    } else if constexpr (FIREWALL > 0 && D % (FIREWALL > 0 ? FIREWALL : 1) == 0) {
        return chain<D - 1>(erase_handles(then(std::forward<S>(s), inc{})));
    } else {
        return chain<D - 1>(then(std::forward<S>(s), inc{}));
    }
}

volatile int sink;

[[gnu::noinline]] void run_chain(int x) {
    run_root(chain<DEPTH>(just(x)), [](auto& st) { sink = st.template get<set_value_t(int)>(); });
}

int main(int argc, char**) {
    long n = argc > 1 ? 3000 : 1000; // two counts, for Callgrind's difference
    for (long i = 0; i < n; ++i) {
        run_chain(0);
    }
    if (sink != DEPTH) {
        std::printf("wrong answer: %d\n", int(sink));
        return 1;
    }
    return 0;
}
