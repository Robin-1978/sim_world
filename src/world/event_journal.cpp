#include "world/event_journal.hpp"

namespace tide_marks {

WorldEvent EventJournal::append(WorldState& state, TransactionManager& transactions, EventBus& eventBus,
                                const std::string& contentVersion, std::string type, std::string actor,
                                std::string target, std::string detail, Json data,
                                const ProjectionCallback& project) {
    const bool inTransaction = transactions.active();
    if (!inTransaction) ++state.revision;
    else transactions.markChanged(state);
    const auto committedRevision = state.revision + (inTransaction ? 1 : 0);
    const auto sequence = state.events.size() + 1;
    const auto correlationId = inTransaction ? transactions.currentCorrelation()
                                             : "correlation:" + std::to_string(sequence);
    const auto causationSequence = inTransaction ? transactions.currentCausationSequence() : 0;
    const WorldEvent committed{kWorldEventSchemaVersion, sequence, state.now, std::move(type), std::move(actor),
                               std::move(target), std::move(detail), committedRevision, std::move(data),
                               "event:" + std::to_string(sequence), correlationId, causationSequence,
                               contentVersion, kSimulationVersion};
    transactions.recordEvent(state, committed);
    project(committed);
    if (!inTransaction) {
        eventBus.publish(committed);
        eventBus.dispatch();
    }
    return committed;
}

}  // namespace tide_marks
