#include "TitanECS.h"

namespace Titan {
Entity EntityWorld::Create() {
    EnsureMutable();
    uint32_t index;
    if (!Free_.empty()) {
        index = Free_.back();
        Free_.pop_back();
    } else {
        if (Slots_.size() >= std::numeric_limits<uint32_t>::max())
            throw std::length_error("entity index space exhausted");
        index = static_cast<uint32_t>(Slots_.size());
        Slots_.push_back(Slot());
    }
    Slots_[index].Active = true;
    ++AliveCount_;
    return {index, Slots_[index].Generation};
}
bool EntityWorld::Alive(Entity e) const {
    return e.Generation != 0 && e.Index < Slots_.size() &&
           Slots_[e.Index].Active && Slots_[e.Index].Generation == e.Generation;
}
bool EntityWorld::Destroy(Entity e) {
    EnsureMutable();
    if (!Alive(e)) return false;
    for (auto& kv : Pools_) kv.second->Erase(e.Index);
    Slot& slot = Slots_[e.Index];
    slot.Active = false;
    if (++slot.Generation == 0) ++slot.Generation; // zero reserved as invalid
    Free_.push_back(e.Index);
    --AliveCount_;
    return true;
}
} // namespace Titan
