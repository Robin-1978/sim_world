#include "world/transaction_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace tide_marks {

WorldState TransactionManager::transactionalBackup(const WorldState& state, std::size_t& eventCount) {
    eventCount = state.events.size();
    // The immutable event history is intentionally not copied into every
    // savepoint. Long offline advances therefore do not copy history O(n^2).
    WorldState backup;
    backup.worldId = state.worldId;
    backup.instanceId = state.instanceId;
    backup.contentVersion = state.contentVersion;
    backup.now = state.now;
    backup.revision = state.revision;
    backup.resources = state.resources;
    backup.inventories = state.inventories;
    backup.security = state.security;
    backup.activeOverlays = state.activeOverlays;
    backup.entities = state.entities;
    backup.links = state.links;
    backup.npcs = state.npcs;
    backup.socialGroups = state.socialGroups;
    backup.promises = state.promises;
    backup.exchangeContracts = state.exchangeContracts;
    backup.processedTurnIds = state.processedTurnIds;
    return backup;
}

void TransactionManager::restoreTransactionalBackup(WorldState& state, WorldState backup,
                                                     std::size_t eventCount) {
    std::vector<WorldEvent> committed;
    const auto end = std::min(eventCount, state.events.size());
    committed.assign(state.events.begin(), state.events.begin() + static_cast<std::ptrdiff_t>(end));
    state = std::move(backup);
    state.events = std::move(committed);
}

void TransactionManager::begin(WorldState& state) {
    Frame frame;
    frame.backup = transactionalBackup(state, frame.backupEventCount);
    frame.backupDeferredEventCount = deferredEvents_.size();
    if (frames_.empty()) {
        frame.correlationId = "correlation:" + std::to_string(state.events.size() + 1);
    } else {
        const auto& parent = frames_.back();
        frame.correlationId = parent.correlationId;
        frame.lastEventSequence = parent.lastEventSequence;
    }
    frames_.push_back(std::move(frame));
}

void TransactionManager::commit(WorldState& state, EventBus& eventBus,
                                const RebuildCallback& rebuild) {
    if (frames_.empty()) throw std::logic_error("transaction commit without an active scope");
    auto completed = std::move(frames_.back());
    const auto completedLastEventSequence = completed.lastEventSequence;
    frames_.pop_back();
    if (!frames_.empty()) {
        // A nested savepoint joins its parent's causal chain only after it
        // commits. Events from a rolled-back child remain discarded.
        frames_.back().lastEventSequence = completedLastEventSequence;
        return;
    }
    if (completed.changed) ++state.revision;
    rebuild();
    auto events = std::move(deferredEvents_);
    deferredEvents_.clear();
    for (const auto& event : events) eventBus.publish(event);
    eventBus.dispatch();
}

void TransactionManager::rollback(WorldState& state, const RebuildCallback& rebuild) {
    if (frames_.empty()) throw std::logic_error("transaction rollback without an active scope");
    auto frame = std::move(frames_.back());
    frames_.pop_back();
    restoreTransactionalBackup(state, std::move(frame.backup), frame.backupEventCount);
    deferredEvents_.resize(std::min(frame.backupDeferredEventCount, deferredEvents_.size()));
    rebuild();
}

void TransactionManager::markChanged(WorldState& state) {
    if (frames_.empty()) {
        ++state.revision;
        return;
    }
    frames_.front().changed = true;
}

std::string TransactionManager::currentCorrelation() const {
    return frames_.empty() ? std::string{} : frames_.back().correlationId;
}

std::uint64_t TransactionManager::currentCausationSequence() const {
    return frames_.empty() ? 0 : frames_.back().lastEventSequence;
}

void TransactionManager::recordEvent(WorldState& state, const WorldEvent& event) {
    state.events.push_back(event);
    if (frames_.empty()) return;
    frames_.back().lastEventSequence = event.sequence;
    deferredEvents_.push_back(event);
}

}  // namespace tide_marks
