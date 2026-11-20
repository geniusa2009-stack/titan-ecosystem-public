// TITAN Engine — utility-based NPC decisions: weighted considerations, response curves, prerequisites, cooldowns,
// commitment and hysteresis, with explainable results. Original TITAN code.
//
// Scoring. Each consideration reads one blackboard key (int / float, or bool as 0 / 1), normalizes it to [0, 1]
// over [min, max] (clamped), and maps it through a response curve to a score in [0, 1]. An action's score is
//     weight * weighted geometric mean of its consideration scores        (1 when it has none)
// so one zero consideration vetoes the action and weights express relative importance. An unset key uses the
// consideration's missing-data policy (score 0 by default: no evidence, no action) and is flagged in the
// explanation.
//
// Selection (UtilityReasoner::decide), deterministic for the same inputs:
//   1. Every action is evaluated; it is eligible when all prerequisites (blackboard conditions, as in behavior
//      trees) hold, it is not cooling down, and its score exceeds model.minScore.
//   2. The current action ends when it is no longer eligible.
//   3. The best eligible action wins by (score, priority, definition order). A different best action replaces the
//      current one only after the current action's minCommitSeconds and only if it beats the current score by
//      more than model.switchMargin.
//   4. An ended action may not start again until its cooldownSeconds have passed.
// Every Decision carries a reason and per-action / per-consideration details for debugging tools.
// Bounds: <= 256 actions, <= 32 considerations and <= 16 prerequisites per action. Time is caller-supplied seconds
// and must be finite and non-decreasing.
// Thread safety: the model is immutable and shareable; a reasoner (per-agent state) belongs to one thread.
#pragma once

#include "titan/behavior_tree.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace titan::ai {

struct ResponseCurve {
    enum class Kind { Linear, Power, Logistic, Step, Piecewise };
    Kind kind = Kind::Linear;
    double slope = 1, intercept = 0;           // linear: slope * x + intercept, clamped to [0, 1]
    double exponent = 1;                       // power: x^exponent, exponent in (0, 16]
    double steepness = 10, midpoint = 0.5;     // logistic: 1 / (1 + e^(-steepness * (x - midpoint)))
    double threshold = 0.5, low = 0, high = 1;  // step: x >= threshold ? high : low
    std::vector<std::pair<double, double>> points;  // piecewise linear, 2..32 points, x strictly increasing
    bool invert = false;                       // 1 - y

    void validate() const;            // std::invalid_argument
    double evaluate(double x) const;  // x is clamped to [0, 1]; the result is in [0, 1]
    Json toJson() const;
    static ResponseCurve fromJson(const Json& j);
};

struct Consideration {
    enum class Missing { Zero, One, Value };
    std::string name;
    std::string key;            // numeric or bool blackboard key
    double min = 0, max = 1;    // input range, min < max
    double weight = 1;          // [0, 10]; 0 disables the consideration
    ResponseCurve curve;
    Missing missing = Missing::Zero;
    double missingValue = 0;    // score used by Missing::Value, [0, 1]
};

struct UtilityAction {
    std::string name;
    double weight = 1;              // [0, 10]
    int priority = 0;               // tie-break, higher first, [-1000, 1000]
    double cooldownSeconds = 0;     // [0, 1e6]
    double minCommitSeconds = 0;    // [0, 1e6]
    std::vector<Json> prerequisites;  // blackboard conditions
    std::vector<Consideration> considerations;
};

class UtilityModel {
public:
    // Validates everything against the schema; std::invalid_argument names the action / consideration at fault.
    UtilityModel(std::shared_ptr<const BlackboardSchema> schema, std::vector<UtilityAction> actions,
                 double minScore = 0, double switchMargin = 0.05);

    const BlackboardSchema& schema() const { return *schema_; }
    const std::vector<UtilityAction>& actions() const { return actions_; }
    double minScore() const { return minScore_; }
    double switchMargin() const { return switchMargin_; }
    int find(const std::string& action) const;  // -1 when absent

    // {"format":"titan.utility","version":1,"schema":{...},"minScore","switchMargin","actions":[...]}
    Json toJson() const;
    static UtilityModel fromJson(const Json& j);

private:
    std::shared_ptr<const BlackboardSchema> schema_;
    std::vector<UtilityAction> actions_;
    double minScore_, switchMargin_;
};

struct ConsiderationEval {
    std::string name;
    std::optional<double> input;  // raw value; nullopt when the key was unset
    double normalized = 0;
    double score = 0;
    double weight = 0;
    bool missing = false;
};

struct ActionEval {
    std::string name;
    double score = 0;
    bool eligible = false;
    std::string blockedBy;  // "", "prerequisite <n>", "cooldown", "score"
    std::vector<ConsiderationEval> considerations;
};

struct Decision {
    std::string action;   // empty when nothing is chosen
    bool switched = false;
    std::string reason;   // "started", "kept", "switched", "commitment", "hysteresis", "none eligible"
    std::vector<ActionEval> evaluations;  // in definition order
};

class UtilityReasoner {
public:
    explicit UtilityReasoner(std::shared_ptr<const UtilityModel> model);  // std::invalid_argument for null

    // std::invalid_argument (state unchanged) for a non-finite or decreasing `now`, or a blackboard bound to
    // another schema.
    Decision decide(const Blackboard& bb, double now);
    void reset();  // forget the current action, timers and cooldowns
    const std::string& current() const;  // empty when idle

    // Per-agent state: {"format":"titan.utility-state","version":1,"current","startedAt","lastNow","endedAt":{...}}
    Json saveState() const;
    void loadState(const Json& j);  // validated against the model; all or nothing

    static ActionEval evaluate(const UtilityAction& action, const Blackboard& bb);  // score and details only

private:
    std::shared_ptr<const UtilityModel> model_;
    int current_ = -1;
    double startedAt_ = 0;
    std::optional<double> lastNow_;
    std::vector<std::optional<double>> endedAt_;
};

}  // namespace titan::ai
