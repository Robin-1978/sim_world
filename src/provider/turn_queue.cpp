#include "provider/turn_queue.hpp"

#include <exception>
#include <iterator>

namespace tide_marks {

TurnQueue::TurnQueue(IProvider& provider) : provider_(provider), worker_([this] { run(); }) {}

TurnQueue::~TurnQueue() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) worker_.join();
}

bool TurnQueue::submit(TurnRequest request) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return false;
        pending_.push_back(std::move(request));
    }
    condition_.notify_one();
    return true;
}

std::vector<TurnResult> TurnQueue::drain() {
    std::deque<TurnResult> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(ready_);
    }
    return {std::make_move_iterator(ready.begin()), std::make_move_iterator(ready.end())};
}

std::size_t TurnQueue::pending() const {
    std::lock_guard lock(mutex_);
    return pending_.size() + ready_.size();
}

void TurnQueue::run() {
    while (true) {
        TurnRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) return;
            request = std::move(pending_.front());
            pending_.pop_front();
        }

        ProviderResponse response;
        try {
            response = provider_.decide(request.context, request.playerInput);
        } catch (const std::exception& error) {
            response = ProviderResponse{false, {}, {}, std::string("provider_exception:") + error.what()};
        } catch (...) {
            response = ProviderResponse{false, {}, {}, "provider_exception:unknown"};
        }
        TurnResult result{std::move(request), std::move(response), provider_.name()};
        {
            std::lock_guard lock(mutex_);
            ready_.push_back(std::move(result));
        }
    }
}

}  // namespace tide_marks
