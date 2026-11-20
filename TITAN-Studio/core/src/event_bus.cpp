// TITAN Engine — gameplay event bus. Original TITAN code.
#include "titan/event_bus.h"

#include <algorithm>
#include <stdexcept>

namespace titan {

bool isValidEventType(const std::string& type) {
    if (type.empty() || type.size() > 128) return false;
    return std::all_of(type.begin(), type.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == ':' || c == '-';
    });
}

EventBus::EventBus(size_t maxEventsPerDispatch, size_t maxQueued)
    : maxPerDispatch_(maxEventsPerDispatch), maxQueued_(maxQueued) {
    if (maxEventsPerDispatch == 0 || maxQueued == 0) throw std::invalid_argument("event bus limits must be positive");
}

SubscriptionId EventBus::subscribe(const std::string& type, EventHandler handler, ObjectId source) {
    if (!isValidEventType(type)) throw std::invalid_argument("invalid event type '" + type + "'");
    if (!handler) throw std::invalid_argument("event handler is empty");
    const SubscriptionId id = nextId_++;
    subs_.emplace(id, Sub{type, source, std::move(handler)});
    return id;
}

bool EventBus::unsubscribe(SubscriptionId id) { return subs_.erase(id) != 0; }

void EventBus::post(GameEvent event) {
    if (!isValidEventType(event.type)) throw std::invalid_argument("invalid event type '" + event.type + "'");
    if (!event.payload.isObject()) throw std::invalid_argument("event payload must be an object");
    if (queue_.size() >= maxQueued_) throw std::length_error("event queue is full");
    queue_.push_back(std::move(event));
}

size_t EventBus::dispatch() {
    if (dispatching_) throw std::logic_error("EventBus::dispatch is not re-entrant");
    dispatching_ = true;
    size_t delivered = 0;
    try {
        while (!queue_.empty() && delivered < maxPerDispatch_) {
            const GameEvent event = std::move(queue_.front());
            queue_.pop_front();
            ++delivered;
            // Snapshot of matching subscriptions: later subscriptions do not see this event.
            std::vector<SubscriptionId> targets;
            for (const auto& [id, sub] : subs_)
                if (sub.type == event.type && (sub.source == kNoObject || sub.source == event.source))
                    targets.push_back(id);
            for (SubscriptionId id : targets) {
                auto it = subs_.find(id);
                if (it == subs_.end()) continue;  // unsubscribed by an earlier handler
                const EventHandler handler = it->second.handler;  // copy: the handler may unsubscribe itself
                try {
                    handler(event, *this);
                } catch (const std::exception& e) {
                    errors_.push_back({id, event.type, e.what()});
                } catch (...) {
                    errors_.push_back({id, event.type, "unknown exception"});
                }
            }
        }
    } catch (...) {
        dispatching_ = false;
        throw;
    }
    dispatching_ = false;
    return delivered;
}

}  // namespace titan
