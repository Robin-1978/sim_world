#pragma once

#include "core/event_bus.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace tide_marks {

// TransactionManager owns only the mechanics of a world commit boundary.
// Domain systems still decide which facts to mutate; they cannot publish an
// event or bypass the savepoint protocol without going through WorldService.
class TransactionManager {
public:
    using RebuildCallback = std::function<void()>;

    void begin(WorldState& state);
    void commit(WorldState& state, EventBus& eventBus, const RebuildCallback& rebuild);
    void rollback(WorldState& state, const RebuildCallback& rebuild);
    void markChanged(WorldState& state);
    // A scheduler may coalesce quiet minutes into one transaction. The
    // corresponding logical revisions are still accounted for here so domain
    // systems never write WorldState::revision directly.
    void addRevisions(WorldState& state, std::uint64_t count);

    [[nodiscard]] bool active() const { return !frames_.empty(); }
    [[nodiscard]] std::string currentCorrelation() const;
    [[nodiscard]] std::uint64_t currentCausationSequence() const;
    void recordEvent(WorldState& state, const WorldEvent& event);

private:
    struct Frame {
        WorldState backup;
        std::size_t backupEventCount{0};
        std::size_t backupDeferredEventCount{0};
        std::string correlationId;
        std::uint64_t lastEventSequence{0};
        bool changed{false};
    };

    std::vector<Frame> frames_;
    std::vector<WorldEvent> deferredEvents_;

    static WorldState transactionalBackup(const WorldState& state, std::size_t& eventCount);
    static void restoreTransactionalBackup(WorldState& state, WorldState backup,
                                           std::size_t eventCount);
};

}  // namespace tide_marks
