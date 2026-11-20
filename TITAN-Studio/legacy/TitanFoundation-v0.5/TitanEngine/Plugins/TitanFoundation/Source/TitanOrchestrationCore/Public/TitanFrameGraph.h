#pragma once
// TITAN-owned C++14 render-frame graph compiler; GPU work is intentionally back-end agnostic.
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace Titan {
struct GraphResource {
    std::string Name;
    std::size_t Bytes;
    bool External; // externally provided / imported resource, assumed initialized
};
struct GraphPass {
    std::string Name;
    std::vector<int> Reads;
    std::vector<int> Writes;
    std::vector<int> After;
};
struct ResourceLifetime { int First = -1; int Last = -1; int PoolSlot = -1; };
struct GraphPlan {
    std::vector<int> PassOrder;
    std::vector<ResourceLifetime> Lifetimes;
    std::vector<std::size_t> SlotCapacities;
    std::size_t TransientPoolBytes = 0;
    std::size_t EdgeCount = 0;
};
class FrameGraph {
public:
    int AddResource(const std::string& name, std::size_t bytes, bool external = false);
    int AddPass(const std::string& name, const std::vector<int>& reads,
                const std::vector<int>& writes, const std::vector<int>& after = {});
    // Deterministic topological ordering, hazard detection and transient-memory alias planning.
    // Throws std::invalid_argument / std::logic_error on invalid graphs.
    GraphPlan Compile() const;
    const std::vector<GraphResource>& Resources() const { return Resources_; }
    const std::vector<GraphPass>& Passes() const { return Passes_; }
private:
    std::vector<GraphResource> Resources_;
    std::vector<GraphPass> Passes_;
};
}
