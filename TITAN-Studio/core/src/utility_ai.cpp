// TITAN Engine — utility-based NPC decisions. Original TITAN code.
#include "titan/utility_ai.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace titan::ai {
namespace {

void fail(const std::string& msg) { throw std::invalid_argument(msg); }
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }  // false for NaN
double clamp01(double v) { return v >= 0 ? (v <= 1 ? v : 1) : 0; }       // NaN -> 0

const char* kindName(ResponseCurve::Kind k) {
    switch (k) {
        case ResponseCurve::Kind::Linear: return "linear";
        case ResponseCurve::Kind::Power: return "power";
        case ResponseCurve::Kind::Logistic: return "logistic";
        case ResponseCurve::Kind::Step: return "step";
        case ResponseCurve::Kind::Piecewise: return "piecewise";
    }
    return "linear";
}
ResponseCurve::Kind kindFrom(const std::string& s) {
    for (auto k : {ResponseCurve::Kind::Linear, ResponseCurve::Kind::Power, ResponseCurve::Kind::Logistic,
                   ResponseCurve::Kind::Step, ResponseCurve::Kind::Piecewise})
        if (s == kindName(k)) return k;
    throw JsonError("unknown response curve '" + s + "'");
}
const char* missingName(Consideration::Missing m) {
    return m == Consideration::Missing::One ? "one" : (m == Consideration::Missing::Value ? "value" : "zero");
}
Consideration::Missing missingFrom(const std::string& s) {
    if (s == "zero") return Consideration::Missing::Zero;
    if (s == "one") return Consideration::Missing::One;
    if (s == "value") return Consideration::Missing::Value;
    throw JsonError("unknown missing-data policy '" + s + "'");
}

void checkVersion(const Json& j, const char* format) {
    if (!j.isObject() || j.getString("format") != format) throw JsonError(std::string("not a ") + format + " document");
    const Json* v = j.find("version");
    if (!v || !v->isNumber() || v->asNumber() != 1) throw JsonError(std::string("unsupported ") + format + " version");
}

}  // namespace

// ---- curves --------------------------------------------------------------------------------------------------------

void ResponseCurve::validate() const {
    switch (kind) {
        case Kind::Linear:
            if (!inRange(slope, -1e6, 1e6) || !inRange(intercept, -1e6, 1e6)) fail("linear curve needs finite slope and intercept");
            return;
        case Kind::Power:
            if (!(exponent > 0) || !(exponent <= 16)) fail("power curve exponent must be in (0, 16]");
            return;
        case Kind::Logistic:
            if (!inRange(steepness, -100, 100) || !inRange(midpoint, 0, 1)) fail("logistic curve needs steepness in [-100, 100] and midpoint in [0, 1]");
            return;
        case Kind::Step:
            if (!inRange(threshold, 0, 1) || !inRange(low, 0, 1) || !inRange(high, 0, 1)) fail("step curve values must be in [0, 1]");
            return;
        case Kind::Piecewise:
            if (points.size() < 2 || points.size() > 32) fail("piecewise curve needs 2 to 32 points");
            for (size_t i = 0; i < points.size(); ++i) {
                if (!inRange(points[i].first, 0, 1) || !inRange(points[i].second, 0, 1)) fail("piecewise points must be in [0, 1]");
                if (i > 0 && !(points[i].first > points[i - 1].first)) fail("piecewise x values must strictly increase");
            }
            return;
    }
    fail("unknown response curve kind");
}

double ResponseCurve::evaluate(double x) const {
    x = clamp01(x);
    double y = 0;
    switch (kind) {
        case Kind::Linear: y = slope * x + intercept; break;
        case Kind::Power: y = std::pow(x, exponent); break;
        case Kind::Logistic: y = 1.0 / (1.0 + std::exp(-steepness * (x - midpoint))); break;
        case Kind::Step: y = x >= threshold ? high : low; break;
        case Kind::Piecewise: {
            if (points.empty()) break;
            if (x <= points.front().first) { y = points.front().second; break; }
            if (x >= points.back().first) { y = points.back().second; break; }
            auto it = std::upper_bound(points.begin(), points.end(), x,
                                       [](double v, const std::pair<double, double>& p) { return v < p.first; });
            const auto& [x1, y1] = *it;
            const auto& [x0, y0] = *(it - 1);
            y = y0 + (y1 - y0) * ((x - x0) / (x1 - x0));
            break;
        }
    }
    y = clamp01(y);
    return invert ? 1 - y : y;
}

Json ResponseCurve::toJson() const {
    Json j = Json::object();
    j["kind"] = kindName(kind);
    switch (kind) {
        case Kind::Linear: j["slope"] = slope; j["intercept"] = intercept; break;
        case Kind::Power: j["exponent"] = exponent; break;
        case Kind::Logistic: j["steepness"] = steepness; j["midpoint"] = midpoint; break;
        case Kind::Step: j["threshold"] = threshold; j["low"] = low; j["high"] = high; break;
        case Kind::Piecewise: {
            Json ps = Json::array();
            for (const auto& [x, y] : points) ps.push_back(Json(Json::Array{Json(x), Json(y)}));
            j["points"] = std::move(ps);
            break;
        }
    }
    if (invert) j["invert"] = true;
    return j;
}

ResponseCurve ResponseCurve::fromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("response curve must be an object");
    ResponseCurve c;
    c.kind = kindFrom(j["kind"].asString());
    c.slope = j.getNumber("slope", 1);
    c.intercept = j.getNumber("intercept", 0);
    c.exponent = j.getNumber("exponent", 1);
    c.steepness = j.getNumber("steepness", 10);
    c.midpoint = j.getNumber("midpoint", 0.5);
    c.threshold = j.getNumber("threshold", 0.5);
    c.low = j.getNumber("low", 0);
    c.high = j.getNumber("high", 1);
    if (const Json* ps = j.find("points")) {
        if (!ps->isArray() || ps->size() > 32) throw JsonError("curve points must be an array of at most 32");
        for (const Json& p : ps->asArray()) {
            if (!p.isArray() || p.size() != 2) throw JsonError("curve point must be [x, y]");
            c.points.emplace_back(p.at(0).asNumber(), p.at(1).asNumber());
        }
    }
    c.invert = j.getBool("invert", false);
    c.validate();
    return c;
}

// ---- model ---------------------------------------------------------------------------------------------------------

UtilityModel::UtilityModel(std::shared_ptr<const BlackboardSchema> schema, std::vector<UtilityAction> actions,
                           double minScore, double switchMargin)
    : schema_(std::move(schema)), actions_(std::move(actions)), minScore_(minScore), switchMargin_(switchMargin) {
    if (!schema_) fail("utility model needs a blackboard schema");
    if (actions_.empty() || actions_.size() > 256) fail("utility models need 1 to 256 actions");
    if (!inRange(minScore_, 0, 10)) fail("minScore must be in [0, 10]");
    if (!inRange(switchMargin_, 0, 10)) fail("switchMargin must be in [0, 10]");
    std::set<std::string> names;
    for (const UtilityAction& a : actions_) {
        if (!isValidKeyName(a.name)) fail("invalid utility action name '" + a.name + "'");
        if (!names.insert(a.name).second) fail("duplicate utility action '" + a.name + "'");
        const std::string w = "utility action '" + a.name + "': ";
        if (!inRange(a.weight, 0, 10)) fail(w + "weight must be in [0, 10]");
        if (a.priority < -1000 || a.priority > 1000) fail(w + "priority must be in [-1000, 1000]");
        if (!inRange(a.cooldownSeconds, 0, 1e6) || !inRange(a.minCommitSeconds, 0, 1e6))
            fail(w + "cooldownSeconds and minCommitSeconds must be in [0, 1e6]");
        if (a.prerequisites.size() > 16) fail(w + "at most 16 prerequisites");
        for (size_t k = 0; k < a.prerequisites.size(); ++k)
            validateCondition(a.prerequisites[k], *schema_, w + "prerequisite " + std::to_string(k) + ": ");
        if (a.considerations.size() > 32) fail(w + "at most 32 considerations");
        std::set<std::string> cnames;
        for (const Consideration& c : a.considerations) {
            const std::string cw = w + "consideration '" + c.name + "': ";
            if (!isValidKeyName(c.name)) fail(cw + "invalid name");
            if (!cnames.insert(c.name).second) fail(cw + "duplicate name");
            const BbKey* key = schema_->find(c.key);
            if (!key) fail(cw + "unknown blackboard key '" + c.key + "'");
            if (key->type != BbType::Int && key->type != BbType::Float && key->type != BbType::Bool)
                fail(cw + "key '" + c.key + "' must be int, float or bool");
            if (!inRange(c.min, -1e15, 1e15) || !inRange(c.max, -1e15, 1e15) || !(c.min < c.max))
                fail(cw + "needs finite min < max");
            if (!inRange(c.weight, 0, 10)) fail(cw + "weight must be in [0, 10]");
            if (!inRange(c.missingValue, 0, 1)) fail(cw + "missingValue must be in [0, 1]");
            try {
                c.curve.validate();
            } catch (const std::exception& e) {
                fail(cw + e.what());
            }
        }
    }
}

int UtilityModel::find(const std::string& action) const {
    for (size_t i = 0; i < actions_.size(); ++i)
        if (actions_[i].name == action) return int(i);
    return -1;
}

Json UtilityModel::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.utility";
    j["version"] = 1;
    j["schema"] = schema_->toJson();
    j["minScore"] = minScore_;
    j["switchMargin"] = switchMargin_;
    Json as = Json::array();
    for (const UtilityAction& a : actions_) {
        Json o = Json::object();
        o["name"] = a.name;
        o["weight"] = a.weight;
        o["priority"] = a.priority;
        o["cooldownSeconds"] = a.cooldownSeconds;
        o["minCommitSeconds"] = a.minCommitSeconds;
        Json pre = Json::array();
        for (const Json& p : a.prerequisites) pre.push_back(p);
        o["prerequisites"] = std::move(pre);
        Json cs = Json::array();
        for (const Consideration& c : a.considerations) {
            Json cj = Json::object();
            cj["name"] = c.name;
            cj["key"] = c.key;
            cj["min"] = c.min;
            cj["max"] = c.max;
            cj["weight"] = c.weight;
            cj["curve"] = c.curve.toJson();
            cj["missing"] = missingName(c.missing);
            cj["missingValue"] = c.missingValue;
            cs.push_back(std::move(cj));
        }
        o["considerations"] = std::move(cs);
        as.push_back(std::move(o));
    }
    j["actions"] = std::move(as);
    return j;
}

UtilityModel UtilityModel::fromJson(const Json& j) {
    checkVersion(j, "titan.utility");
    auto schema = std::make_shared<BlackboardSchema>(BlackboardSchema::fromJson(j["schema"]));
    const Json& as = j["actions"];
    if (!as.isArray() || as.size() > 256) throw JsonError("titan.utility needs an 'actions' array (<= 256)");
    std::vector<UtilityAction> actions;
    for (const Json& o : as.asArray()) {
        if (!o.isObject()) throw JsonError("utility action must be an object");
        UtilityAction a;
        a.name = o["name"].asString();
        a.weight = o.getNumber("weight", 1);
        a.priority = int(std::clamp<int64_t>(o.contains("priority") ? o["priority"].asInt() : 0, -100000, 100000));
        a.cooldownSeconds = o.getNumber("cooldownSeconds", 0);
        a.minCommitSeconds = o.getNumber("minCommitSeconds", 0);
        if (const Json* pre = o.find("prerequisites")) {
            if (!pre->isArray() || pre->size() > 16) throw JsonError("prerequisites must be an array (<= 16)");
            for (const Json& p : pre->asArray()) a.prerequisites.push_back(p);
        }
        if (const Json* cs = o.find("considerations")) {
            if (!cs->isArray() || cs->size() > 32) throw JsonError("considerations must be an array (<= 32)");
            for (const Json& cj : cs->asArray()) {
                if (!cj.isObject()) throw JsonError("consideration must be an object");
                Consideration c;
                c.name = cj["name"].asString();
                c.key = cj["key"].asString();
                c.min = cj["min"].asNumber();
                c.max = cj["max"].asNumber();
                c.weight = cj.getNumber("weight", 1);
                c.curve = ResponseCurve::fromJson(cj["curve"]);
                c.missing = missingFrom(cj.getString("missing", "zero"));
                c.missingValue = cj.getNumber("missingValue", 0);
                a.considerations.push_back(std::move(c));
            }
        }
        actions.push_back(std::move(a));
    }
    return UtilityModel(std::move(schema), std::move(actions), j.getNumber("minScore", 0), j.getNumber("switchMargin", 0.05));
}

// ---- reasoner ------------------------------------------------------------------------------------------------------

UtilityReasoner::UtilityReasoner(std::shared_ptr<const UtilityModel> model) : model_(std::move(model)) {
    if (!model_) fail("utility reasoner needs a model");
    endedAt_.assign(model_->actions().size(), std::nullopt);
}

const std::string& UtilityReasoner::current() const {
    static const std::string none;
    return current_ < 0 ? none : model_->actions()[size_t(current_)].name;
}

void UtilityReasoner::reset() {
    current_ = -1;
    startedAt_ = 0;
    lastNow_.reset();
    std::fill(endedAt_.begin(), endedAt_.end(), std::nullopt);
}

ActionEval UtilityReasoner::evaluate(const UtilityAction& a, const Blackboard& bb) {
    ActionEval ev;
    ev.name = a.name;
    double sumW = 0, sumLog = 0;
    bool veto = false;
    for (const Consideration& c : a.considerations) {
        ConsiderationEval ce;
        ce.name = c.name;
        ce.weight = c.weight;
        const BbValue* v = bb.get(c.key);
        if (!v) {
            ce.missing = true;
            ce.score = c.missing == Consideration::Missing::One ? 1.0
                       : (c.missing == Consideration::Missing::Value ? c.missingValue : 0.0);
        } else {
            const double raw = std::holds_alternative<bool>(*v) ? (std::get<bool>(*v) ? 1.0 : 0.0)
                               : std::holds_alternative<int64_t>(*v) ? double(std::get<int64_t>(*v))
                                                                    : std::get<double>(*v);
            ce.input = raw;
            ce.normalized = clamp01((raw - c.min) / (c.max - c.min));
            ce.score = c.curve.evaluate(ce.normalized);
        }
        if (c.weight > 0) {
            if (!(ce.score > 0)) veto = true;
            else sumLog += c.weight * std::log(ce.score);
            sumW += c.weight;
        }
        ev.considerations.push_back(std::move(ce));
    }
    ev.score = veto ? 0.0 : a.weight * (sumW > 0 ? std::exp(sumLog / sumW) : 1.0);
    return ev;
}

Decision UtilityReasoner::decide(const Blackboard& bb, double now) {
    if (&bb.schema() != &model_->schema()) fail("blackboard is not bound to the utility model's schema");
    if (!std::isfinite(now)) fail("decision time must be finite");
    if (lastNow_ && now < *lastNow_) fail("decision time must not go backwards");
    const auto& actions = model_->actions();
    Decision d;
    d.evaluations.reserve(actions.size());
    for (size_t i = 0; i < actions.size(); ++i) {
        const UtilityAction& a = actions[i];
        ActionEval ev = evaluate(a, bb);
        for (size_t k = 0; k < a.prerequisites.size() && ev.blockedBy.empty(); ++k)
            if (!evaluateCondition(a.prerequisites[k], bb)) ev.blockedBy = "prerequisite " + std::to_string(k);
        if (ev.blockedBy.empty() && int(i) != current_ && endedAt_[i] && now - *endedAt_[i] < a.cooldownSeconds)
            ev.blockedBy = "cooldown";
        if (ev.blockedBy.empty() && !(ev.score > model_->minScore())) ev.blockedBy = "score";
        ev.eligible = ev.blockedBy.empty();
        d.evaluations.push_back(std::move(ev));
    }

    // Decide on copies; commit at the end.
    int cur = current_;
    double started = startedAt_;
    std::vector<std::optional<double>> ended = endedAt_;
    const bool hadCurrent = cur >= 0;
    if (cur >= 0 && !d.evaluations[size_t(cur)].eligible) {
        ended[size_t(cur)] = now;
        cur = -1;
    }
    int best = -1;
    for (size_t i = 0; i < actions.size(); ++i) {
        const ActionEval& ev = d.evaluations[i];
        if (!ev.eligible) continue;
        if (best < 0) { best = int(i); continue; }
        const ActionEval& b = d.evaluations[size_t(best)];
        if (ev.score > b.score || (ev.score == b.score && actions[i].priority > actions[size_t(best)].priority))
            best = int(i);  // equal score and priority: the earlier definition stays
    }
    if (cur < 0) {
        if (best < 0) {
            d.reason = "none eligible";
        } else {
            cur = best;
            started = now;
            d.reason = hadCurrent ? "switched" : "started";
        }
    } else if (best == cur) {
        d.reason = "kept";
    } else if (now - started < actions[size_t(cur)].minCommitSeconds) {
        d.reason = "commitment";
    } else if (d.evaluations[size_t(best)].score > d.evaluations[size_t(cur)].score + model_->switchMargin()) {
        ended[size_t(cur)] = now;
        cur = best;
        started = now;
        d.reason = "switched";
    } else {
        d.reason = "hysteresis";
    }
    d.switched = hadCurrent && cur != current_;
    d.action = cur < 0 ? std::string() : actions[size_t(cur)].name;
    current_ = cur;
    startedAt_ = started;
    endedAt_ = std::move(ended);
    lastNow_ = now;
    return d;
}

Json UtilityReasoner::saveState() const {
    Json j = Json::object();
    j["format"] = "titan.utility-state";
    j["version"] = 1;
    j["current"] = current();
    j["startedAt"] = startedAt_;
    if (lastNow_) j["lastNow"] = *lastNow_;
    Json e = Json::object();
    for (size_t i = 0; i < endedAt_.size(); ++i)
        if (endedAt_[i]) e[model_->actions()[i].name] = *endedAt_[i];
    j["endedAt"] = std::move(e);
    return j;
}

void UtilityReasoner::loadState(const Json& j) {
    checkVersion(j, "titan.utility-state");
    const std::string name = j["current"].asString();
    const int cur = name.empty() ? -1 : model_->find(name);
    if (!name.empty() && cur < 0) fail("state names an unknown action '" + name + "'");
    const double started = j["startedAt"].asNumber();
    std::optional<double> last;
    if (const Json* l = j.find("lastNow")) last = l->asNumber();
    if (!std::isfinite(started) || (last && (!std::isfinite(*last) || started > *last))) fail("invalid state times");
    if (cur >= 0 && !last) fail("a current action needs lastNow");
    std::vector<std::optional<double>> ended(model_->actions().size());
    const Json& e = j["endedAt"];
    if (!e.isObject()) throw JsonError("state needs an 'endedAt' object");
    for (const auto& [action, t] : e.asObject()) {
        const int i = model_->find(action);
        if (i < 0) fail("state names an unknown action '" + action + "'");
        const double v = t.asNumber();
        if (!std::isfinite(v) || !last || v > *last) fail("invalid end time for '" + action + "'");
        ended[size_t(i)] = v;
    }
    current_ = cur;
    startedAt_ = started;
    lastNow_ = last;
    endedAt_ = std::move(ended);
}

}  // namespace titan::ai
