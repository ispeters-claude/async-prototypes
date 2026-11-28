// The workloads, shared by checks.cpp and bench.cpp. Each scenario makes one
// call from a root and returns what came back, as a string: the value, or
// "error:<type>" or "stopped".
#pragma once
#include "kv.hpp"
#include "sync.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace scn {

using namespace kv;

inline std::string text(const str& s) {
#ifdef SLICE_COUNT
    return s.s;
#else
    return s;
#endif
}

// Turn whatever completion arrived at a root into a string.
template <class State>
std::string outcome(State& st) {
    std::string r = "(nothing)";
    using list    = typename State::list;
    for_each_index<size_of<list>>([&]<std::size_t I>() {
        using Sig = nth_t<I, list>;
        if (st.which != int(I)) {
            return;
        }
        if constexpr (same_as<Sig, set_value_t(str)>) {
            r = text(st.template get<Sig>());
        } else if constexpr (same_as<Sig, set_value_t(value_t)>) {
            auto& v = st.template get<Sig>();
            r       = v ? text(*v) : "(empty)";
        } else if constexpr (same_as<Sig, set_value_t(pinned)>) {
            r = "pinned:" + std::to_string(st.template get<Sig>().v);
        } else if constexpr (same_as<Sig, set_value_t(int)>) {
            r = std::to_string(st.template get<Sig>());
        } else if constexpr (same_as<Sig, set_error_t(not_found)>) {
            r = "error:not_found";
        } else if constexpr (same_as<Sig, set_error_t(timed_out)>) {
            r = "error:timed_out:" + std::to_string(st.template get<Sig>().after_ms);
        } else if constexpr (same_as<Sig, set_error_t(corrupt)>) {
            r = "error:corrupt:" + std::to_string(st.template get<Sig>().code);
        } else if constexpr (same_as<Sig, set_stopped_t()>) {
            r = "stopped";
        } else {
            r = "(unexpected completion)";
        }
    });
    return r;
}

template <class S>
std::string call(S&& s) {
    std::string r;
    run_slice(std::forward<S>(s), [&](auto& st) { r = outcome(st); });
    return r;
}

inline std::string text(const yard::result& r) {
    return r ? text(*r) : std::string("error:not_found");
}
inline std::string text(const value_t& v) { return v ? text(*v) : std::string("(empty)"); }

// Everything the scenarios call, built once.
struct world {
    mem_store mem, mem_deferred, mem_b;
    cache     warm{&mem}, cold{&mem, false}, cold_deferred{&mem_deferred, false};
    two_tier  tier{&mem, &mem_b}, tier_deferred{&mem_deferred, &mem_b};
    with_default             dflt{&mem};
    std::vector<pass_store*> chain; // chain[i] is i + 1 layers over mem
    mem5_store               five;
    pass5_store              five1{&five}, five2{&five1};
    pinned_mem               pins;
    pinned_default           pin_dflt{&pins};
    std::vector<pass_store*> chain_deferred;

    yard::mem_store  ymem, ymem_b;
    yard::cache      ywarm{&ymem}, ycold{&ymem, false};
    yard::two_tier   ytier{&ymem, &ymem_b};
    yard::with_default ydflt{&ymem};
    std::vector<yard::pass_store*> ychain;

    world() {
        mem.data["k1"] = mem_deferred.data["k1"] = "value-one";
        mem_b.data["k1"]                         = "other-one";
        mem_b.data["k2"]                         = "value-two";
        mem_deferred.deferred                    = true;
        five.data["k1"]                          = "value-five";
        ymem.data["k1"]                          = "value-one";
        ymem_b.data["k1"]                        = "other-one";
        ymem_b.data["k2"]                        = "value-two";
        const kv_store*       below              = &mem;
        const kv_store*       below_d            = &mem_deferred;
        const yard::kv_store* ybelow             = &ymem;
        for (int i = 0; i < 8; ++i) {
            chain.push_back(new pass_store(below));
            below = chain.back();
            chain_deferred.push_back(new pass_store(below_d));
            below_d = chain_deferred.back();
            ychain.push_back(new yard::pass_store(ybelow));
            ybelow = ychain.back();
        }
        // warm the caches
        call(warm.get(str("k1")));
        ywarm.get(str("k1"));
    }
    world(const world&) = delete;
    ~world() {
        for (auto* p : chain) {
            delete p;
        }
        for (auto* p : chain_deferred) {
            delete p;
        }
        for (auto* p : ychain) {
            delete p;
        }
    }
};

// A static middle above an erased call: upon_error composed with the caller.
inline auto or_else(world& w, const char* key) {
    return upon_error(w.mem.fetch(str(key)), [](not_found) noexcept { return str("fallback"); });
}

// The scenarios, as an X-macro: X(name, expected outcome, A or S, expr),
// where expr is the call (A: a sender, run from a root; S: a synchronous
// call), written in terms of a world w.
// clang-format off
#define SLICE_SCENARIOS(X) \
    /* the root alone, to subtract */ \
    X(root_just,          "7",               A, just(7)) \
    /* the cache */ \
    X(get_hit,            "value-one",       A, w.warm.get(str("k1"))) \
    X(get_miss,           "value-one",       A, w.cold.get(str("k1"))) \
    X(get_miss_absent,    "(empty)",         A, w.cold.get(str("k9"))) \
    X(get_miss_deferred,  "value-one",       A, w.cold_deferred.get(str("k1"))) \
    /* a default for a typed error, erased and composed */ \
    X(get_or_value,       "value-one",       A, w.dflt.get_or(str("k1"), str("fallback"))) \
    X(get_or_error,       "fallback",        A, w.dflt.get_or(str("k9"), str("fallback"))) \
    X(or_else_value,      "value-one",       A, or_else(w, "k1")) \
    X(or_else_error,      "fallback",        A, or_else(w, "k9")) \
    /* two tiers */ \
    X(tier_primary,       "value-one",       A, w.tier.fetch(str("k1"))) \
    X(tier_secondary,     "value-two",       A, w.tier.fetch(str("k2"))) \
    X(tier_error,         "error:not_found", A, w.tier.fetch(str("k9"))) \
    X(tier_get_secondary, "value-two",       A, w.tier.get(str("k2"))) \
    X(tier_secondary_deferred, "value-two",  A, w.tier_deferred.fetch(str("k2"))) \
    /* chains of pass-through layers */ \
    X(pass1_value,        "value-one",       A, w.chain[0]->fetch(str("k1"))) \
    X(pass2_value,        "value-one",       A, w.chain[1]->fetch(str("k1"))) \
    X(pass4_value,        "value-one",       A, w.chain[3]->fetch(str("k1"))) \
    X(pass8_value,        "value-one",       A, w.chain[7]->fetch(str("k1"))) \
    X(pass1_error,        "error:not_found", A, w.chain[0]->fetch(str("k9"))) \
    X(pass8_error,        "error:not_found", A, w.chain[7]->fetch(str("k9"))) \
    X(pass8_get,          "value-one",       A, w.chain[7]->get(str("k1"))) \
    X(pass4_deferred,     "value-one",       A, w.chain_deferred[3]->fetch(str("k1"))) \
    /* five completions, through two layers */ \
    X(five_value,         "value-five",      A, w.five2.fetch(str("k1"))) \
    X(five_not_found,     "error:not_found", A, w.five2.fetch(str("k9"))) \
    X(five_timeout,       "error:timed_out:5", A, w.five2.fetch(str("k-timeout"))) \
    X(five_corrupt,       "error:corrupt:1", A, w.five2.fetch(str("k-corrupt"))) \
    X(five_stopped,       "stopped",         A, w.five2.fetch(str("k-stop"))) \
    /* an immovable value through a filtering middle */ \
    X(pinned_value,       "pinned:42",       A, w.pin_dflt.get_or(str("k1"))) \
    X(pinned_error,       "pinned:7",        A, w.pin_dflt.get_or(str("k9"))) \
    /* the synchronous yardstick */ \
    X(sync_get_hit,       "value-one",       S, w.ywarm.get(str("k1"))) \
    X(sync_get_miss,      "value-one",       S, w.ycold.get(str("k1"))) \
    X(sync_get_or_value,  "value-one",       S, w.ydflt.get_or(str("k1"), str("fallback"))) \
    X(sync_get_or_error,  "fallback",        S, w.ydflt.get_or(str("k9"), str("fallback"))) \
    X(sync_tier_primary,  "value-one",       S, w.ytier.fetch(str("k1"))) \
    X(sync_tier_secondary,"value-two",       S, w.ytier.fetch(str("k2"))) \
    X(sync_tier_error,    "error:not_found", S, w.ytier.fetch(str("k9"))) \
    X(sync_tier_get_secondary, "value-two",  S, w.ytier.get(str("k2"))) \
    X(sync_pass1_value,   "value-one",       S, w.ychain[0]->fetch(str("k1"))) \
    X(sync_pass2_value,   "value-one",       S, w.ychain[1]->fetch(str("k1"))) \
    X(sync_pass4_value,   "value-one",       S, w.ychain[3]->fetch(str("k1"))) \
    X(sync_pass8_value,   "value-one",       S, w.ychain[7]->fetch(str("k1"))) \
    X(sync_pass8_error,   "error:not_found", S, w.ychain[7]->fetch(str("k9")))
// clang-format on

} // namespace scn
