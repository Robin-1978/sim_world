#include "core/event_bus.hpp"

#include <utility>

namespace tide_marks {

EventBus::Token EventBus::subscribe(std::string eventType, Handler handler) {
    const Token token = nextToken_++;
    subscriptions_.emplace(token, Subscription{std::move(eventType), std::move(handler)});
    return token;
}

void EventBus::unsubscribe(Token token) {
    subscriptions_.erase(token);
}

void EventBus::publish(WorldEvent event) {
    queue_.push_back(std::move(event));
}

void EventBus::dispatch() {
    if (dispatching_) return;
    dispatching_ = true;
    while (!queue_.empty()) {
        WorldEvent event = std::move(queue_.front());
        queue_.pop_front();
        for (const auto& [token, subscription] : subscriptions_) {
            (void)token;
            if (subscription.eventType == "*" || subscription.eventType == event.type) {
                subscription.handler(event);
            }
        }
    }
    dispatching_ = false;
}

void EventBus::clear() {
    queue_.clear();
}

}  // namespace tide_marks
