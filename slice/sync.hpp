// The synchronous yardstick (namespace yard): the same bodies as plain virtual functions.
// A typed error is returned as the alternative of a small result type (not
// std::expected, which libstdc++ 13 doesn't offer to Clang 18).
#pragma once
#include "kv.hpp"

#include <map>
#include <memory>
#include <new>

namespace yard {

using kv::not_found;
using kv::value_t;
// A str or not_found, like std::expected<str, not_found>.
class result {
  public:
    result(const str& v) : ok_(true) { ::new (&v_) str(v); }
    result(str&& v) noexcept : ok_(true) { ::new (&v_) str(std::move(v)); }
    result(not_found) noexcept : ok_(false) {}
    result(result&& o) noexcept : ok_(o.ok_) {
        if (ok_) {
            ::new (&v_) str(std::move(o.v_));
        }
    }
    ~result() {
        if (ok_) {
            std::destroy_at(&v_);
        }
    }
    explicit operator bool() const noexcept { return ok_; }
    str&     operator*() noexcept { return v_; }
    const str& operator*() const noexcept { return v_; }

  private:
    bool ok_;
    union {
        str v_;
    };
};

struct kv_store {
    virtual value_t get(str key) const   = 0;
    virtual result  fetch(str key) const = 0;
    virtual ~kv_store()                  = default;
};

class mem_store final : public kv_store {
  public:
    std::map<str, str> data;
    value_t            get(str key) const override;
    result             fetch(str key) const override;
};

class cache final : public kv_store {
  public:
    explicit cache(const kv_store* b, bool r = true) : backing(b), retain(r) {}
    const kv_store*            backing;
    bool                       retain;
    mutable std::map<str, str> entries;
    value_t                    get(str key) const override;
    result                     fetch(str key) const override;
};

class pass_store final : public kv_store {
  public:
    explicit pass_store(const kv_store* i) : inner(i) {}
    const kv_store* inner;
    mutable long    calls = 0;
    value_t         get(str key) const override;
    result          fetch(str key) const override;
};

class two_tier final : public kv_store {
  public:
    two_tier(const kv_store* p, const kv_store* s) : primary(p), secondary(s) {}
    const kv_store* primary;
    const kv_store* secondary;
    value_t         get(str key) const override;
    result          fetch(str key) const override;
};

class with_default {
  public:
    explicit with_default(const kv_store* b) : backing(b) {}
    const kv_store* backing;
    virtual str     get_or(str key, str fallback) const;
    virtual ~with_default() = default;
};

} // namespace yard
