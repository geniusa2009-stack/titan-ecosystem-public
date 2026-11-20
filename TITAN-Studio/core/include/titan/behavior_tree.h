// TITAN Engine — behavior trees: validated tree assets, tick-based deterministic evaluation, per-agent instances.
// Original TITAN code.
//
// Asset (immutable, shared): nodes with integer ids, a root, and a blackboard schema. validate() rejects unknown
// kinds, wrong child counts, missing children, cycles, shared children (a node with two parents), unreachable
// nodes, trees deeper than 64 or larger than 4096 nodes, conditions on undeclared or mistyped keys, and actions
// whose type is not registered or whose params the action rejects.
//
// Node kinds (params):
//   sequence, selector                        composites with memory: a Running child is resumed next tick
//   inverter, forceSuccess, forceFailure      decorators
//   repeat {count}                            child must succeed `count` times (one completion per tick)
//   retry {count}                             child gets `count` attempts (one attempt per tick)
//   timeout {ticks}                           halts the child and fails if it is still Running on tick `ticks`
//   guard {condition}                         ticks the child only while the condition holds; halts it otherwise
//   condition {key, op, value?}               blackboard test; ops: isSet notSet isTrue isFalse eq ne lt le gt ge
//   wait {ticks}                              Running until its `ticks`-th tick, which returns Success
//   set {key, value} / clear {key}            blackboard writes, then Success
//   action {type, params}                     game logic from an ActionRegistry (TITAN Core knows no game APIs)
// Missing information: comparisons on an unset key are false (notSet is true).
//
// Runtime (BehaviorTreeInstance, one per agent): tick() visits at most TickBudget::maxNodeVisits nodes; when the
// budget runs out the tree reports Running and resumes from the same place next tick, with no node's side effects
// performed twice. abort() halts every running node (Action::abort is called exactly once per running action, in
// tree order) and resets all state; destroying an instance does not call Action::abort (it has no blackboard), so
// call abort() first when actions hold external resources. An exception thrown by an action is caught, recorded
// in lastError() and treated as Failure. tick() must be given a blackboard bound to the tree's own schema.
// Save format: the asset is "titan.behaviortree" v1; blackboards use titan/blackboard.h. In-flight action state
// is owned by the actions and is not serialized: a loaded tree starts from its root.
// Thread safety: an instance is used by one thread; assets and registries are immutable and shareable.
#pragma once

#include "titan/blackboard.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace titan::ai {

enum class Status { Success, Failure, Running };
const char* toString(Status s);

struct ActionContext {
    Blackboard& blackboard;
    const Json& params;
    uint64_t agent;
    uint64_t tick;  // ticks completed by this instance before this one
};

class Action {
public:
    virtual ~Action() = default;
    virtual Status tick(ActionContext& ctx) = 0;
    virtual void abort(ActionContext&) {}  // called when a Running action is interrupted
};

struct ActionType {
    std::string name;
    std::function<std::unique_ptr<Action>(const Json& params)> create;
    // Optional: throws std::invalid_argument when params are invalid for this schema.
    std::function<void(const Json& params, const BlackboardSchema& schema)> validate;
};

class ActionRegistry {
public:
    void add(ActionType type);  // std::invalid_argument: bad / duplicate name or no factory
    const ActionType* find(const std::string& name) const;

private:
    std::map<std::string, ActionType> types_;
};

struct BtNode {
    int64_t id = 0;
    std::string kind;
    std::vector<int64_t> children;
    Json params = Json::object();
};

class BehaviorTree {
public:
    // Validates against the registry (may be null when the tree has no action nodes).
    BehaviorTree(std::shared_ptr<const BlackboardSchema> schema, std::vector<BtNode> nodes, int64_t root,
                 const ActionRegistry* registry);

    const BlackboardSchema& schema() const { return *schema_; }
    std::shared_ptr<const BlackboardSchema> schemaPtr() const { return schema_; }
    const std::vector<BtNode>& nodes() const { return nodes_; }  // as given
    int64_t root() const { return root_; }

    // {"format":"titan.behaviortree","version":1,"schema":{...},"root":id,"nodes":[{"id","kind","children","params"}]}
    Json toJson() const;
    static BehaviorTree fromJson(const Json& j, const ActionRegistry* registry);

private:
    friend class BehaviorTreeInstance;
    struct Compiled {
        int kind;
        std::vector<int> children;  // dense indices
        const BtNode* node;
    };
    std::shared_ptr<const BlackboardSchema> schema_;
    std::vector<BtNode> nodes_;
    int64_t root_;
    std::vector<Compiled> compiled_;  // dense, root first, depth-first pre-order
};

struct TickBudget {
    uint32_t maxNodeVisits = 1024;  // [1, 2^20]
};

struct TickResult {
    Status status = Status::Running;
    uint32_t visits = 0;
    bool budgetExhausted = false;
};

class BehaviorTreeInstance {
public:
    // std::invalid_argument for a null tree, a blackboard bound to another schema, or a missing registry when the
    // tree has actions.
    BehaviorTreeInstance(std::shared_ptr<const BehaviorTree> tree, std::shared_ptr<const ActionRegistry> registry,
                         uint64_t agent = 0);
    BehaviorTreeInstance(const BehaviorTreeInstance&) = delete;
    BehaviorTreeInstance& operator=(const BehaviorTreeInstance&) = delete;

    TickResult tick(Blackboard& blackboard, const TickBudget& budget = {});
    void abort(Blackboard& blackboard);  // halt running nodes and reset
    bool running() const;
    uint64_t ticks() const { return ticks_; }
    const std::string& lastError() const { return lastError_; }

private:
    struct NodeState {
        bool running = false;
        uint32_t index = 0;    // composites: current child
        uint32_t counter = 0;  // repeat / retry / wait / timeout
    };
    Status run(int i, Blackboard& bb);
    void halt(int i, Blackboard& bb);

    std::shared_ptr<const BehaviorTree> tree_;
    std::shared_ptr<const ActionRegistry> registry_;
    uint64_t agent_;
    std::vector<NodeState> state_;
    std::vector<std::unique_ptr<Action>> actions_;  // per dense node (actions only)
    uint32_t visitsLeft_ = 0;
    bool exhausted_ = false;
    uint64_t ticks_ = 0;
    std::string lastError_;
};

// Blackboard conditions ({key, op, value?}), shared with the utility decision system.
// validateCondition throws std::invalid_argument (prefixed with `context`) for unknown keys / ops or mistyped values.
void validateCondition(const Json& condition, const BlackboardSchema& schema, const std::string& context = {});
bool evaluateCondition(const Json& condition, const Blackboard& bb);  // condition must have been validated

}  // namespace titan::ai
