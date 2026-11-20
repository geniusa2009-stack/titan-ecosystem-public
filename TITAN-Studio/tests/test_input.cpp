// TITAN input mapping tests. Original TITAN code.
#include "titan/input_map.h"
#include "titan_test.h"

using namespace titan;

namespace {
InputMap sampleMap() {
    InputMap m;
    m.actions = {{"Jump", ActionKind::Button}, {"MoveX", ActionKind::Axis}, {"Pause", ActionKind::Button},
                 {"MenuBack", ActionKind::Button}};
    InputContext game;
    game.name = "Gameplay";
    game.priority = 0;
    game.bindings = {{"Keyboard.Space", "Jump", 1},     {"Gamepad.A", "Jump", 1},
                     {"Keyboard.D", "MoveX", 1},        {"Keyboard.A", "MoveX", -1},
                     {"Gamepad.LeftX", "MoveX", 1},     {"Keyboard.Escape", "Pause", 1}};
    InputContext menu;
    menu.name = "Menu";
    menu.priority = 10;
    menu.bindings = {{"Keyboard.Escape", "MenuBack", 1}};
    m.contexts = {game, menu};
    return m;
}
}  // namespace

TEST("input: actions resolve from bindings with axis sums and clamping") {
    InputRouter r(sampleMap());
    r.push("Gameplay");
    std::map<std::string, float> c;
    c["Keyboard.Space"] = 1;
    c["Keyboard.D"] = 1;
    c["Gamepad.LeftX"] = 0.75f;
    auto out = r.evaluate(c);
    CHECK(out.at("Jump") == 1);
    CHECK(out.at("MoveX") == 1);  // 1 + 0.75 clamped
    CHECK(out.at("Pause") == 0);
    c["Keyboard.A"] = 1;
    c["Gamepad.LeftX"] = -0.25f;
    out = r.evaluate(c);
    CHECK_NEAR(out.at("MoveX"), -0.25, 1e-6);
    std::map<std::string, float> weak;
    weak["Gamepad.A"] = 0.4f;
    weak["Gamepad.Unbound"] = 1;
    out = r.evaluate(weak);
    CHECK(out.at("Jump") == 0);
    CHECK(out.size() == 4);
}

TEST("input: higher-priority contexts take shared controls; inactive contexts see nothing") {
    InputRouter r(sampleMap());
    std::map<std::string, float> esc;
    esc["Keyboard.Escape"] = 1;
    CHECK(r.evaluate(esc).at("Pause") == 0);  // no context active
    r.push("Gameplay");
    CHECK(r.evaluate(esc).at("Pause") == 1);
    r.push("Menu");
    auto out = r.evaluate(esc);
    CHECK(out.at("MenuBack") == 1 && out.at("Pause") == 0);
    std::map<std::string, float> space;
    space["Keyboard.Space"] = 1;
    CHECK(r.evaluate(space).at("Jump") == 1);  // controls the menu does not bind still reach gameplay
    r.pop("Menu");
    out = r.evaluate(esc);
    CHECK(out.at("MenuBack") == 0 && out.at("Pause") == 1);
    CHECK_THROWS(r.pop("Menu"));
    CHECK_THROWS(r.push("Gameplay"));
    CHECK_THROWS(r.push("Vehicle"));
}

TEST("input: equal priorities resolve to the most recently pushed context") {
    InputMap m = sampleMap();
    InputContext vehicle;
    vehicle.name = "Vehicle";
    vehicle.priority = 0;
    vehicle.bindings = {{"Keyboard.Space", "Pause", 1}};
    m.contexts.push_back(vehicle);
    InputRouter r(m);
    r.push("Gameplay");
    r.push("Vehicle");
    std::map<std::string, float> space;
    space["Keyboard.Space"] = 1;
    auto out = r.evaluate(space);
    CHECK(out.at("Pause") == 1 && out.at("Jump") == 0);
    std::map<std::string, float> bad;
    bad["Keyboard.D"] = std::nanf("");
    CHECK_THROWS(r.evaluate(bad));
}

TEST("input: validation refuses malformed maps") {
    InputMap dupAction = sampleMap();
    dupAction.actions.push_back({"Jump", ActionKind::Axis});
    CHECK_THROWS(dupAction.validate());
    InputMap badControl = sampleMap();
    badControl.contexts[0].bindings.push_back({"Joystick.Fire", "Jump", 1});
    CHECK_THROWS(badControl.validate());
    InputMap noName = sampleMap();
    noName.contexts[0].bindings.push_back({"Keyboard.", "Jump", 1});
    CHECK_THROWS(noName.validate());
    InputMap unknownAction = sampleMap();
    unknownAction.contexts[0].bindings.push_back({"Keyboard.Q", "Fly", 1});
    CHECK_THROWS(unknownAction.validate());
    InputMap zeroScale = sampleMap();
    zeroScale.contexts[0].bindings.push_back({"Keyboard.Q", "MoveX", 0});
    CHECK_THROWS(zeroScale.validate());
    InputMap twice = sampleMap();
    twice.contexts[0].bindings.push_back({"Keyboard.D", "MoveX", 0.5f});
    CHECK_THROWS(twice.validate());
    InputMap dupContext = sampleMap();
    dupContext.contexts.push_back(dupContext.contexts[0]);
    CHECK_THROWS(dupContext.validate());
    CHECK_THROWS(InputRouter(dupContext));
}

TEST("input: json round trip") {
    const InputMap m = sampleMap();
    const Json j = m.toJson();
    const InputMap back = InputMap::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.findContext("Menu")->priority == 10);
    Json badKind = j;
    badKind["actions"].asArray()[0]["kind"] = "trigger";
    CHECK_THROWS(InputMap::fromJson(badKind));
}

TITAN_TEST_MAIN()
