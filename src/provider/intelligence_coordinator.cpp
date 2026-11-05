#include "provider/intelligence_coordinator.hpp"

#include <algorithm>

namespace tide_marks {

IntelligenceCoordinator::IntelligenceCoordinator(IProvider& provider)
    : queue_(std::make_unique<TurnQueue>(provider)) {}

TurnSubmitResult IntelligenceCoordinator::submit(TurnRequest request) {
    auto result = queue_->submit(std::move(request));
    for (const auto& supersededId : result.supersededTurnIds) {
        trackedTurnIds_.erase(supersededId);
        trackedTurnNpcs_.erase(supersededId);
    }
    return result;
}

std::vector<TurnResult> IntelligenceCoordinator::drain() { return queue_->drain(); }

std::size_t IntelligenceCoordinator::pending() const { return queue_->pending(); }

bool IntelligenceCoordinator::isTracked(const std::string& turnId) const {
    return trackedTurnIds_.contains(turnId);
}

bool IntelligenceCoordinator::isTrackedForNpc(const std::string& npcId) const {
    return std::any_of(trackedTurnNpcs_.begin(), trackedTurnNpcs_.end(),
                       [&npcId](const auto& item) { return item.second == npcId; });
}

void IntelligenceCoordinator::track(const std::string& turnId, const std::string& npcId) {
    trackedTurnIds_.insert(turnId);
    if (!npcId.empty()) trackedTurnNpcs_[turnId] = npcId;
}

void IntelligenceCoordinator::forget(const std::string& turnId) {
    trackedTurnIds_.erase(turnId);
    trackedTurnNpcs_.erase(turnId);
}

}  // namespace tide_marks
