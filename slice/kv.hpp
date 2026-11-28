// The slice's bodies: kv_store's spine, each function hand-lowered as a
// compiler would lower its direct-style body, behind the stand-in
// `function`. The direct-style source of each is in its comment.
//
// Lowering rules, the same everywhere (D4471's, as settled):
// - An awaited call's `function` is a temporary that lives to the end of the
//   full-expression; its operation state is destroyed when it completes,
//   before the body carries on.
// - A local that is returned (`auto v = co_await f(); ... co_return v;`) is
//   constructed at the caller's destination, and its address is asked for
//   once, when it comes into existence.
// - `co_return co_await f(...)` passes the callee's completions through: the
//   callee is told the caller's destinations.
// - A `catch` handler for a typed error gives that error storage of the
//   frame's own: the callee's error is filtered there.
// - Parameters are the callee's: a body may move from them. They live in the
//   caller's `function` (the factory borrows them).
#pragma once
#include "function.hpp"
#include "sched.hpp"

#include <map>
#include <optional>
#include <string>

// ---- an instrumented string, to count copies and moves --------------------
#ifdef SLICE_COUNT
struct str {
    std::string s;
    // counted separately for keys ("k...") and values (everything else)
    inline static long key_copies, key_moves, val_copies, val_moves;
    static bool        is_key(const str& o) { return !o.s.empty() && o.s[0] == 'k'; }
    str() = default;
    str(const char* p) : s(p) {}
    str(const str& o) : s(o.s) { ++(is_key(o) ? key_copies : val_copies); }
    str(str&& o) noexcept : s(std::move(o.s)) { ++(is_key(*this) ? key_moves : val_moves); }
    str& operator=(const str& o) {
        ++(is_key(o) ? key_copies : val_copies);
        s = o.s;
        return *this;
    }
    str& operator=(str&& o) noexcept {
        s = std::move(o.s);
        ++(is_key(*this) ? key_moves : val_moves);
        return *this;
    }
    friend bool operator<(const str& a, const str& b) { return a.s < b.s; }
    friend bool operator==(const str& a, const str& b) { return a.s == b.s; }
    static void reset() { key_copies = key_moves = val_copies = val_moves = 0; }
};
#else
using str = std::string;
#endif

namespace kv {

using namespace th;

// ---- what the functions complete with -----------------------------------
struct not_found {};
struct timed_out {
    int after_ms;
};
struct corrupt {
    int code;
};
// A value that can't be moved: it must be constructed where it ends up.
struct pinned {
    int v;
    explicit pinned(int x) noexcept : v(x) {}
    pinned(pinned&&) = delete;
};

using value_t = std::optional<str>;

using get_sigs    = completion_signatures<set_value_t(value_t)>;
using fetch_sigs  = completion_signatures<set_value_t(str), set_error_t(not_found)>;
using fetch5_sigs = completion_signatures<set_value_t(str),
                                          set_error_t(not_found),
                                          set_error_t(timed_out),
                                          set_error_t(corrupt),
                                          set_stopped_t()>;
using str_sigs    = completion_signatures<set_value_t(str)>;
using pinned_sigs = completion_signatures<set_value_t(pinned), set_error_t(not_found)>;
using pinned_value_sigs = completion_signatures<set_value_t(pinned)>;

using get_fn          = function<get_sigs, str>;
using fetch_fn        = function<fetch_sigs, str>;
using fetch5_fn       = function<fetch5_sigs, str>;
using get_or_fn       = function<str_sigs, str, str>;
using pinned_fn       = function<pinned_sigs, str>;
using pinned_value_fn = function<pinned_value_sigs, str>;

// ---- the interfaces ---------------------------------------------------------
struct kv_store {
    virtual get_fn   get(str key) const   = 0;
    virtual fetch_fn fetch(str key) const = 0;
    virtual ~kv_store()                   = default;
};
struct kv5_store {
    virtual fetch5_fn fetch(str key) const = 0;
    virtual ~kv5_store()                   = default;
};
struct defaulting {
    virtual get_or_fn get_or(str key, str fallback) const = 0;
    virtual ~defaulting()                                 = default;
};
struct pinned_store {
    virtual pinned_fn fetch(str key) const = 0;
    virtual ~pinned_store()                = default;
};
struct pinned_defaulting {
    virtual pinned_value_fn get_or(str key) const = 0;
    virtual ~pinned_defaulting()                  = default;
};

// ---- deferred work, standing in for an event loop -----------------------
// One thread; no lock. Whoever drains it is the bottom of the machine stack.
struct local_queue {
    task*  head = nullptr;
    task** tail = &head;
    void   push(task* t) noexcept {
        t->next = nullptr;
        *tail   = t;
        tail    = &t->next;
    }
    void drain() noexcept {
        while (task* t = head) {
            head = t->next;
            if (!head) {
                tail = &head;
            }
            erased_handle work = t->work;
            th::run(work);
        }
    }
};
inline local_queue g_loop;

// ---- an awaited call ---------------------------------------------------------
// The temporary `function` and the operation connected to it. Both end when
// the call completes (reset()), before the body carries on.
template <class Fn, class Rcvr>
struct await_slot {
    slot<Fn>                                call;
    slot<connect_result_t<Fn&, Rcvr>>       op;

    template <class Make>
    TH_INLINE auto& connect(Make make, Rcvr r) noexcept {
        call.emplace_from(make);
        return op.emplace_from([&] { return th::connect(*call, std::move(r)); });
    }
    // At the start of a body: the call is the body's first transfer of
    // control, so starting it is a plain call.
    template <class Make>
    TH_INLINE auto start(Make make, Rcvr r) noexcept {
        return th::start(connect(make, std::move(r)));
    }
    // From inside a completion: deferred, as let_value starts its second.
    template <class Make>
    TH_INLINE auto start_later(Make make, Rcvr r) noexcept {
        return th::start_later(connect(make, std::move(r)));
    }
    TH_INLINE void reset() noexcept {
        op.reset();
        call.reset();
    }
};

// A receiver for an awaited call inside the operation Op: destinations and
// completions are Op's business (Op::dest<Which, Sig>, Op::on<Which, Sig>).
template <class Op, int Which>
struct await_rcvr : operation_handle<await_rcvr<Op, Which>, Op> {
    using base_t = operation_handle<await_rcvr, Op>;
    using base_t::base_t;

    template <class Sig>
    TH_INLINE void* destination() noexcept {
        return this->op()->template dest<Which, Sig>();
    }
    TH_INLINE auto get_env() const noexcept { return th::get_env(this->op()->rcvr); }

  private:
    friend base_t;
    template <class Sig>
    TH_INLINE static auto resume(Op& self) noexcept {
        return self.template on<Which, Sig>();
    }
};

// A sender whose operation is Body<R>{rcvr, args...}: how each hand-lowered
// body is returned by its factory. Args are pointers into the callee's object
// and the caller's curried arguments.
template <template <class> class Body, class Sigs, class... Args>
struct body_sender {
    std::tuple<Args...> args;
    template <class Env>
    static consteval auto get_completion_signatures() {
        return Sigs();
    }
    template <class R>
    Body<R> connect(R rcvr) && noexcept {
        return std::apply(
            [&](Args... as) { return Body<R>(std::move(rcvr), as...); }, args);
    }
};

// ============================================================================
// The backing store: a leaf. `get` never fails; `fetch` fails with not_found.
// Inline, or from the event loop.
// ============================================================================
class mem_store final : public kv_store {
  public:
    std::map<str, str> data;
    bool               deferred = false;

    const str* lookup(const str& key) const {
        auto it = data.find(key);
        return it == data.end() ? nullptr : &it->second;
    }
    get_fn   get(str key) const override;
    fetch_fn fetch(str key) const override;
};

template <bool Fetch>
struct mem_body {
    template <class R>
    struct op : task {
        R                rcvr;
        const mem_store* self;
        str*             key;

        op(R r, const mem_store* s, str* k) noexcept : rcvr(std::move(r)), self(s), key(k) {}
        op(op&&) = delete;

        // value_t get(str key) const { co_return find(key); }
        // str fetch(str key) const {
        //   if (auto* v = lookup(key)) co_return *v;
        //   throw not_found{};       // the typed error
        // }
        TH_INLINE auto now() noexcept {
            const str* v = self->lookup(*key);
            if constexpr (!Fetch) {
                using sig = set_value_t(value_t);
                if (v) {
                    ::new (destination_of<R, sig>(rcvr)) value_t(*v);
                } else {
                    ::new (destination_of<R, sig>(rcvr)) value_t();
                }
                return later<sig>(rcvr);
            } else {
                return either(
                    v != nullptr,
                    [&] {
                        ::new (destination_of<R, set_value_t(str)>(rcvr)) str(*v);
                        return later<set_value_t(str)>(rcvr);
                    },
                    [&] {
                        ::new (destination_of<R, set_error_t(not_found)>(rcvr)) not_found{};
                        return later<set_error_t(not_found)>(rcvr);
                    });
            }
        }
        static erased_handle execute(void* p) noexcept { return settle(static_cast<op*>(p)->now()); }

        TH_INLINE auto start() & noexcept {
            return either(
                self->deferred,
                [&] {
                    this->work = erased_handle(&execute, static_cast<void*>(this));
                    g_loop.push(this);
                    return no_operation{};
                },
                [&] { return now(); });
        }
    };
};
template <class R>
using mem_get_op = typename mem_body<false>::template op<R>;
template <class R>
using mem_fetch_op = typename mem_body<true>::template op<R>;

// ============================================================================
// The cache.
//
//   value_t get(str key) const {
//     if (auto hit = entries_.find(key)) co_return hit;
//     auto value = co_await backing_->get(key);
//     if (value) entries_.insert(key, *value);
//     co_return value;
//   }
//   str fetch(str key) const { co_return co_await backing_->fetch(std::move(key)); }
// ============================================================================
class cache final : public kv_store {
  public:
    explicit cache(const kv_store* b, bool retain = true) : backing(b), retain_(retain) {}
    const kv_store*            backing;
    mutable std::map<str, str> entries; // a cache changes under a const interface

    value_t find(const str& key) const {
        auto it = entries.find(key);
        return it == entries.end() ? value_t() : value_t(it->second);
    }
    void insert(const str& key, const str& value) const {
        if (retain_) { // false keeps every lookup a miss, for the miss path
            entries.insert_or_assign(key, value);
        }
    }
    get_fn   get(str key) const override;
    fetch_fn fetch(str key) const override;

  private:
    bool retain_;
};

template <class R>
struct cache_get_op {
    using vsig = set_value_t(value_t);
    using ar   = await_rcvr<cache_get_op, 0>;

    R                      rcvr;
    const cache*           self;
    str*                   key;
    value_t*               value = nullptr; // the returned local, at the caller's destination
    await_slot<get_fn, ar> call;

    cache_get_op(R r, const cache* s, str* k) noexcept : rcvr(std::move(r)), self(s), key(k) {}
    cache_get_op(cache_get_op&&) = delete;

    TH_INLINE auto start() & noexcept {
        auto* hit = ::new (destination_of<R, vsig>(rcvr)) value_t(self->find(*key));
        return either(
            hit->has_value(),
            [&] { return later<vsig>(rcvr); },
            [&] {
                std::destroy_at(hit); // `hit`'s scope ends
                value = static_cast<value_t*>(destination_of<R, vsig>(rcvr));
                return call.start([&] { return self->backing->get(*key); }, ar(this));
            });
    }

    template <int, class Sig>
    TH_INLINE void* dest() noexcept {
        return value;
    }
    template <int, class Sig>
    TH_INLINE auto on() noexcept {
        call.reset();
        if (*value) {
            self->insert(*key, **value);
        }
        return later<vsig>(rcvr);
    }
};

// A layer that passes everything through: `co_return co_await inner(...)`.
template <class Fn, class Iface, auto Member, class R>
struct pass_op {
    using ar = await_rcvr<pass_op, 0>;

    R                  rcvr;
    const Iface*       inner;
    str*               key;
    await_slot<Fn, ar> call;

    pass_op(R r, const Iface* i, str* k) noexcept : rcvr(std::move(r)), inner(i), key(k) {}
    pass_op(pass_op&&) = delete;

    TH_INLINE auto start() & noexcept {
        return call.start([&] { return (inner->*Member)(std::move(*key)); }, ar(this));
    }
    template <int, class Sig>
    TH_INLINE void* dest() noexcept {
        return destination_of<R, Sig>(rcvr);
    }
    template <int, class Sig>
    TH_INLINE auto on() noexcept {
        call.reset();
        return later<Sig>(rcvr);
    }
};

// ============================================================================
// A pass-through layer, for chains of them.
//
//   str fetch(str key) const { ++calls_; co_return co_await inner_->fetch(std::move(key)); }
//   value_t get(str key) const { ++calls_; co_return co_await inner_->get(std::move(key)); }
// ============================================================================
class pass_store final : public kv_store {
  public:
    explicit pass_store(const kv_store* i) : inner(i) {}
    const kv_store* inner;
    mutable long    calls = 0;
    get_fn          get(str key) const override;
    fetch_fn        fetch(str key) const override;
};

// ============================================================================
// Two tiers: fall back to a second backend on a typed error (fetch), or on
// an empty answer (get, a middle that filters a value).
//
//   str fetch(str key) const {
//     try { co_return co_await primary_->fetch(key); }
//     catch (not_found) { co_return co_await secondary_->fetch(std::move(key)); }
//   }
//   value_t get(str key) const {
//     if (auto v = co_await primary_->get(key)) co_return v;
//     co_return co_await secondary_->get(std::move(key));
//   }
// ============================================================================
class two_tier final : public kv_store {
  public:
    two_tier(const kv_store* p, const kv_store* s) : primary(p), secondary(s) {}
    const kv_store* primary;
    const kv_store* secondary;
    get_fn          get(str key) const override;
    fetch_fn        fetch(str key) const override;
};

template <class R>
struct tier_fetch_op {
    using a1 = await_rcvr<tier_fetch_op, 0>;
    using a2 = await_rcvr<tier_fetch_op, 1>;

    R                        rcvr;
    const two_tier*          self;
    str*                     key;
    uninit<not_found>        caught; // the handler's error
    await_slot<fetch_fn, a1> first;
    await_slot<fetch_fn, a2> second;

    tier_fetch_op(R r, const two_tier* s, str* k) noexcept : rcvr(std::move(r)), self(s), key(k) {}
    tier_fetch_op(tier_fetch_op&&) = delete;

    TH_INLINE auto start() & noexcept {
        return first.start([&] { return self->primary->fetch(*key); }, a1(this));
    }
    template <int W, class Sig>
    TH_INLINE void* dest() noexcept {
        if constexpr (W == 0 && same_as<Sig, set_error_t(not_found)>) {
            return &caught.value;
        } else {
            return destination_of<R, Sig>(rcvr);
        }
    }
    template <int W, class Sig>
    TH_INLINE auto on() noexcept {
        if constexpr (W == 0) {
            first.reset();
            if constexpr (same_as<Sig, set_error_t(not_found)>) {
                std::destroy_at(&caught.value);
                return second.start_later([&] { return self->secondary->fetch(std::move(*key)); },
                                          a2(this));
            } else {
                return later<Sig>(rcvr);
            }
        } else {
            second.reset();
            return later<Sig>(rcvr);
        }
    }
};

template <class R>
struct tier_get_op {
    using vsig = set_value_t(value_t);
    using a1   = await_rcvr<tier_get_op, 0>;
    using a2   = await_rcvr<tier_get_op, 1>;

    R                      rcvr;
    const two_tier*        self;
    str*                   key;
    value_t*               v = nullptr; // the returned local
    await_slot<get_fn, a1> first;
    await_slot<get_fn, a2> second;

    tier_get_op(R r, const two_tier* s, str* k) noexcept : rcvr(std::move(r)), self(s), key(k) {}
    tier_get_op(tier_get_op&&) = delete;

    TH_INLINE auto start() & noexcept {
        v = static_cast<value_t*>(destination_of<R, vsig>(rcvr));
        return first.start([&] { return self->primary->get(*key); }, a1(this));
    }
    template <int W, class Sig>
    TH_INLINE void* dest() noexcept {
        if constexpr (W == 0) {
            return v;
        } else {
            return destination_of<R, Sig>(rcvr);
        }
    }
    template <int W, class Sig>
    TH_INLINE auto on() noexcept {
        if constexpr (W == 0) {
            first.reset();
            return either(
                v->has_value(),
                [&] { return later<vsig>(rcvr); },
                [&] {
                    std::destroy_at(v); // `v`'s scope ends
                    return second.start_later(
                        [&] { return self->secondary->get(std::move(*key)); }, a2(this));
                });
        } else {
            second.reset();
            return later<Sig>(rcvr);
        }
    }
};

// ============================================================================
// A default for a typed error: the error is filtered, the value passes
// through.
//
//   str get_or(str key, str fallback) const {
//     try { co_return co_await backing_->fetch(std::move(key)); }
//     catch (not_found) { co_return std::move(fallback); }
//   }
// ============================================================================
class with_default final : public defaulting {
  public:
    explicit with_default(const kv_store* b) : backing(b) {}
    const kv_store* backing;
    get_or_fn       get_or(str key, str fallback) const override;
};

template <class R>
struct get_or_op {
    using vsig = set_value_t(str);
    using ar   = await_rcvr<get_or_op, 0>;

    R                        rcvr;
    const with_default*      self;
    str*                     key;
    str*                     fallback;
    uninit<not_found>        caught;
    await_slot<fetch_fn, ar> call;

    get_or_op(R r, const with_default* s, str* k, str* f) noexcept
        : rcvr(std::move(r)), self(s), key(k), fallback(f) {}
    get_or_op(get_or_op&&) = delete;

    TH_INLINE auto start() & noexcept {
        return call.start([&] { return self->backing->fetch(std::move(*key)); }, ar(this));
    }
    template <int, class Sig>
    TH_INLINE void* dest() noexcept {
        if constexpr (same_as<Sig, set_error_t(not_found)>) {
            return &caught.value;
        } else {
            return destination_of<R, Sig>(rcvr);
        }
    }
    template <int, class Sig>
    TH_INLINE auto on() noexcept {
        call.reset();
        if constexpr (same_as<Sig, set_error_t(not_found)>) {
            std::destroy_at(&caught.value);
            ::new (destination_of<R, vsig>(rcvr)) str(std::move(*fallback));
        }
        return later<vsig>(rcvr);
    }
};

// ============================================================================
// Five completions: a leaf that can fail three ways or stop, and a layer
// that passes them all through.
// ============================================================================
class mem5_store final : public kv5_store {
  public:
    std::map<str, str> data;
    fetch5_fn          fetch(str key) const override;
};

template <class R>
struct mem5_op {
    R                 rcvr;
    const mem5_store* self;
    str*              key;
    mem5_op(R r, const mem5_store* s, str* k) noexcept : rcvr(std::move(r)), self(s), key(k) {}
    mem5_op(mem5_op&&) = delete;

    // A copy that throws ends the program: the function declares no
    // exception_ptr error, as none of the slice's functions do.
    template <class Sig, class... Xs>
    TH_INLINE auto finish(Xs&&... xs) noexcept {
        if constexpr (has_payload<Sig>) {
            construct_payload<Sig>(destination_of<R, Sig>(rcvr), std::forward<Xs>(xs)...);
        }
        return later<Sig>(rcvr);
    }
    // str fetch(str key) const {
    //   switch (classify(key)) {
    //     case timeout: throw timed_out{5};  case bad: throw corrupt{1};
    //     case stop: co_yield stopped;       case absent: throw not_found{};
    //     default: co_return data_.at(key);
    //   }
    // }
    TH_INLINE auto start() & noexcept {
        auto it   = self->data.find(*key);
        int  kind = it != self->data.end() ? 0
                    : *key == str("k-timeout") ? 1
                    : *key == str("k-corrupt") ? 2
                    : *key == str("k-stop")    ? 3
                                               : 4;
        return by_index<5>(kind, [&]<std::size_t I>() {
            if constexpr (I == 0) {
                return finish<set_value_t(str)>(it->second);
            } else if constexpr (I == 1) {
                return finish<set_error_t(timed_out)>(timed_out{5});
            } else if constexpr (I == 2) {
                return finish<set_error_t(corrupt)>(corrupt{1});
            } else if constexpr (I == 3) {
                return finish<set_stopped_t()>();
            } else {
                return finish<set_error_t(not_found)>(not_found{});
            }
        });
    }
};

class pass5_store final : public kv5_store {
  public:
    explicit pass5_store(const kv5_store* i) : inner(i) {}
    const kv5_store* inner;
    fetch5_fn        fetch(str key) const override;
};

// ============================================================================
// An immovable value through a middle that filters.
//
//   pinned fetch(str key) const;              // pinned(42), or not_found
//   pinned get_or(str key) const {
//     try { co_return co_await backing_->fetch(std::move(key)); }
//     catch (not_found) { co_return pinned(7); }
//   }
// ============================================================================
class pinned_mem final : public pinned_store {
  public:
    pinned_fn fetch(str key) const override;
};
class pinned_default final : public pinned_defaulting {
  public:
    explicit pinned_default(const pinned_store* b) : backing(b) {}
    const pinned_store* backing;
    pinned_value_fn     get_or(str key) const override;
};

template <class R>
struct pinned_fetch_op {
    R    rcvr;
    str* key;
    pinned_fetch_op(R r, const pinned_mem*, str* k) noexcept : rcvr(std::move(r)), key(k) {}
    pinned_fetch_op(pinned_fetch_op&&) = delete;
    TH_INLINE auto start() & noexcept {
        return either(
            *key == str("k1"),
            [&] {
                ::new (destination_of<R, set_value_t(pinned)>(rcvr)) pinned(42);
                return later<set_value_t(pinned)>(rcvr);
            },
            [&] {
                ::new (destination_of<R, set_error_t(not_found)>(rcvr)) not_found{};
                return later<set_error_t(not_found)>(rcvr);
            });
    }
};

template <class R>
struct pinned_get_or_op {
    using vsig = set_value_t(pinned);
    using ar   = await_rcvr<pinned_get_or_op, 0>;
    R                         rcvr;
    const pinned_default*     self;
    str*                      key;
    uninit<not_found>         caught;
    await_slot<pinned_fn, ar> call;
    pinned_get_or_op(R r, const pinned_default* s, str* k) noexcept
        : rcvr(std::move(r)), self(s), key(k) {}
    pinned_get_or_op(pinned_get_or_op&&) = delete;

    TH_INLINE auto start() & noexcept {
        return call.start([&] { return self->backing->fetch(std::move(*key)); }, ar(this));
    }
    template <int, class Sig>
    TH_INLINE void* dest() noexcept {
        if constexpr (same_as<Sig, set_error_t(not_found)>) {
            return &caught.value;
        } else {
            return destination_of<R, Sig>(rcvr);
        }
    }
    template <int, class Sig>
    TH_INLINE auto on() noexcept {
        call.reset();
        if constexpr (same_as<Sig, set_error_t(not_found)>) {
            std::destroy_at(&caught.value);
            ::new (destination_of<R, vsig>(rcvr)) pinned(7);
        }
        return later<vsig>(rcvr);
    }
};

template <class R>
using cache_fetch_op = pass_op<fetch_fn, kv_store, &kv_store::fetch, R>;
template <class R>
using pass_fetch_op = pass_op<fetch_fn, kv_store, &kv_store::fetch, R>;
template <class R>
using pass_get_op = pass_op<get_fn, kv_store, &kv_store::get, R>;
template <class R>
using pass5_op = pass_op<fetch5_fn, kv5_store, &kv5_store::fetch, R>;

// ============================================================================
// The root: a strand on the machine stack, and the root's state.
// ============================================================================
struct root_env {
    strand*            sp;
    TH_INLINE strand* query(get_frame_allocator_t) const noexcept { return sp; }
};

template <class Sigs>
struct slice_root_rcvr : operation_handle<slice_root_rcvr<Sigs>, root_state<Sigs>> {
    using state_t = root_state<Sigs>;
    using base_t  = operation_handle<slice_root_rcvr, state_t>;
    strand* sp;

    slice_root_rcvr(state_t* s, strand* p) noexcept : base_t(s), sp(p) {}
    slice_root_rcvr(slice_root_rcvr&&) noexcept = default;
    slice_root_rcvr(const slice_root_rcvr&)     = delete;

    TH_INLINE root_env get_env() const noexcept { return {sp}; }
    template <class Sig>
    TH_INLINE void* destination() noexcept {
        return this->op()->storage.address();
    }

  private:
    friend base_t;
    template <class Sig>
    TH_INLINE static no_operation resume(state_t& self) noexcept {
        self.which = int(index_in<Sig, typename state_t::list>);
        return {};
    }
};

// Connect s to a root, run it and the event loop until nothing is left,
// then call use(state).
template <class S, class Use>
void run_slice(S&& s, Use use) {
    alignas(std::max_align_t) std::byte stack[4096];
    strand                              sp(stack, sizeof stack);
    using Sigs = completions_of_t<S, root_env>;
    root_state<Sigs> st;
    {
        auto op = th::connect(std::forward<S>(s), slice_root_rcvr<Sigs>{&st, &sp});
        th::run(th::start(op));
        g_loop.drain();
    }
    use(st);
}

} // namespace kv
