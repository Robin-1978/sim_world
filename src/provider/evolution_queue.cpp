#include "provider/evolution_queue.hpp"

#include <exception>
#include <iterator>

namespace tide_marks {

EvolutionQueue::EvolutionQueue(IProvider& provider)
    : provider_(provider), worker_([this] { run(); }) {}

EvolutionQueue::~EvolutionQueue() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) worker_.join();
}

EvolutionSubmitResult EvolutionQueue::submit(EvolutionRequest request) {
    EvolutionSubmitResult result;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return result;
        if (request.coalescePending && !request.key.empty()) {
            for (auto item = pending_.begin(); item != pending_.end();) {
                if (item->key == request.key) {
                    result.supersededRequestIds.push_back(item->requestId);
                    item = pending_.erase(item);
                } else {
                    ++item;
                }
            }
        }
        pending_.push_back(std::move(request));
    }
    condition_.notify_one();
    result.accepted = true;
    return result;
}

std::vector<EvolutionResult> EvolutionQueue::drain() {
    std::deque<EvolutionResult> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(ready_);
    }
    return {std::make_move_iterator(ready.begin()), std::make_move_iterator(ready.end())};
}

std::size_t EvolutionQueue::pending() const {
    std::lock_guard lock(mutex_);
    return pending_.size() + ready_.size() + inFlight_;
}

void EvolutionQueue::run() {
    while (true) {
        EvolutionRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) return;
            request = std::move(pending_.front());
            pending_.pop_front();
            ++inFlight_;
        }

        ProviderResponse response;
        try {
            if (request.requestKind == "reflection") {
                response = provider_.proposeReflection(request.context, request.targetNpcId,
                                                        request.validationFeedback);
            } else {
                response = provider_.proposeEvolution(request.context, request.evolutionLevel,
                                                      request.validationFeedback);
            }
        } catch (const std::exception& error) {
            response = ProviderResponse{false, {}, {}, std::string("provider_exception:") + error.what(), Json::object()};
        } catch (...) {
            response = ProviderResponse{false, {}, {}, "provider_exception:unknown", Json::object()};
        }
        EvolutionResult result{std::move(request), std::move(response), provider_.name()};
        {
            std::lock_guard lock(mutex_);
            --inFlight_;
            ready_.push_back(std::move(result));
        }
    }
}

}  // namespace tide_marks
