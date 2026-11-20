#pragma once
// TITAN original data-oriented entity storage. C++14, no Epic/engine headers.
// Structural changes during iteration are forbidden: use DeferredCommands afterwards.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Titan {

struct Entity {
    uint32_t Index = 0;
    uint32_t Generation = 0;
    bool operator==(Entity other) const { return Index == other.Index && Generation == other.Generation; }
    bool operator!=(Entity other) const { return !(*this == other); }
    explicit operator bool() const { return Generation != 0; }
};

struct Vec3d { double X = 0, Y = 0, Z = 0; };
struct Transform { Vec3d Position; };
struct Velocity { Vec3d Linear; };

class EntityWorld {
    struct Slot { uint32_t Generation = 1; bool Active = false; };
    struct IPool {
        virtual ~IPool() = default;
        virtual void Erase(uint32_t index) = 0;
        virtual std::size_t Size() const = 0;
    };
    template<class T> struct Pool final : IPool {
        static uint32_t Invalid() { return std::numeric_limits<uint32_t>::max(); }
        std::vector<uint32_t> Sparse;
        std::vector<Entity> Entities;
        std::vector<T> Values;
        std::size_t Size() const override { return Values.size(); }
        T* Find(Entity e) {
            if (e.Index >= Sparse.size() || Sparse[e.Index] == Invalid()) return nullptr;
            const uint32_t dense = Sparse[e.Index];
            return (dense < Entities.size() && Entities[dense] == e) ? &Values[dense] : nullptr;
        }
        const T* Find(Entity e) const { return const_cast<Pool*>(this)->Find(e); }
        void Set(Entity e, T value) {
            if (T* found = Find(e)) { *found = std::move(value); return; }
            if (e.Index >= Sparse.size()) Sparse.resize(static_cast<std::size_t>(e.Index) + 1, Invalid());
            if (Values.size() >= Invalid()) throw std::length_error("component pool exhausted");
            const uint32_t dense = static_cast<uint32_t>(Values.size());
            Values.push_back(std::move(value));
            try { Entities.push_back(e); } catch (...) { Values.pop_back(); throw; }
            Sparse[e.Index] = dense;
        }
        void Erase(uint32_t index) override {
            if (index >= Sparse.size() || Sparse[index] == Invalid()) return;
            const uint32_t dense = Sparse[index];
            const uint32_t last = static_cast<uint32_t>(Values.size() - 1);
            if (dense != last) {
                Values[dense] = std::move(Values[last]);
                Entities[dense] = Entities[last];
                Sparse[Entities[dense].Index] = dense;
            }
            Values.pop_back();
            Entities.pop_back();
            Sparse[index] = Invalid();
        }
    };
public:
    Entity Create();
    bool Alive(Entity entity) const;
    bool Destroy(Entity entity);
    std::size_t AliveCount() const { return AliveCount_; }
    std::size_t Capacity() const { return Slots_.size(); }

    template<class T> void Set(Entity entity, T value) {
        EnsureMutable();
        if (!Alive(entity)) throw std::invalid_argument("stale or invalid entity");
        FetchOrCreatePool<T>().Set(entity, std::move(value));
    }
    template<class T> T* Get(Entity entity) {
        if (!Alive(entity)) return nullptr;
        Pool<T>* pool = FindPool<T>();
        return pool ? pool->Find(entity) : nullptr;
    }
    template<class T> const T* Get(Entity entity) const {
        return const_cast<EntityWorld*>(this)->Get<T>(entity);
    }
    template<class T> bool Remove(Entity entity) {
        EnsureMutable();
        if (!Alive(entity)) return false;
        Pool<T>* pool = FindPool<T>();
        if (!pool || !pool->Find(entity)) return false;
        pool->Erase(entity.Index);
        return true;
    }
    template<class A, class Fn> void Each(Fn fn) {
        Pool<A>* a = FindPool<A>();
        if (!a) return;
        IterationGuard guard(*this);
        for (std::size_t i = 0; i < a->Values.size(); ++i) {
            if (Alive(a->Entities[i])) fn(a->Entities[i], a->Values[i]);
        }
    }
    template<class A, class B, class Fn> void Each(Fn fn) {
        Pool<A>* a = FindPool<A>();
        Pool<B>* b = FindPool<B>();
        if (!a || !b) return;
        IterationGuard guard(*this);
        if (a->Values.size() <= b->Values.size()) {
            for (std::size_t i = 0; i < a->Values.size(); ++i)
                if (B* other = b->Find(a->Entities[i])) fn(a->Entities[i], a->Values[i], *other);
        } else {
            for (std::size_t i = 0; i < b->Values.size(); ++i)
                if (A* other = a->Find(b->Entities[i])) fn(b->Entities[i], *other, b->Values[i]);
        }
    }
private:
    struct IterationGuard {
        EntityWorld& World;
        explicit IterationGuard(EntityWorld& world) : World(world) { ++World.IterationDepth_; }
        ~IterationGuard() { --World.IterationDepth_; }
    };
    void EnsureMutable() const {
        if (IterationDepth_) throw std::logic_error("structural ECS change during iteration; defer it");
    }
    template<class T> static const char* TypeSignature() {
#if defined(_MSC_VER)
        return __FUNCSIG__;
#else
        return __PRETTY_FUNCTION__;
#endif
    }
    template<class T> Pool<T>* FindPool() const {
        auto it = Pools_.find(TypeSignature<T>());
        return it == Pools_.end() ? nullptr : static_cast<Pool<T>*>(it->second.get());
    }
    template<class T> Pool<T>& FetchOrCreatePool() {
        if (Pool<T>* found = FindPool<T>()) return *found;
        std::unique_ptr<Pool<T>> incoming(new Pool<T>());
        Pool<T>* raw = incoming.get();
        Pools_.emplace(TypeSignature<T>(), std::move(incoming));
        return *raw;
    }
    std::vector<Slot> Slots_;
    std::vector<uint32_t> Free_;
    // Toolchain-local, cross-module type names (no RTTI required, UE typically disables it).
    // Do not serialize these strings: the spelling is compiler-specific.
    std::unordered_map<std::string, std::unique_ptr<IPool>> Pools_;
    std::size_t AliveCount_ = 0;
    unsigned IterationDepth_ = 0;
};

class DeferredCommands {
public:
    void Destroy(Entity entity) { Commands_.emplace_back([entity](EntityWorld& w) { w.Destroy(entity); }); }
    template<class T> void Set(Entity entity, T value) {
        Commands_.emplace_back([entity, value](EntityWorld& w) { if (w.Alive(entity)) w.Set<T>(entity, value); });
    }
    template<class T> void Remove(Entity entity) {
        Commands_.emplace_back([entity](EntityWorld& w) { w.Remove<T>(entity); });
    }
    void Apply(EntityWorld& world) {
        std::vector<std::function<void(EntityWorld&)>> commands;
        commands.swap(Commands_);
        for (auto& command : commands) command(world);
    }
    std::size_t Pending() const { return Commands_.size(); }
private:
    std::vector<std::function<void(EntityWorld&)>> Commands_;
};
} // namespace Titan
