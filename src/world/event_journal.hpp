#pragma once

#include "core/event_bus.hpp"
#include "core/types.hpp"
#include "world/transaction_manager.hpp"

#include <functional>
#include <string>

namespace tide_marks {

// EventJournal is the sole event identity and publication boundary. It does
// not decide whether a domain operation is legal; callers must validate and
// mutate facts before appending the resulting event.
class EventJournal {
public:
    using ProjectionCallback = std::function<void(const WorldEvent&)>;

    WorldEvent append(WorldState& state, TransactionManager& transactions, EventBus& eventBus,
                      const std::string& contentVersion, std::string type, std::string actor,
                      std::string target, std::string detail, Json data,
                      const ProjectionCallback& project);
};

}  // namespace tide_marks
