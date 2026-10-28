// Binary archive for deterministic save states (Simulation::serialize). One code path reads and writes:
// io(Archive&, T&) overloads list the fields of every type in a fixed order; integers are little-endian with
// a fixed width, so the bytes are identical on every platform and compiler (no struct memcpy, no padding).
// Reading checks bounds; a truncated or corrupt buffer sets ok() = false and yields zeros, never crashes.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "ECS.h"
#include "Fixed.h"

namespace rts {

class Archive {
public:
    explicit Archive(std::string* out) : out_(out) {}
    explicit Archive(const std::string& in) : in_(&in) {}
    bool writing() const { return out_ != nullptr; }
    bool ok() const { return ok_; }
    void fail() { ok_ = false; }
    size_t remaining() const { return in_ ? in_->size() - pos_ : 0; }

    void bytes(uint64_t& v, int n) {
        if (out_) { for (int i = 0; i < n; ++i) out_->push_back(char((v >> (8 * i)) & 0xFF)); return; }
        if (!ok_ || pos_ + size_t(n) > in_->size()) { ok_ = false; v = 0; return; }
        v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t(uint8_t((*in_)[pos_ + size_t(i)])) << (8 * i);
        pos_ += size_t(n);
    }
    // Length prefix of a container; when reading, rejects lengths the rest of the buffer cannot hold.
    uint32_t count(size_t n, size_t minBytesEach = 1) {
        uint64_t v = n;
        bytes(v, 4);
        if (!out_ && (v > remaining() / (minBytesEach ? minBytesEach : 1) + 1 || v > 0x7FFFFFFF)) { ok_ = false; return 0; }
        return uint32_t(v);
    }

    template <class T, class... R> void operator()(T& v, R&... rest);

private:
    std::string* out_ = nullptr;
    const std::string* in_ = nullptr;
    size_t pos_ = 0;
    bool ok_ = true;
};

template <class T> requires(std::is_integral_v<T> || std::is_enum_v<T>)
void io(Archive& a, T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        uint64_t u = v ? 1 : 0; a.bytes(u, 1); if (!a.writing()) v = u != 0;
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        U u = U(v); io(a, u); if (!a.writing()) v = T(u);
    } else {
        using U = std::make_unsigned_t<T>;
        uint64_t u = uint64_t(U(v)); a.bytes(u, int(sizeof(T))); if (!a.writing()) v = T(U(u));
    }
}
inline void io(Archive& a, Fixed& v) { io(a, v.raw); }
inline void io(Archive& a, Vec3& v) { io(a, v.x); io(a, v.y); io(a, v.z); }
inline void io(Archive& a, Entity& e) { io(a, e.index); io(a, e.gen); }
inline void io(Archive& a, std::string& s) {
    uint32_t n = a.count(s.size());
    if (!a.writing()) s.assign(n, '\0');
    for (uint32_t i = 0; i < n && a.ok(); ++i) { uint8_t c = uint8_t(s[i]); io(a, c); s[i] = char(c); }
}
template <class T, size_t N> void io(Archive& a, T (&arr)[N]) { for (auto& x : arr) io(a, x); }
template <class A, class B> void io(Archive& a, std::pair<A, B>& p) { io(a, p.first); io(a, p.second); }
template <class T> void io(Archive& a, std::vector<T>& v) {
    uint32_t n = a.count(v.size());
    if (!a.writing()) v.assign(n, T{});
    for (uint32_t i = 0; i < n && a.ok(); ++i) io(a, v[i]);
}
template <class K, class V> void io(Archive& a, std::map<K, V>& m) {
    uint32_t n = a.count(m.size());
    if (a.writing()) { for (auto& [k, v] : m) { K kk = k; io(a, kk); io(a, v); } return; }
    m.clear();
    for (uint32_t i = 0; i < n && a.ok(); ++i) { K k{}; V v{}; io(a, k); io(a, v); m.emplace(std::move(k), std::move(v)); }
}

template <class T, class... R> void Archive::operator()(T& v, R&... rest) {
    io(*this, v);
    if constexpr (sizeof...(R) > 0) (*this)(rest...);
}

} // namespace rts
