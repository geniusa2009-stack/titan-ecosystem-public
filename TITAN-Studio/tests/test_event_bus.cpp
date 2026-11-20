// TITAN gameplay event bus tests. Original TITAN code.
#include "titan/event_bus.h"
#include "titan_test.h"

using namespace titan;

namespace {
GameEvent ev(const char* type, ObjectId source = kNoObject) {
    GameEvent e;
    e.type = type;
    e.source = source;
    return e;
}
}  // namespace

TEST("event bus: queued FIFO delivery in subscription order with source filters") {
    EventBus bus;
    std::vector<std::string> log;
    bus.subscribe("damage", [&](const GameEvent& e, EventBus&) { log.push_back("a:" + std::to_string(e.source)); });
    bus.subscribe("damage", [&](const GameEvent& e, EventBus&) { log.push_back("b:" + std::to_string(e.source)); }, 7);
    bus.subscribe("heal", [&](const GameEvent&, EventBus&) { log.push_back("heal"); });
    bus.post(ev("damage", 3));
    bus.post(ev("heal"));
    bus.post(ev("damage", 7));
    CHECK(log.empty());  // post only queues
    CHECK(bus.pending() == 3);
    CHECK(bus.dispatch() == 3);
    const std::vector<std::string> want{"a:3", "heal", "a:7", "b:7"};
    CHECK(log == want);
    CHECK(bus.pending() == 0 && bus.errors().empty());
}

TEST("event bus: events posted by handlers queue behind current ones and respect the budget") {
    EventBus bus(5);
    std::vector<std::string> log;
    bus.subscribe("ping", [&](const GameEvent&, EventBus& b) { log.push_back("ping"); b.post(ev("pong")); });
    bus.subscribe("pong", [&](const GameEvent&, EventBus& b) { log.push_back("pong"); b.post(ev("ping")); });
    bus.subscribe("other", [&](const GameEvent&, EventBus&) { log.push_back("other"); });
    bus.post(ev("ping"));
    bus.post(ev("other"));
    CHECK(bus.dispatch() == 5);
    const std::vector<std::string> want{"ping", "other", "pong", "ping", "pong"};
    CHECK(log == want);
    CHECK(bus.pending() == 1);  // the endless chain continues next frame instead of hanging this one
    CHECK(bus.dispatch() == 5);
    CHECK(log.size() == 10);
}

TEST("event bus: unsubscribe and subscribe during delivery are safe and immediate") {
    EventBus bus;
    std::vector<std::string> log;
    SubscriptionId second = 0;
    SubscriptionId self = 0;
    self = bus.subscribe("tick", [&](const GameEvent&, EventBus& b) {
        log.push_back("first");
        CHECK(b.unsubscribe(self));    // removes itself while running
        CHECK(b.unsubscribe(second));  // and a later handler for this same event
        b.subscribe("tick", [&](const GameEvent&, EventBus&) { log.push_back("late"); });
    });
    second = bus.subscribe("tick", [&](const GameEvent&, EventBus&) { log.push_back("second"); });
    bus.post(ev("tick"));
    bus.dispatch();
    CHECK(log.size() == 1 && log[0] == "first");  // neither the removed nor the new handler saw the event
    bus.post(ev("tick"));
    bus.dispatch();
    CHECK(log.size() == 2 && log[1] == "late");
    CHECK(bus.subscriptionCount() == 1);
    CHECK(!bus.unsubscribe(9999));
    CHECK(bus.errors().empty());  // CHECKs inside handlers would surface here
}

TEST("event bus: handler failures and re-entrant dispatch are contained") {
    EventBus bus;
    int after = 0;
    const SubscriptionId bad = bus.subscribe("hit", [](const GameEvent&, EventBus&) { throw std::runtime_error("boom"); });
    bus.subscribe("hit", [&](const GameEvent&, EventBus& b) { (void)b.dispatch(); });
    bus.subscribe("hit", [&](const GameEvent&, EventBus&) { ++after; });
    bus.post(ev("hit"));
    bus.post(ev("hit"));
    CHECK(bus.dispatch() == 2);
    CHECK(after == 2);
    CHECK(bus.errors().size() == 4);
    CHECK(bus.errors()[0].subscription == bad && bus.errors()[0].message == "boom");
    CHECK(bus.errors()[1].message.find("re-entrant") != std::string::npos);
    bus.clearErrors();
    CHECK(bus.errors().empty());
}

TEST("event bus: validation and queue limits") {
    EventBus bus(8, 2);
    CHECK_THROWS(bus.post(ev("")));
    CHECK_THROWS(bus.post(ev("bad type")));
    GameEvent notObject = ev("x");
    notObject.payload = Json(3);
    CHECK_THROWS(bus.post(notObject));
    CHECK_THROWS(bus.subscribe("x", EventHandler{}));
    CHECK_THROWS(bus.subscribe("x y", [](const GameEvent&, EventBus&) {}));
    bus.post(ev("game:start"));
    bus.post(ev("game:start"));
    bool full = false;
    try {
        bus.post(ev("game:start"));
    } catch (const std::length_error&) {
        full = true;
    }
    CHECK(full && bus.pending() == 2);
    CHECK_THROWS(EventBus(0));
}

TITAN_TEST_MAIN()
