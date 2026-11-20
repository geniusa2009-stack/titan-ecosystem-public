// TITAN Engine — input mapping: named actions, bindings from physical controls and prioritized input contexts.
// Original TITAN code.
//
// Controls are named "<Device>.<Name>" with Device one of Keyboard, Mouse, Gamepad (e.g. "Keyboard.W",
// "Gamepad.LeftX"). A context groups bindings (control -> action, with a scale) and has a priority. When several
// active contexts bind the same control, only the highest-priority one sees it (ties: the most recently pushed),
// so a "Menu" context can take Escape away from "Gameplay" without either knowing about the other.
// Evaluation is a pure function of the active contexts and the control values:
//   Button actions -> 1 when any claimed |value * scale| >= 0.5, else 0
//   Axis actions   -> sum of value * scale over claimed bindings, clamped to [-1, 1]
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"

#include <map>
#include <string>
#include <vector>

namespace titan {

enum class ActionKind { Button, Axis };

struct InputAction {
    std::string name;
    ActionKind kind = ActionKind::Button;
};

struct InputBinding {
    std::string control;
    std::string action;
    float scale = 1.0f;  // finite, non-zero
};

struct InputContext {
    std::string name;
    int priority = 0;
    std::vector<InputBinding> bindings;
};

struct InputMap {
    std::vector<InputAction> actions;
    std::vector<InputContext> contexts;

    // std::invalid_argument: bad or duplicate action/context names, malformed controls, bindings to unknown
    // actions, non-finite or zero scales, or the same (control, action) bound twice in one context.
    void validate() const;
    const InputContext* findContext(const std::string& name) const;

    // {"format":"titan.input","version":1,"actions":[{"name","kind"}],"contexts":[{"name","priority","bindings"}]}
    Json toJson() const;
    static InputMap fromJson(const Json& j);  // validates
};

bool isValidControlName(const std::string& control);

class InputRouter {
public:
    explicit InputRouter(InputMap map);  // validates

    // std::invalid_argument for an unknown context or one that is already active / not active.
    void push(const std::string& context);
    void pop(const std::string& context);
    const std::vector<std::string>& active() const { return active_; }  // push order

    // Value of every action (0 when nothing claims it). Unknown controls are ignored; non-finite values throw
    // std::invalid_argument.
    std::map<std::string, float> evaluate(const std::map<std::string, float>& controls) const;

private:
    InputMap map_;
    std::vector<std::string> active_;
};

}  // namespace titan
