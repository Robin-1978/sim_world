#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <string>

namespace tide_marks {

// Runtime delivery is separate from the persistent WorldState event log.
// publish() queues events; dispatch() drains them FIFO. This keeps event order
// deterministic and prevents recursive subscriptions from reordering events.
class EventBus {
public:
    using Token = std::size_t;
    using Handler = std::function<void(const WorldEvent&)>;

    [[nodiscard]] Token subscribe(std::string eventType, Handler handler);
    void unsubscribe(Token token);
    void publish(WorldEvent event);
    void dispatch();
    [[nodiscard]] std::size_t pending() const { return queue_.size(); }
    [[nodiscard]] std::size_t deliveryFailures() const { return deliveryFailures_; }
    void clear();

private:
    struct Subscription {
        std::string eventType;
        Handler handler;
    };
    Token nextToken_{1};
    bool dispatching_{false};
    std::size_t deliveryFailures_{0};
    std::map<Token, Subscription> subscriptions_;
    std::deque<WorldEvent> queue_;
};

}  // namespace tide_marks
