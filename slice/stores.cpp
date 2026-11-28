// The definitions behind the interfaces, compiled on their own: a caller
// sees only `function`s and the entry points their constructors generate.
// Each factory borrows the object and the curried arguments, and returns
// the hand-lowered body as a sender.
#include "kv.hpp"
#include "sync.hpp"

namespace kv {

template <template <class> class Body, class Sigs, class... Args>
TH_INLINE body_sender<Body, Sigs, Args...> body(Args... args) noexcept {
    return {{args...}};
}

get_fn mem_store::get(str key) const {
    return get_fn(*this, std::move(key), [](const mem_store& self, str& key) noexcept {
        return body<mem_get_op, get_sigs>(&self, &key);
    });
}
fetch_fn mem_store::fetch(str key) const {
    return fetch_fn(*this, std::move(key), [](const mem_store& self, str& key) noexcept {
        return body<mem_fetch_op, fetch_sigs>(&self, &key);
    });
}

get_fn cache::get(str key) const {
    return get_fn(*this, std::move(key), [](const cache& self, str& key) noexcept {
        return body<cache_get_op, get_sigs>(&self, &key);
    });
}
fetch_fn cache::fetch(str key) const {
    return fetch_fn(*this, std::move(key), [](const cache& self, str& key) noexcept {
        return body<cache_fetch_op, fetch_sigs>(self.backing, &key);
    });
}

get_fn pass_store::get(str key) const {
    return get_fn(*this, std::move(key), [](const pass_store& self, str& key) noexcept {
        ++self.calls;
        return body<pass_get_op, get_sigs>(self.inner, &key);
    });
}
fetch_fn pass_store::fetch(str key) const {
    return fetch_fn(*this, std::move(key), [](const pass_store& self, str& key) noexcept {
        ++self.calls;
        return body<pass_fetch_op, fetch_sigs>(self.inner, &key);
    });
}

get_fn two_tier::get(str key) const {
    return get_fn(*this, std::move(key), [](const two_tier& self, str& key) noexcept {
        return body<tier_get_op, get_sigs>(&self, &key);
    });
}
fetch_fn two_tier::fetch(str key) const {
    return fetch_fn(*this, std::move(key), [](const two_tier& self, str& key) noexcept {
        return body<tier_fetch_op, fetch_sigs>(&self, &key);
    });
}

get_or_fn with_default::get_or(str key, str fallback) const {
    return get_or_fn(*this,
                     std::move(key),
                     std::move(fallback),
                     [](const with_default& self, str& key, str& fallback) noexcept {
                         return body<get_or_op, str_sigs>(&self, &key, &fallback);
                     });
}

fetch5_fn mem5_store::fetch(str key) const {
    return fetch5_fn(*this, std::move(key), [](const mem5_store& self, str& key) noexcept {
        return body<mem5_op, fetch5_sigs>(&self, &key);
    });
}
fetch5_fn pass5_store::fetch(str key) const {
    return fetch5_fn(*this, std::move(key), [](const pass5_store& self, str& key) noexcept {
        return body<pass5_op, fetch5_sigs>(self.inner, &key);
    });
}

pinned_fn pinned_mem::fetch(str key) const {
    return pinned_fn(*this, std::move(key), [](const pinned_mem& self, str& key) noexcept {
        return body<pinned_fetch_op, pinned_sigs>(&self, &key);
    });
}
pinned_value_fn pinned_default::get_or(str key) const {
    return pinned_value_fn(*this, std::move(key), [](const pinned_default& self, str& key) noexcept {
        return body<pinned_get_or_op, pinned_value_sigs>(&self, &key);
    });
}

} // namespace kv

// ---- the synchronous yardstick ---------------------------------------------
namespace yard {

value_t mem_store::get(str key) const {
    if (auto it = data.find(key); it != data.end()) {
        return it->second;
    }
    return std::nullopt;
}
result mem_store::fetch(str key) const {
    if (auto it = data.find(key); it != data.end()) {
        return it->second;
    }
    return not_found{};
}

value_t cache::get(str key) const {
    if (auto it = entries.find(key); it != entries.end()) {
        return value_t(it->second);
    }
    auto value = backing->get(key);
    if (value && retain) {
        entries.insert_or_assign(key, *value);
    }
    return value;
}
result cache::fetch(str key) const { return backing->fetch(std::move(key)); }

value_t pass_store::get(str key) const {
    ++calls;
    return inner->get(std::move(key));
}
result pass_store::fetch(str key) const {
    ++calls;
    return inner->fetch(std::move(key));
}

value_t two_tier::get(str key) const {
    if (auto v = primary->get(key)) {
        return v;
    }
    return secondary->get(std::move(key));
}
result two_tier::fetch(str key) const {
    if (auto r = primary->fetch(key)) {
        return r;
    }
    return secondary->fetch(std::move(key));
}

str with_default::get_or(str key, str fallback) const {
    if (auto r = backing->fetch(std::move(key))) {
        return std::move(*r);
    }
    return fallback;
}

} // namespace yard
