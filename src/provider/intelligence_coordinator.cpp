#include "provider/intelligence_coordinator.hpp"

namespace tide_marks {

IntelligenceCoordinator::IntelligenceCoordinator(IProvider& provider)
    : queue_(std::make_unique<TurnQueue>(provider)) {}

TurnSubmitResult IntelligenceCoordinator::submit(TurnRequest request) {
    auto result = queue_->submit(std::move(request));
    for (const auto& supersededId : result.supersededTurnIds) trackedTurnIds_.erase(supersededId);
    return result;
}

std::vector<TurnResult> IntelligenceCoordinator::drain() { return queue_->drain(); }

std::size_t IntelligenceCoordinator::pending() const { return queue_->pending(); }

bool IntelligenceCoordinator::isTracked(const std::string& turnId) const {
    return trackedTurnIds_.contains(turnId);
}

void IntelligenceCoordinator::track(const std::string& turnId) { trackedTurnIds_.insert(turnId); }

void IntelligenceCoordinator::forget(const std::string& turnId) { trackedTurnIds_.erase(turnId); }

}  // namespace tide_marks
