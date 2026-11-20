// TITAN Engine — gameplay event bus: deterministic, queued communication between components.
// Original TITAN code.
//
// post() only queues; dispatch() delivers queued events in FIFO order. For each event, matching handlers run in
// ascending subscription id. Guarantees:
//   * Events posted by handlers are queued behind the current ones (no re-entrant delivery); one dispatch()
//     delivers at most `maxEventsPerDispatch` events, so ping-pong chains cannot hang a frame.
//   * Unsubscribing (even from inside a handler, even itself) takes effect immediately; a subscription made during
//     delivery of an event does not receive that event.
//   * A handler that throws is recorded in errors() and does not stop other handlers or events.
//   * dispatch() is not re-entrant: calling it from a handler throws std::logic_error (recorded as that handler's
//     error).
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"
#include "titan/scene.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace titan {

struct GameEvent {
    std::string type;                // 1-128 characters of [A-Za-z0-9_.:-]
    ObjectId source = kNoObject;
    Json payload = Json::object();   // must be an object
};

using SubscriptionId = uint64_t;
class EventBus;
using EventHandler = std::function<void(const GameEvent&, EventBus&)>;

class EventBus {
public:
    struct HandlerError {
        SubscriptionId subscription = 0;
        std::string eventType;
        std::string message;
    };

    explicit EventBus(size_t maxEventsPerDispatch = 1024, size_t maxQueued = 65536);

    // `source` filters by GameEvent::source (kNoObject: any source). std::invalid_argument for a bad type or an
    // empty handler.
    SubscriptionId subscribe(const std::string& type, EventHandler handler, ObjectId source = kNoObject);
    bool unsubscribe(SubscriptionId id);  // false when unknown
    size_t subscriptionCount() const { return subs_.size(); }

    // std::invalid_argument for a bad type or payload; std::length_error when the queue is full.
    void post(GameEvent event);
    size_t pending() const { return queue_.size(); }

    // Delivers up to maxEventsPerDispatch queued events; returns how many were delivered.
    size_t dispatch();

    const std::vector<HandlerError>& errors() const { return errors_; }
    void clearErrors() { errors_.clear(); }

private:
    struct Sub {
        std::string type;
        ObjectId source;
        EventHandler handler;
    };
    size_t maxPerDispatch_;
    size_t maxQueued_;
    SubscriptionId nextId_ = 1;
    std::map<SubscriptionId, Sub> subs_;
    std::deque<GameEvent> queue_;
    std::vector<HandlerError> errors_;
    bool dispatching_ = false;
};

bool isValidEventType(const std::string& type);

}  // namespace titan
