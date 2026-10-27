// Minimal, deterministic sparse-set ECS.
//
// Determinism rules:
//  * iteration order = dense order of the first component in the query; it depends only on
//    the sequence of add/remove calls, which is itself deterministic in a lockstep simulation;
//  * entity ids are recycled LIFO with a generation counter;
//  * no pointers/addresses ever influence logic (no unordered_map keyed by pointer, etc.).
//
// Structural changes while iterating: adding components of OTHER types and creating entities is
// fine; destroying entities or removing components must go through destroyLater()/flush().
#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <cassert>
#include <tuple>
#include <type_traits>

namespace rts {

struct Entity {
    uint32_t index = UINT32_MAX;
    uint32_t gen = 0;
    constexpr bool valid() const { return index != UINT32_MAX; }
    constexpr bool operator==(const Entity&) const = default;
    constexpr uint64_t packed() const { return (uint64_t(gen) << 32) | index; }
    static constexpr Entity fromPacked(uint64_t p) { return {uint32_t(p & 0xffffffffu), uint32_t(p >> 32)}; }
};
inline constexpr Entity kNullEntity{};

namespace detail {
inline uint32_t nextComponentTypeId() { static uint32_t counter = 0; return counter++; }
template <class T> uint32_t componentTypeId() { static const uint32_t id = nextComponentTypeId(); return id; }
} // namespace detail

class IPool {
public:
    virtual ~IPool() = default;
    virtual void remove(uint32_t index) = 0;
    virtual bool has(uint32_t index) const = 0;
    virtual void clear() = 0;
};

template <class T>
class Pool final : public IPool {
public:
    static constexpr uint32_t kNone = UINT32_MAX;

    template <class... Args>
    T& emplace(Entity e, Args&&... args) {
        if (e.index >= sparse_.size()) sparse_.resize(e.index + 1, kNone);
        if (sparse_[e.index] != kNone) { data_[sparse_[e.index]] = T{std::forward<Args>(args)...}; return data_[sparse_[e.index]]; }
        sparse_[e.index] = static_cast<uint32_t>(dense_.size());
        dense_.push_back(e);
        data_.push_back(T{std::forward<Args>(args)...});
        return data_.back();
    }
    void remove(uint32_t index) override {
        if (!has(index)) return;
        uint32_t pos = sparse_[index];
        uint32_t last = static_cast<uint32_t>(dense_.size() - 1);
        if (pos != last) {
            dense_[pos] = dense_[last];
            data_[pos] = std::move(data_[last]);
            sparse_[dense_[pos].index] = pos;
        }
        dense_.pop_back(); data_.pop_back();
        sparse_[index] = kNone;
    }
    bool has(uint32_t index) const override { return index < sparse_.size() && sparse_[index] != kNone; }
    void clear() override { sparse_.clear(); dense_.clear(); data_.clear(); }

    T* tryGet(uint32_t index) { return has(index) ? &data_[sparse_[index]] : nullptr; }
    T& get(uint32_t index) { assert(has(index)); return data_[sparse_[index]]; }

    size_t size() const { return dense_.size(); }
    Entity entityAt(size_t i) const { return dense_[i]; }
    T& dataAt(size_t i) { return data_[i]; }

private:
    std::vector<uint32_t> sparse_;
    std::vector<Entity> dense_;
    std::vector<T> data_;
};

class World {
public:
    Entity create() {
        if (!free_.empty()) {
            uint32_t idx = free_.back(); free_.pop_back();
            alive_[idx] = 1;
            return {idx, gens_[idx]};
        }
        uint32_t idx = static_cast<uint32_t>(gens_.size());
        gens_.push_back(0); alive_.push_back(1);
        return {idx, 0};
    }

    bool alive(Entity e) const { return e.valid() && e.index < gens_.size() && alive_[e.index] && gens_[e.index] == e.gen; }

    void destroy(Entity e) {
        if (!alive(e)) return;
        for (auto& p : pools_) if (p) p->remove(e.index);
        alive_[e.index] = 0;
        ++gens_[e.index];
        free_.push_back(e.index);
    }
    void destroyLater(Entity e) { pendingDestroy_.push_back(e); }
    void flush() {
        for (size_t i = 0; i < pendingDestroy_.size(); ++i) destroy(pendingDestroy_[i]);
        pendingDestroy_.clear();
    }

    template <class T, class... Args> T& add(Entity e, Args&&... args) {
        assert(alive(e));
        return pool<T>().emplace(e, std::forward<Args>(args)...);
    }
    template <class T> void remove(Entity e) { if (alive(e)) pool<T>().remove(e.index); }
    template <class T> bool has(Entity e) const {
        auto* p = poolIfExists<T>();
        return alive(e) && p && p->has(e.index);
    }
    template <class T> T* tryGet(Entity e) {
        if (!alive(e)) return nullptr;
        auto* p = poolIfExists<T>();
        return p ? p->tryGet(e.index) : nullptr;
    }
    template <class T> T& get(Entity e) { assert(alive(e)); return pool<T>().get(e.index); }

    // Iterate entities having all components. fn(Entity, A&, B&, ...)
    template <class First, class... Rest, class Fn>
    void each(Fn&& fn) {
        auto& first = pool<First>();
        const size_t n = first.size(); // snapshot: entities created during iteration are skipped
        for (size_t i = 0; i < n && i < first.size(); ++i) {
            Entity e = first.entityAt(i);
            if constexpr (sizeof...(Rest) == 0) {
                fn(e, first.dataAt(i));
            } else {
                if ((pool<Rest>().has(e.index) && ...))
                    fn(e, first.dataAt(i), pool<Rest>().get(e.index)...);
            }
        }
    }
    template <class T> size_t count() { return pool<T>().size(); }

    template <class T> Pool<T>& pool() {
        uint32_t id = detail::componentTypeId<T>();
        if (id >= pools_.size()) pools_.resize(id + 1);
        if (!pools_[id]) pools_[id] = std::make_unique<Pool<T>>();
        return *static_cast<Pool<T>*>(pools_[id].get());
    }

    // Singleton "resources" (game config, map graph, etc.) stored outside the ECS pools.
    template <class T, class... Args> T& setResource(Args&&... args) {
        uint32_t id = detail::componentTypeId<T>();
        if (id >= resources_.size()) resources_.resize(id + 1);
        resources_[id] = std::make_shared<T>(std::forward<Args>(args)...);
        return *static_cast<T*>(resources_[id].get());
    }
    template <class T> T* resource() {
        uint32_t id = detail::componentTypeId<T>();
        return id < resources_.size() && resources_[id] ? static_cast<T*>(resources_[id].get()) : nullptr;
    }

    void clear() {
        pools_.clear(); resources_.clear(); gens_.clear(); alive_.clear(); free_.clear(); pendingDestroy_.clear();
    }

private:
    template <class T> const Pool<T>* poolIfExists() const {
        uint32_t id = detail::componentTypeId<T>();
        return id < pools_.size() && pools_[id] ? static_cast<const Pool<T>*>(pools_[id].get()) : nullptr;
    }
    template <class T> Pool<T>* poolIfExists() {
        uint32_t id = detail::componentTypeId<T>();
        return id < pools_.size() && pools_[id] ? static_cast<Pool<T>*>(pools_[id].get()) : nullptr;
    }

    std::vector<std::unique_ptr<IPool>> pools_;
    std::vector<std::shared_ptr<void>> resources_;
    std::vector<uint32_t> gens_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> free_;
    std::vector<Entity> pendingDestroy_;
};

} // namespace rts
