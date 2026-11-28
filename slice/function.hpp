// A stand-in for P4223's `function`, with a hidden entry, frames on the async
// stack, and the result destinations of the asynchronous calling convention.
//
//   function<Sigs, Args...>
//
// is a sender that completes with one of the completion signatures Sigs and
// curries Args. It is constructed as P4223R2's `function` is,
//
//   function(obj, args..., factory)
//
// where factory(obj, args&...) returns the sender that does the work. The
// factory must be stateless and noexcept, and borrows the curried arguments:
// they stay in the `function`, which the caller keeps until the call has
// completed, so they are constructed once, in the caller (arguments in
// place). The constructor generates the callee's entry points from the
// factory's type (the hidden entry):
//
//   connect(frame, obj, args, k, sp)   construct the callee's operation in a
//                                      frame the caller pushed on the strand
//   start(frame)                       start it
//   enter(frame, obj, args, k, sp)     both, in one call
//   destroy(frame)                     destroy it, or null if there's nothing
//                                      to destroy
//
// with the frame's size and alignment. k is the caller's continuation
// record and sp the strand. The caller pushes the frame at connect or at
// start (W-161: SLICE_ALLOC), and destroys and pops it when the callee
// completes, before the caller's code runs (release at completion).
//
// The continuation record is one pointer to a static table with one resume
// function per declared signature, in Sigs' order; the stop token is
// forwarded through the table, and the strand is passed to the callee
// directly. Where each completion's payload goes is the part that differs
// between builds (SLICE_DEST; the rows of kv-dest's W-157 grid):
//
//   CHAIN   4A: the table has a destination function per signature; the
//           callee calls it when it constructs the payload, and the caller
//           answers then, asking its own receiver (a chain of erased
//           pass-through layers chains the calls)
//   ONCE    4A, the same table and the same callee code; the caller asks its
//           receiver once, when the callee is started, and its destination
//           function loads the answer
//   RECORD  3A: a pointer per signature in the record, filled at start; the
//           callee loads it
//   OUT     2A, which is 5A's erased half: a pointer per signature in a table
//           beside the record, passed to the callee as one more argument;
//           the callee loads it
//
// In statically composed code every build uses the member template
// destination<Sig>(), the half of 5A that every design shares.
#pragma once
#include "algos.hpp"
#include "stack.hpp"

#include <cstddef>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#define SLICE_DEST_CHAIN 0
#define SLICE_DEST_ONCE 1
#define SLICE_DEST_RECORD 2
#define SLICE_DEST_OUT 3
#ifndef SLICE_DEST
    #define SLICE_DEST SLICE_DEST_CHAIN
#endif

#define SLICE_ALLOC_CONNECT 0
#define SLICE_ALLOC_START 1
#ifndef SLICE_ALLOC
    #define SLICE_ALLOC SLICE_ALLOC_CONNECT
#endif

namespace th {

inline constexpr bool dest_by_function =
    SLICE_DEST == SLICE_DEST_CHAIN || SLICE_DEST == SLICE_DEST_ONCE;

// ---- The ABI: the record, its table, the entry points ----------------------
struct fn_table_base {
    inplace_stop_token (*stop_token)(void*) noexcept;
};
template <std::size_t N>
struct fn_table : fn_table_base {
    erased_handle (*resume[N])(void*) noexcept;
#if SLICE_DEST == SLICE_DEST_CHAIN || SLICE_DEST == SLICE_DEST_ONCE
    void* (*destination[N])(void*) noexcept; // null for no payload
#endif
};

template <std::size_t N>
struct fn_record {
    const fn_table_base* table;
#if SLICE_DEST == SLICE_DEST_RECORD
    void* dests[N];
#endif
    const fn_table<N>* tbl() const noexcept { return static_cast<const fn_table<N>*>(table); }
};

#if SLICE_DEST == SLICE_DEST_OUT
    #define SLICE_OUT_PARAM , void* const *out
    #define SLICE_OUT_ARG , out
    #define SLICE_OUT_ARG_OP , out
#else
    #define SLICE_OUT_ARG_OP
    #define SLICE_OUT_PARAM
    #define SLICE_OUT_ARG
#endif

template <std::size_t N, class... Args>
struct fn_entry {
    using args_t = std::tuple<Args...>;
    std::size_t size, align;
    void (*connect)(void* frame, const void* obj, args_t* args, fn_record<N>* k SLICE_OUT_PARAM,
                    strand* sp) noexcept;
    erased_handle (*start)(void* frame) noexcept;
    erased_handle (*enter)(void* frame, const void* obj, args_t* args,
                           fn_record<N>* k SLICE_OUT_PARAM, strand* sp) noexcept;
    void (*destroy)(void* frame) noexcept;
};

// ---- The callee's side ------------------------------------------------------
struct fn_env {
    const fn_table_base* const* k; // the record, which starts with its table
    strand*                     sp;
    TH_INLINE inplace_stop_token query(get_stop_token_t) const noexcept {
        return (*k)->stop_token(const_cast<const fn_table_base**>(k));
    }
    TH_INLINE strand* query(get_frame_allocator_t) const noexcept { return sp; }
};

// What the callee is connected to: the caller's record, the strand, and in
// the OUT build the destination table.
template <class Sigs>
struct fn_receiver {
    using list                     = as_list_t<Sigs>;
    static constexpr std::size_t n = size_of<list>;

    fn_record<n>* k;
#if SLICE_DEST == SLICE_DEST_OUT
    void* const* out;
#endif
    strand* sp;

    TH_INLINE fn_env get_env() const noexcept { return {&k->table, sp}; }

    template <class Sig>
    TH_INLINE void* destination() noexcept {
        constexpr std::size_t i = index_in<Sig, list>;
        static_assert(i < n, "the callee completes with a signature its function doesn't declare");
#if SLICE_DEST == SLICE_DEST_CHAIN || SLICE_DEST == SLICE_DEST_ONCE
        return k->tbl()->destination[i](k);
#elif SLICE_DEST == SLICE_DEST_RECORD
        return k->dests[i];
#else
        return out[i];
#endif
    }
    template <class Sig>
    TH_INLINE erased_handle complete() const noexcept {
        constexpr std::size_t i = index_in<Sig, list>;
        static_assert(i < n, "the callee completes with a signature its function doesn't declare");
        return {k->tbl()->resume[i], k}; // returned, not called
    }
};

template <class Sigs, class Sub>
inline constexpr bool sigs_within = [] {
    using big = as_list_t<Sigs>;
    return []<class... S>(type_list<S...>) {
        return ((index_in<S, big> < size_of<big>) && ...);
    }(as_list_t<Sub>());
}();

// The entry points for one factory, generated from its type.
template <class Sigs, class Obj, class Factory, class... Args>
struct fn_gen {
    static_assert(std::is_empty_v<Factory> && std::is_default_constructible_v<Factory>,
                  "the factory must be stateless");
    static_assert(std::is_nothrow_invocable_v<Factory, const Obj&, Args&...>,
                  "the factory must be noexcept");
    using rcvr_t                   = fn_receiver<Sigs>;
    static constexpr std::size_t n = rcvr_t::n;
    using sndr_t                   = std::invoke_result_t<Factory, const Obj&, Args&...>;
    using op_t                     = connect_result_t<sndr_t, rcvr_t>;
    using args_t                   = std::tuple<Args...>;
    static_assert(sigs_within<Sigs, completions_of_t<sndr_t, fn_env>>,
                  "the factory's sender completes in a way the function doesn't declare");
    static_assert(noexcept(th::connect(std::declval<sndr_t>(), std::declval<rcvr_t>())));

    static void connect(void* frame, const void* obj, args_t* args, fn_record<n>* k SLICE_OUT_PARAM,
                        strand* sp) noexcept {
        std::apply(
            [&](Args&... as) {
                ::new (frame) op_t(th::connect(Factory{}(*static_cast<const Obj*>(obj), as...),
                                               rcvr_t{k SLICE_OUT_ARG, sp}));
            },
            *args);
    }
    static erased_handle start(void* frame) noexcept {
        return settle(th::start(*static_cast<op_t*>(frame)));
    }
    static erased_handle enter(void* frame, const void* obj, args_t* args,
                               fn_record<n>* k SLICE_OUT_PARAM, strand* sp) noexcept {
        connect(frame, obj, args, k SLICE_OUT_ARG, sp);
        return start(frame);
    }
    static void destroy(void* frame) noexcept { std::destroy_at(static_cast<op_t*>(frame)); }

    static constexpr fn_entry<n, Args...> entry{
        sizeof(op_t),
        alignof(op_t),
        &connect,
        &start,
        &enter,
        std::is_trivially_destructible_v<op_t> ? nullptr : &destroy,
    };
};

// ---- The caller's side -----------------------------------------------------
template <class Sigs, class... Args>
class function;

template <class Fn, class R, bool Owns>
struct fn_op;

template <class Sigs, class... Args>
class function {
  public:
    using list                     = as_list_t<Sigs>;
    static constexpr std::size_t n = size_of<list>;
    using entry_t                  = fn_entry<n, Args...>;

    template <class Obj, class Factory>
    function(const Obj& obj, Args&&... args, Factory) noexcept(
        (std::is_nothrow_move_constructible_v<Args> && ...))
        : entry_(&fn_gen<Sigs, Obj, Factory, Args...>::entry),
          obj_(&obj),
          args_(std::move(args)...) {}

    function(function&&) = default;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return Sigs();
    }

    // Connecting an lvalue borrows the function, and with it the curried
    // arguments; the caller keeps it until the operation has completed.
    template <class R>
    fn_op<function, R, false> connect(R rcvr) & noexcept {
        return {*this, std::move(rcvr)};
    }
    // Connecting an rvalue moves it into the operation.
    template <class R>
    fn_op<function, R, true> connect(R rcvr) && noexcept {
        return {std::move(*this), std::move(rcvr)};
    }

  private:
    template <class, class, bool>
    friend struct fn_op;

    const entry_t*      entry_;
    const void*         obj_;
    std::tuple<Args...> args_;
};

template <class Fn, class R, bool Owns>
struct fn_op : fn_record<Fn::n> {
    static constexpr std::size_t n = Fn::n;
    using list                     = typename Fn::list;
    using record_t                 = fn_record<n>;

    using held_t = std::conditional_t<Owns, Fn, Fn*>;
    held_t fn;
    R      rcvr;
    void*  frame = nullptr;
#if SLICE_DEST == SLICE_DEST_ONCE
    void* saved[n];
#elif SLICE_DEST == SLICE_DEST_OUT
    void* out[n];
#endif

    Fn& f() noexcept {
        if constexpr (Owns) {
            return fn;
        } else {
            return *fn;
        }
    }
    strand* sp() noexcept { return frame_allocator_of(rcvr); }

    template <class F>
    fn_op(F&& f_, R r) noexcept : record_t{&table}, fn(hold(f_)), rcvr(std::move(r)) {
#if SLICE_ALLOC == SLICE_ALLOC_CONNECT
        auto* e = f().entry_;
        frame   = sp()->push(e->size, e->align);
        e->connect(frame, f().obj_, &f().args_, this SLICE_OUT_ARG_OP, sp());
#endif
    }
    fn_op(fn_op&&) = delete;
    ~fn_op() {
        if (frame) { // connected but not started, or not completed
            release();
        }
    }

    TH_INLINE erased_handle start() & noexcept {
        resolve();
        auto* e = f().entry_;
#if SLICE_ALLOC == SLICE_ALLOC_CONNECT
        return e->start(frame);
#else
        frame = sp()->push(e->size, e->align);
        return e->enter(frame, f().obj_, &f().args_, this SLICE_OUT_ARG_OP, sp());
#endif
    }

  private:
    template <class F>
    static held_t hold(F& f_) noexcept {
        if constexpr (Owns) {
            return std::move(f_);
        } else {
            return &f_;
        }
    }

    // Ask the receiver once for every payload's destination (ONCE, RECORD,
    // OUT). CHAIN asks when the callee does.
    TH_INLINE void resolve() noexcept {
#if SLICE_DEST != SLICE_DEST_CHAIN
        for_each_index<n>([&]<std::size_t I>() {
            using Sig = nth_t<I, list>;
            if constexpr (has_payload<Sig>) {
    #if SLICE_DEST == SLICE_DEST_ONCE
                saved[I] = destination_of<R, Sig>(rcvr);
    #elif SLICE_DEST == SLICE_DEST_RECORD
                this->dests[I] = destination_of<R, Sig>(rcvr);
    #else
                out[I] = destination_of<R, Sig>(rcvr);
    #endif
            }
        });
#endif
    }

    // Release at completion: the callee's operation is destroyed and its
    // frame popped before the caller's code runs.
    TH_INLINE void release() noexcept {
        auto* e = f().entry_;
        if (e->destroy) {
            e->destroy(frame);
        }
        sp()->pop(frame, e->size);
        frame = nullptr;
    }

    static fn_op* self_of(void* k) noexcept {
        return static_cast<fn_op*>(static_cast<record_t*>(k));
    }
    template <class Sig>
    static erased_handle resume(void* k) noexcept {
        fn_op* self = self_of(k);
        self->release();
        return settle(self->rcvr.template complete<Sig>());
    }
    template <class Sig>
    static void* dest(void* k) noexcept {
#if SLICE_DEST == SLICE_DEST_ONCE
        return self_of(k)->saved[index_in<Sig, list>];
#else
        return destination_of<R, Sig>(self_of(k)->rcvr);
#endif
    }
    static inplace_stop_token stop_token_of(void* k) noexcept {
        if constexpr (unstoppable<stop_token_of_t<R>>) {
            return {};
        } else {
            return get_stop_token(th::get_env(self_of(k)->rcvr));
        }
    }
    static constexpr fn_table<n> make_table() {
        fn_table<n> t{};
        t.stop_token = &stop_token_of;
        for_each_index<n>([&]<std::size_t I>() {
            using Sig   = nth_t<I, list>;
            t.resume[I] = &resume<Sig>;
#if SLICE_DEST == SLICE_DEST_CHAIN || SLICE_DEST == SLICE_DEST_ONCE
            if constexpr (has_payload<Sig>) {
                t.destination[I] = &dest<Sig>;
            }
#endif
        });
        return t;
    }
    static constexpr fn_table<n> table = make_table();
};

// ---- erase_handles: a lightweight firewall ----------------------------------
// Erases the handles that cross it and nothing else: the child's receiver
// forwards destinations and the environment to the parent's statically, but
// its completions, and the child's start, hand back erased handles. The
// child's types then stay out of the parent's, and the parent's out of the
// child's, at one indirect call per crossing.
template <class Child, class R>
struct erase_handles_op {
    struct rcvr_t : operation_handle<rcvr_t, erase_handles_op> {
        using base_t = operation_handle<rcvr_t, erase_handles_op>;
        using base_t::base_t;

        template <class Sig>
        TH_INLINE void* destination() noexcept {
            return destination_of<R, Sig>(this->op()->rcvr);
        }
        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;
        template <class Sig>
        TH_INLINE static erased_handle resume(erase_handles_op& self) noexcept {
            return erased_handle(later<Sig>(self.rcvr));
        }
    };

    R                               rcvr;
    connect_result_t<Child, rcvr_t> child;

    erase_handles_op(Child&& c, R r)
        : rcvr(std::move(r)), child(th::connect(std::move(c), rcvr_t(this))) {}
    erase_handles_op(erase_handles_op&&) = delete;

    TH_INLINE erased_handle start() & noexcept { return erased_handle(th::start(child)); }
};

template <class Child>
struct erase_handles_sender {
    Child child;
    template <class Env>
    static consteval auto get_completion_signatures() {
        return completions_of_t<Child, Env>();
    }
    template <class R>
    erase_handles_op<Child, R> connect(R rcvr) && {
        return {std::move(child), std::move(rcvr)};
    }
};
template <class Child>
erase_handles_sender<std::decay_t<Child>> erase_handles(Child&& c) {
    return {std::forward<Child>(c)};
}

} // namespace th
