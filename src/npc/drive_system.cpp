#include "npc/drive_system.hpp"

#include <algorithm>
#include <cmath>

namespace tide_marks {

namespace {

constexpr double kMinimumChange = 0.000001;

NeedState& stateFor(NpcState& npc, const NeedContent& definition, GameTime now) {
    auto [found, inserted] = npc.needs.try_emplace(definition.id);
    if (inserted) {
        found->second.deficit = definition.initialDeficit;
        found->second.lastUpdated = now;
        found->second.sensitivity = definition.weight;
        for (const auto& [actionId, satisfier] : definition.satisfiers) {
            found->second.expectedRelief[actionId] = satisfier.relief;
        }
    }
    return found->second;
}

}  // namespace

void DriveSystem::initialize(NpcState& npc, const std::vector<NeedContent>& definitions, GameTime now) {
    npc.needs.clear();
    for (const auto& definition : definitions) {
        NeedState state;
        state.deficit = definition.initialDeficit;
        state.lastUpdated = now;
        state.sensitivity = definition.weight;
        for (const auto& [actionId, satisfier] : definition.satisfiers) {
            state.expectedRelief[actionId] = satisfier.relief;
        }
        npc.needs.emplace(definition.id, std::move(state));
    }
}

std::vector<NeedChange> DriveSystem::advance(NpcState& npc,
                                             const std::vector<NeedContent>& definitions,
                                             GameTime from,
                                             GameTime to) {
    std::vector<NeedChange> changes;
    if (to <= from) return changes;
    for (const auto& definition : definitions) {
        auto& state = stateFor(npc, definition, from);
        const GameTime effectiveFrom{std::max(from.minute, state.lastUpdated.minute)};
        const double elapsedHours = static_cast<double>(to.minute - effectiveFrom.minute) /
                                    static_cast<double>(kMinutesPerHour);
        if (elapsedHours <= 0.0) continue;
        const double before = std::clamp(state.deficit, 0.0, 1.0);
        const double after = std::clamp(before + definition.decayPerHour * elapsedHours, 0.0, 1.0);
        if (std::abs(after - before) < kMinimumChange) {
            state.lastUpdated = to;
            continue;
        }
        state.deficit = after;
        state.lastUpdated = to;
        changes.push_back(NeedChange{definition.id, before, after,
                                     before < definition.criticalThreshold &&
                                         after >= definition.criticalThreshold});
    }
    return changes;
}

std::vector<NeedChange> DriveSystem::relieve(NpcState& npc,
                                             const std::vector<NeedContent>& definitions,
                                             const std::string& actionId,
                                             GameTime now,
                                             const DriveAdaptationPolicy& policy) {
    std::vector<NeedChange> changes;
    for (const auto& definition : definitions) {
        const auto satisfier = definition.satisfiers.find(actionId);
        if (satisfier == definition.satisfiers.end()) continue;
        auto& state = stateFor(npc, definition, now);
        const double before = std::clamp(state.deficit, 0.0, 1.0);
        const double authoredRelief = std::max(0.0, satisfier->second.relief);
        auto expectation = state.expectedRelief.find(actionId);
        if (expectation == state.expectedRelief.end()) {
            expectation = state.expectedRelief.emplace(actionId, authoredRelief).first;
        }
        const double expectationBefore = std::clamp(expectation->second, 0.0, 1.0);
        const double after = std::clamp(before - authoredRelief, 0.0, 1.0);
        const double actualRelief = before - after;
        if (std::abs(after - before) < kMinimumChange) continue;
        const double sensitivityBefore = state.sensitivity;
        double expectationAfter = expectationBefore;
        bool adapted = false;
        if (policy.enabled && policy.expectationLearningRate > 0.0 &&
            (state.lastAdapted.minute == 0 || now.minute - state.lastAdapted.minute >= policy.cooldownMinutes)) {
            const double target = expectationBefore +
                                  policy.expectationLearningRate * (actualRelief - expectationBefore);
            const double delta = std::clamp(target - expectationBefore,
                                            -policy.maxExpectationStep, policy.maxExpectationStep);
            expectationAfter = std::clamp(expectationBefore + delta, 0.0, 1.0);
            if (std::abs(expectationAfter - expectationBefore) >= kMinimumChange) {
                expectation->second = expectationAfter;
                state.lastAdapted = now;
                ++state.adaptationVersion;
                adapted = true;
            }
        }
        state.deficit = after;
        state.lastUpdated = now;
        changes.push_back(NeedChange{definition.id, before, after,
                                     before < definition.criticalThreshold &&
                                         after >= definition.criticalThreshold,
                                     expectationBefore, actualRelief, expectationBefore, expectationAfter,
                                     sensitivityBefore, state.sensitivity, adapted});
    }
    return changes;
}

double DriveSystem::drive(const NeedContent& definition, const NeedState& state) {
    const double deficit = std::clamp(state.deficit, 0.0, 1.0);
    const double base = deficit * deficit;
    const double threshold = std::clamp(definition.criticalThreshold, 0.0, 1.0);
    double criticalMultiplier = 1.0;
    if (deficit >= threshold && threshold < 1.0) {
        criticalMultiplier += 2.0 * (deficit - threshold) / (1.0 - threshold);
    }
    const double sensitivity = std::clamp(state.sensitivity, 0.0, 100.0);
    return definition.priority + sensitivity * base * criticalMultiplier;
}

}  // namespace tide_marks
