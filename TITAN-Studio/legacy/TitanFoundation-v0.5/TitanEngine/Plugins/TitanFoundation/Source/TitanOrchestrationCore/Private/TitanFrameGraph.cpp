#include "TitanFrameGraph.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace Titan {
int FrameGraph::AddResource(const std::string& name, std::size_t bytes, bool external) {
    if (name.empty() || bytes == 0) throw std::invalid_argument("resource must have a name and size");
    for (const auto& r : Resources_) if (r.Name == name) throw std::invalid_argument("duplicate resource: "+name);
    Resources_.push_back({name,bytes,external});
    return static_cast<int>(Resources_.size()-1);
}
int FrameGraph::AddPass(const std::string& name, const std::vector<int>& reads,
                        const std::vector<int>& writes, const std::vector<int>& after) {
    if (name.empty()) throw std::invalid_argument("unnamed pass");
    for (const auto& p : Passes_) if (p.Name == name) throw std::invalid_argument("duplicate pass: "+name);
    Passes_.push_back({name,reads,writes,after});
    return static_cast<int>(Passes_.size()-1);
}
GraphPlan FrameGraph::Compile() const {
    const int n = static_cast<int>(Passes_.size());
    const int rn = static_cast<int>(Resources_.size());
    std::vector<std::set<int>> edges(static_cast<std::size_t>(n));
    std::vector<int> indegree(static_cast<std::size_t>(n),0);
    auto edge = [&](int from,int to) {
        if (from == to) throw std::logic_error("self dependency in graph");
        if (edges[static_cast<std::size_t>(from)].insert(to).second) ++indegree[static_cast<std::size_t>(to)];
    };
    std::vector<int> lastWriter(static_cast<std::size_t>(rn),-1);
    std::vector<std::set<int>> activeReaders(static_cast<std::size_t>(rn));
    for (int p=0;p<n;++p) {
        const GraphPass& pass=Passes_[static_cast<std::size_t>(p)];
        std::set<int> readSet(pass.Reads.begin(),pass.Reads.end());
        std::set<int> writeSet(pass.Writes.begin(),pass.Writes.end());
        if (readSet.size()!=pass.Reads.size() || writeSet.size()!=pass.Writes.size())
            throw std::invalid_argument("duplicate resource access in pass "+pass.Name);
        for (int dep : pass.After) {
            if (dep < 0 || dep >= n) throw std::invalid_argument("invalid pass dependency");
            edge(dep,p);
        }
        for (int id : readSet) {
            if (id<0 || id>=rn) throw std::invalid_argument("unknown read resource");
            if (writeSet.count(id)) throw std::invalid_argument("ambiguous same-pass read/write");
            if (lastWriter[static_cast<std::size_t>(id)]<0 && !Resources_[static_cast<std::size_t>(id)].External)
                throw std::logic_error("read before write: "+Resources_[static_cast<std::size_t>(id)].Name);
            if (lastWriter[static_cast<std::size_t>(id)]>=0) edge(lastWriter[static_cast<std::size_t>(id)],p);
            activeReaders[static_cast<std::size_t>(id)].insert(p);
        }
        for (int id : writeSet) {
            if (id<0 || id>=rn) throw std::invalid_argument("unknown write resource");
            if (lastWriter[static_cast<std::size_t>(id)]>=0) edge(lastWriter[static_cast<std::size_t>(id)],p);
            for (int readPass : activeReaders[static_cast<std::size_t>(id)]) edge(readPass,p);
            activeReaders[static_cast<std::size_t>(id)].clear();
            lastWriter[static_cast<std::size_t>(id)]=p;
        }
    }
    GraphPlan plan;
    std::set<int> ready;
    for (int i=0;i<n;++i) if (!indegree[static_cast<std::size_t>(i)]) ready.insert(i);
    while (!ready.empty()) {
        int p=*ready.begin(); ready.erase(ready.begin());
        plan.PassOrder.push_back(p);
        for (int next : edges[static_cast<std::size_t>(p)])
            if (--indegree[static_cast<std::size_t>(next)]==0) ready.insert(next);
    }
    if (plan.PassOrder.size()!=static_cast<std::size_t>(n)) throw std::logic_error("render graph cycle");
    for (const auto& e : edges) plan.EdgeCount+=e.size();
    plan.Lifetimes.resize(static_cast<std::size_t>(rn));
    for (int order=0;order<n;++order) {
        const GraphPass& pass=Passes_[static_cast<std::size_t>(plan.PassOrder[static_cast<std::size_t>(order)])];
        auto visit=[&](int id) {
            ResourceLifetime& lt=plan.Lifetimes[static_cast<std::size_t>(id)];
            if (lt.First < 0) lt.First=order;
            lt.Last=order;
        };
        for (int id:pass.Reads) visit(id);
        for (int id:pass.Writes) visit(id);
    }
    // Greedy transient alias allocator. Resources can share a slot only if lifetimes don't overlap.
    struct Slot { std::size_t capacity; int lastUse; };
    std::vector<Slot> slots;
    std::vector<int> resources;
    for (int id=0;id<rn;++id) if (!Resources_[static_cast<std::size_t>(id)].External &&
        plan.Lifetimes[static_cast<std::size_t>(id)].First >= 0) resources.push_back(id);
    std::sort(resources.begin(),resources.end(),[&](int a,int b) {
        const ResourceLifetime& x=plan.Lifetimes[static_cast<std::size_t>(a)];
        const ResourceLifetime& y=plan.Lifetimes[static_cast<std::size_t>(b)];
        return x.First!=y.First ? x.First<y.First : a<b;
    });
    for (int id : resources) {
        const std::size_t bytes=Resources_[static_cast<std::size_t>(id)].Bytes;
        ResourceLifetime& lt=plan.Lifetimes[static_cast<std::size_t>(id)];
        int selected=-1;
        for (std::size_t i=0;i<slots.size();++i) {
            if (slots[i].lastUse < lt.First && slots[i].capacity>=bytes &&
                (selected<0 || slots[i].capacity<slots[static_cast<std::size_t>(selected)].capacity))
                selected=static_cast<int>(i);
        }
        if (selected<0) {
            selected=static_cast<int>(slots.size());
            slots.push_back({bytes,lt.Last});
        } else slots[static_cast<std::size_t>(selected)].lastUse=lt.Last;
        lt.PoolSlot=selected;
    }
    for (const Slot& slot:slots) {
        if (slot.capacity > std::numeric_limits<std::size_t>::max() - plan.TransientPoolBytes)
            throw std::overflow_error("graph memory budget overflow");
        plan.TransientPoolBytes+=slot.capacity;
        plan.SlotCapacities.push_back(slot.capacity);
    }
    return plan;
}
}
