// The async stack: a basic library implementation.
//
// A strand is one sequential path's stack of frames. Frames are pushed and
// popped in LIFO order, in slabs: the first slab is storage the strand's
// owner provides (a root keeps it on the machine stack), and further slabs
// come from operator new when a push doesn't fit, and are freed when the
// last frame in them is popped. The specification would say only that the
// async stack is LIFO storage reached through the frame allocator; this is
// one permitted implementation, and the machine stack is another.
//
// The cursor is the frame allocator: get_frame_allocator(env) answers with
// the strand, whose interface is push and pop. There is no separate cursor
// query.
#pragma once
#include "env.hpp"

#include <cstddef>
#include <cstdint>
#include <new>

namespace th {

#ifdef SLICE_COUNT
// Heap allocations made by slabs, and the high-water mark of frame storage.
inline long        g_slab_allocs = 0;
inline std::size_t g_frame_bytes = 0, g_frame_high = 0;
#endif

class strand {
  public:
    // A strand over storage of n bytes that the owner provides and keeps
    // alive at least as long as the strand.
    strand(void* storage, std::size_t n) noexcept
        : first_{nullptr, static_cast<std::byte*>(storage), n, nullptr},
          cur_(&first_),
          top_(first_.base) {}
    strand(strand&&) = delete;
    ~strand() {
        while (cur_ != &first_) {
            pop_slab();
        }
    }

    // Storage for a frame of `size` bytes aligned to `align`.
    void* push(std::size_t size, std::size_t align) {
        auto*       at   = align_up(top_, align);
        std::size_t used = std::size_t(at - cur_->base) + size;
        if (used > cur_->size) [[unlikely]] {
            push_slab(size + align);
            at = align_up(top_, align);
        }
        top_ = at + size;
#ifdef SLICE_COUNT
        g_frame_bytes += size;
        if (g_frame_bytes > g_frame_high) {
            g_frame_high = g_frame_bytes;
        }
#endif
        return at;
    }

    // Pop the most recently pushed frame, which starts at p and is `size`
    // bytes long. (The size is used only for counting.)
    void pop(void* p, [[maybe_unused]] std::size_t size) noexcept {
        auto* at = static_cast<std::byte*>(p);
        if (at < cur_->base || at > cur_->base + cur_->size) [[unlikely]] {
            pop_slab();
        }
        top_ = at;
#ifdef SLICE_COUNT
        g_frame_bytes -= size;
#endif
    }

    // Whether every frame has been popped.
    bool empty() const noexcept { return cur_ == &first_ && top_ == first_.base; }

  private:
    struct slab {
        slab*       prev;
        std::byte*  base;
        std::size_t size;
        std::byte*  prev_top; // the previous slab's top when this one was pushed
    };

    static std::byte* align_up(std::byte* p, std::size_t a) noexcept {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        return reinterpret_cast<std::byte*>((v + a - 1) & ~std::uintptr_t(a - 1));
    }

    static constexpr std::size_t slab_bytes = 4096;

    void push_slab(std::size_t at_least) {
        std::size_t n   = at_least > slab_bytes ? at_least : slab_bytes;
        void*       raw = ::operator new(sizeof(slab) + n);
#ifdef SLICE_COUNT
        ++g_slab_allocs;
#endif
        cur_ = ::new (raw) slab{cur_, static_cast<std::byte*>(raw) + sizeof(slab), n, top_};
        top_ = cur_->base;
    }
    void pop_slab() noexcept {
        slab* s = cur_;
        cur_    = s->prev;
        top_    = s->prev_top;
        ::operator delete(static_cast<void*>(s));
    }

    slab       first_;
    slab*      cur_;
    std::byte* top_;
};

struct get_frame_allocator_t {
    template <class E>
    TH_INLINE strand* operator()(const E& e) const noexcept {
        if constexpr (requires { e.query(*this); }) {
            return e.query(*this);
        } else {
            return nullptr;
        }
    }
};
inline constexpr get_frame_allocator_t get_frame_allocator{};

template <class R>
TH_INLINE strand* frame_allocator_of(const R& r) noexcept {
    return get_frame_allocator(th::get_env(r));
}

} // namespace th
