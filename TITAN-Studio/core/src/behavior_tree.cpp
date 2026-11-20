// TITAN Engine — behavior trees. Original TITAN code.
#include "titan/behavior_tree.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace titan::ai {
namespace {

enum Kind {
    kSequence, kSelector, kInverter, kForceSuccess, kForceFailure, kRepeat, kRetry, kTimeout, kGuard,
    kCondition, kWait, kSet, kClear, kAction, kKindCount
};
const char* const kKindNames[kKindCount] = {"sequence", "selector", "inverter", "forceSuccess", "forceFailure",
                                            "repeat", "retry", "timeout", "guard", "condition", "wait", "set",
                                            "clear", "action"};
constexpr size_t kMaxNodes = 4096;
constexpr int kMaxDepth = 64;
constexpr size_t kMaxChildren = 256;

void fail(const std::string& msg) { throw std::invalid_argument(msg); }

int kindOf(const std::string& s) {
    for (int k = 0; k < kKindCount; ++k)
        if (s == kKindNames[k]) return k;
    return -1;
}

std::string where(const BtNode& n) { return "behavior tree node " + std::to_string(n.id) + " (" + n.kind + "): "; }

int64_t countParam(const BtNode& n, const char* key, int64_t max) {
    const Json* v = n.params.find(key);
    if (!v || !v->isNumber() || v->asNumber() != std::floor(v->asNumber()) || v->asNumber() < 1 || v->asNumber() > double(max))
        fail(where(n) + "'" + key + "' must be an integer in [1, " + std::to_string(max) + "]");
    return v->asInt();
}

const BbKey& keyParam(const Json& obj, const BlackboardSchema& schema, const std::string& context) {
    const Json* k = obj.find("key");
    if (!k || !k->isString()) fail(context + "'key' must be a string");
    const BbKey* key = schema.find(k->asString());
    if (!key) fail(context + "unknown blackboard key '" + k->asString() + "'");
    return *key;
}

void validateNode(const BtNode& n, int kind, const BlackboardSchema& schema, const ActionRegistry* registry) {
    const std::string w = where(n);
    if (!n.params.isObject()) fail(w + "params must be an object");
    const size_t c = n.children.size();
    switch (kind) {
        case kSequence:
        case kSelector:
            if (c < 1 || c > kMaxChildren) fail(w + "composites need 1 to 256 children");
            return;
        case kInverter:
        case kForceSuccess:
        case kForceFailure:
        case kRepeat:
        case kRetry:
        case kTimeout:
        case kGuard:
            if (c != 1) fail(w + "decorators need exactly one child");
            if (kind == kRepeat || kind == kRetry) countParam(n, "count", 1000000);
            if (kind == kTimeout) countParam(n, "ticks", 1000000000);
            if (kind == kGuard) {
                const Json* cond = n.params.find("condition");
                if (!cond) fail(w + "guard needs a 'condition'");
                validateCondition(*cond, schema, w);
            }
            return;
        default: break;
    }
    if (c != 0) fail(w + "leaves cannot have children");
    switch (kind) {
        case kCondition: validateCondition(n.params, schema, w); return;
        case kWait: countParam(n, "ticks", 1000000000); return;
        case kSet: {
            const BbKey& key = keyParam(n.params, schema, w);
            const Json* v = n.params.find("value");
            if (!v) fail(w + "set needs a 'value'");
            try {
                (void)valueFromJson(*v, key.type);
            } catch (const std::exception& e) {
                fail(w + "value does not match key '" + key.name + "': " + e.what());
            }
            return;
        }
        case kClear: keyParam(n.params, schema, w); return;
        case kAction: {
            const std::string type = n.params.getString("type");
            if (!registry) fail(w + "action nodes need an ActionRegistry");
            const ActionType* t = registry->find(type);
            if (!t) fail(w + "unknown action type '" + type + "'");
            const Json* p = n.params.find("params");
            if (p && !p->isObject()) fail(w + "action 'params' must be an object");
            if (t->validate) {
                try {
                    t->validate(p ? *p : Json::object(), schema);
                } catch (const std::exception& e) {
                    fail(w + "invalid params for action '" + type + "': " + e.what());
                }
            }
            return;
        }
        default: break;
    }
}

const Json& emptyObject() {
    static const Json e = Json::object();
    return e;
}

}  // namespace

const char* toString(Status s) {
    switch (s) {
        case Status::Success: return "success";
        case Status::Failure: return "failure";
        case Status::Running: return "running";
    }
    return "failure";
}

// ---- registry ------------------------------------------------------------------------------------------------------

void ActionRegistry::add(ActionType type) {
    if (!isValidKeyName(type.name)) fail("invalid action type name '" + type.name + "'");
    if (!type.create) fail("action type '" + type.name + "' needs a factory");
    if (types_.count(type.name)) fail("duplicate action type '" + type.name + "'");
    const std::string name = type.name;
    types_.emplace(name, std::move(type));
}

const ActionType* ActionRegistry::find(const std::string& name) const {
    auto it = types_.find(name);
    return it == types_.end() ? nullptr : &it->second;
}

// ---- conditions ----------------------------------------------------------------------------------------------------

void validateCondition(const Json& c, const BlackboardSchema& schema, const std::string& context) {
    if (!c.isObject()) fail(context + "condition must be an object");
    const BbKey& key = keyParam(c, schema, context);
    const std::string op = c.getString("op");
    if (op == "isSet" || op == "notSet") return;
    if (op == "isTrue" || op == "isFalse") {
        if (key.type != BbType::Bool) fail(context + "'" + op + "' needs a bool key");
        return;
    }
    const bool ordered = op == "lt" || op == "le" || op == "gt" || op == "ge";
    if (!ordered && op != "eq" && op != "ne") fail(context + "unknown condition op '" + op + "'");
    if (ordered && key.type != BbType::Int && key.type != BbType::Float) fail(context + "'" + op + "' needs a numeric key");
    const Json* v = c.find("value");
    if (!v) fail(context + "'" + op + "' needs a 'value'");
    try {
        (void)valueFromJson(*v, key.type);
    } catch (const std::exception& e) {
        fail(context + "value does not match key '" + key.name + "' (" + toString(key.type) + "): " + e.what());
    }
}


bool evaluateCondition(const Json& c, const Blackboard& bb) {
    const std::string& keyName = c["key"].asString();
    const std::string op = c.getString("op");
    const BbValue* v = bb.get(keyName);
    if (op == "notSet") return v == nullptr;
    if (!v) return false;  // missing information never satisfies a test
    if (op == "isSet") return true;
    if (op == "isTrue") return std::get<bool>(*v);
    if (op == "isFalse") return !std::get<bool>(*v);
    const BbType type = typeOf(*v);
    const BbValue rhs = valueFromJson(c["value"], type);
    if (op == "eq") return *v == rhs;
    if (op == "ne") return !(*v == rhs);
    const double a = type == BbType::Int ? double(std::get<int64_t>(*v)) : std::get<double>(*v);
    const double b = type == BbType::Int ? double(std::get<int64_t>(rhs)) : std::get<double>(rhs);
    if (op == "lt") return a < b;
    if (op == "le") return a <= b;
    if (op == "gt") return a > b;
    return a >= b;
}

// ---- asset ---------------------------------------------------------------------------------------------------------

BehaviorTree::BehaviorTree(std::shared_ptr<const BlackboardSchema> schema, std::vector<BtNode> nodes, int64_t root,
                           const ActionRegistry* registry)
    : schema_(std::move(schema)), nodes_(std::move(nodes)), root_(root) {
    if (!schema_) fail("behavior tree needs a blackboard schema");
    if (nodes_.empty() || nodes_.size() > kMaxNodes) fail("behavior trees need 1 to 4096 nodes");
    std::map<int64_t, size_t> byId;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (!byId.emplace(nodes_[i].id, i).second) fail("duplicate behavior tree node id " + std::to_string(nodes_[i].id));
    std::vector<int> kinds(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const BtNode& n = nodes_[i];
        kinds[i] = kindOf(n.kind);
        if (kinds[i] < 0) fail("behavior tree node " + std::to_string(n.id) + ": unknown kind '" + n.kind + "'");
        validateNode(n, kinds[i], *schema_, registry);
        for (int64_t c : n.children)
            if (!byId.count(c))
                fail("behavior tree node " + std::to_string(n.id) + ": child " + std::to_string(c) + " does not exist");
    }
    auto rootIt = byId.find(root_);
    if (rootIt == byId.end()) fail("behavior tree root " + std::to_string(root_) + " does not exist");

    // Depth-first compile from the root: rejects cycles, shared children and excessive depth.
    std::vector<int> denseOf(nodes_.size(), -1);
    std::vector<uint8_t> onPath(nodes_.size(), 0);
    struct Frame {
        size_t node;
        size_t next;
        int dense;
    };
    std::vector<Frame> stack;
    auto enter = [&](size_t node) {
        if (onPath[node]) fail("behavior tree has a cycle through node " + std::to_string(nodes_[node].id));
        if (denseOf[node] >= 0) fail("behavior tree node " + std::to_string(nodes_[node].id) + " has more than one parent");
        if (stack.size() >= size_t(kMaxDepth)) fail("behavior tree is deeper than 64 levels");
        const int dense = int(compiled_.size());
        denseOf[node] = dense;
        onPath[node] = 1;
        compiled_.push_back({kinds[node], {}, &nodes_[node]});
        stack.push_back({node, 0, dense});
    };
    enter(rootIt->second);
    while (!stack.empty()) {
        Frame& f = stack.back();
        const BtNode& n = nodes_[f.node];
        if (f.next < n.children.size()) {
            const size_t child = byId.at(n.children[f.next++]);
            const int parentDense = f.dense;
            enter(child);  // may invalidate `f`
            compiled_[size_t(parentDense)].children.push_back(denseOf[child]);
        } else {
            onPath[f.node] = 0;
            stack.pop_back();
        }
    }
    if (compiled_.size() != nodes_.size()) {
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (denseOf[i] < 0) fail("behavior tree node " + std::to_string(nodes_[i].id) + " is not reachable from the root");
    }
}

Json BehaviorTree::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.behaviortree";
    j["version"] = 1;
    j["schema"] = schema_->toJson();
    j["root"] = root_;
    Json ns = Json::array();
    for (const BtNode& n : nodes_) {
        Json o = Json::object();
        o["id"] = n.id;
        o["kind"] = n.kind;
        Json cs = Json::array();
        for (int64_t c : n.children) cs.push_back(c);
        o["children"] = std::move(cs);
        o["params"] = n.params;
        ns.push_back(std::move(o));
    }
    j["nodes"] = std::move(ns);
    return j;
}

BehaviorTree BehaviorTree::fromJson(const Json& j, const ActionRegistry* registry) {
    if (!j.isObject() || j.getString("format") != "titan.behaviortree") throw JsonError("not a titan.behaviortree document");
    const Json* v = j.find("version");
    if (!v || !v->isNumber() || v->asNumber() != 1) throw JsonError("unsupported titan.behaviortree version");
    auto schema = std::make_shared<BlackboardSchema>(BlackboardSchema::fromJson(j["schema"]));
    const Json& ns = j["nodes"];
    if (!ns.isArray() || ns.size() > kMaxNodes) throw JsonError("titan.behaviortree needs a 'nodes' array (<= 4096)");
    std::vector<BtNode> nodes;
    for (const Json& o : ns.asArray()) {
        if (!o.isObject()) throw JsonError("behavior tree node must be an object");
        BtNode n;
        n.id = o["id"].asInt();
        n.kind = o["kind"].asString();
        const Json& cs = o["children"];
        if (!cs.isArray() || cs.size() > kMaxChildren) throw JsonError("node 'children' must be an array (<= 256)");
        for (const Json& c : cs.asArray()) n.children.push_back(c.asInt());
        n.params = o.contains("params") ? o["params"] : Json::object();
        nodes.push_back(std::move(n));
    }
    return BehaviorTree(std::move(schema), std::move(nodes), j["root"].asInt(), registry);
}

// ---- instance ------------------------------------------------------------------------------------------------------

BehaviorTreeInstance::BehaviorTreeInstance(std::shared_ptr<const BehaviorTree> tree,
                                           std::shared_ptr<const ActionRegistry> registry, uint64_t agent)
    : tree_(std::move(tree)), registry_(std::move(registry)), agent_(agent) {
    if (!tree_) fail("behavior tree instance needs a tree");
    state_.assign(tree_->compiled_.size(), NodeState{});
    actions_.resize(tree_->compiled_.size());
    for (size_t i = 0; i < tree_->compiled_.size(); ++i) {
        const auto& c = tree_->compiled_[i];
        if (c.kind != kAction) continue;
        if (!registry_) fail("behavior tree has action nodes but no registry was given");
        const std::string type = c.node->params.getString("type");
        const ActionType* t = registry_->find(type);
        if (!t) fail("unknown action type '" + type + "'");
        const Json* p = c.node->params.find("params");
        actions_[i] = t->create(p ? *p : Json::object());
        if (!actions_[i]) fail("action factory for '" + type + "' returned null");
    }
}

bool BehaviorTreeInstance::running() const { return !state_.empty() && state_[0].running; }

TickResult BehaviorTreeInstance::tick(Blackboard& bb, const TickBudget& budget) {
    if (&bb.schema() != &tree_->schema()) fail("blackboard is not bound to this tree's schema");
    if (budget.maxNodeVisits < 1 || budget.maxNodeVisits > (1u << 20)) fail("maxNodeVisits must be in [1, 2^20]");
    visitsLeft_ = budget.maxNodeVisits;
    exhausted_ = false;
    TickResult r;
    r.status = run(0, bb);
    r.visits = budget.maxNodeVisits - visitsLeft_;
    r.budgetExhausted = exhausted_;
    ++ticks_;
    return r;
}

void BehaviorTreeInstance::abort(Blackboard& bb) {
    if (&bb.schema() != &tree_->schema()) fail("blackboard is not bound to this tree's schema");
    halt(0, bb);
    std::fill(state_.begin(), state_.end(), NodeState{});
}

void BehaviorTreeInstance::halt(int i, Blackboard& bb) {
    NodeState& s = state_[size_t(i)];
    if (!s.running) return;
    const auto& c = tree_->compiled_[size_t(i)];
    for (int child : c.children) halt(child, bb);
    if (c.kind == kAction) {
        const Json* p = c.node->params.find("params");
        ActionContext ctx{bb, p ? *p : emptyObject(), agent_, ticks_};
        try {
            actions_[size_t(i)]->abort(ctx);
        } catch (const std::exception& e) {
            lastError_ = "node " + std::to_string(c.node->id) + " abort: " + e.what();
        } catch (...) {
            lastError_ = "node " + std::to_string(c.node->id) + " abort: unknown exception";
        }
    }
    s = NodeState{};
}

Status BehaviorTreeInstance::run(int i, Blackboard& bb) {
    if (visitsLeft_ == 0) {  // out of budget: report Running without touching this subtree; resume next tick
        exhausted_ = true;
        return Status::Running;
    }
    --visitsLeft_;
    NodeState& s = state_[size_t(i)];
    const auto& c = tree_->compiled_[size_t(i)];
    const Json& p = c.node->params;
    auto done = [&](Status st) {
        if (st != Status::Running) s = NodeState{};
        else s.running = true;
        return st;
    };
    switch (c.kind) {
        case kSequence:
        case kSelector: {
            const Status stop = c.kind == kSequence ? Status::Failure : Status::Success;
            for (size_t k = s.index; k < c.children.size(); ++k) {
                const Status st = run(c.children[k], bb);
                if (st == Status::Running) {
                    s.index = uint32_t(k);
                    return done(Status::Running);
                }
                if (st == stop) return done(stop);
            }
            return done(c.kind == kSequence ? Status::Success : Status::Failure);
        }
        case kInverter: {
            const Status st = run(c.children[0], bb);
            return done(st == Status::Running ? st : (st == Status::Success ? Status::Failure : Status::Success));
        }
        case kForceSuccess:
        case kForceFailure: {
            const Status st = run(c.children[0], bb);
            return done(st == Status::Running ? st : (c.kind == kForceSuccess ? Status::Success : Status::Failure));
        }
        case kRepeat:
        case kRetry: {
            const Status st = run(c.children[0], bb);
            if (st == Status::Running) return done(st);
            const bool counts = c.kind == kRepeat ? st == Status::Success : st == Status::Failure;
            if (!counts) return done(st);  // repeat: a failure ends it; retry: a success ends it
            const uint32_t limit = uint32_t(p["count"].asInt());
            const uint32_t next = s.counter + 1;
            if (next >= limit) return done(st);
            s.counter = next;
            return done(Status::Running);  // one completion per tick keeps every tick bounded
        }
        case kTimeout: {
            const uint32_t next = s.counter + 1;
            const Status st = run(c.children[0], bb);
            if (st != Status::Running) return done(st);
            if (next >= uint32_t(p["ticks"].asInt())) {
                halt(c.children[0], bb);
                return done(Status::Failure);
            }
            s.counter = next;
            return done(Status::Running);
        }
        case kGuard: {
            if (!evaluateCondition(p["condition"], bb)) {
                halt(c.children[0], bb);
                return done(Status::Failure);
            }
            return done(run(c.children[0], bb));
        }
        case kCondition: return done(evaluateCondition(p, bb) ? Status::Success : Status::Failure);
        case kWait: {
            const uint32_t next = s.counter + 1;
            if (next >= uint32_t(p["ticks"].asInt())) return done(Status::Success);
            s.counter = next;
            return done(Status::Running);
        }
        case kSet: {
            const std::string& key = p["key"].asString();
            bb.set(key, valueFromJson(p["value"], bb.schema().find(key)->type));
            return done(Status::Success);
        }
        case kClear:
            bb.unset(p["key"].asString());
            return done(Status::Success);
        case kAction: {
            const Json* ap = p.find("params");
            ActionContext ctx{bb, ap ? *ap : emptyObject(), agent_, ticks_};
            Status st;
            try {
                st = actions_[size_t(i)]->tick(ctx);
            } catch (const std::exception& e) {
                lastError_ = "node " + std::to_string(c.node->id) + ": " + e.what();
                st = Status::Failure;
            } catch (...) {
                lastError_ = "node " + std::to_string(c.node->id) + ": unknown exception";
                st = Status::Failure;
            }
            if (st != Status::Success && st != Status::Failure && st != Status::Running) st = Status::Failure;
            return done(st);
        }
        default: break;
    }
    return done(Status::Failure);
}

}  // namespace titan::ai
