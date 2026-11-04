#pragma once

#include "provider/turn_queue.hpp"

#include <memory>
#include <set>
#include <string>

namespace tide_marks {

// Coordinates asynchronous LLM work without owning any world facts. The
// coordinator tracks request identity and coalescing; WorldService remains the
// only authority that validates and commits a returned proposal.
class IntelligenceCoordinator {
public:
    explicit IntelligenceCoordinator(IProvider& provider);

    [[nodiscard]] TurnSubmitResult submit(TurnRequest request);
    [[nodiscard]] std::vector<TurnResult> drain();
    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] bool isTracked(const std::string& turnId) const;
    void track(const std::string& turnId);
    void forget(const std::string& turnId);

private:
    std::unique_ptr<TurnQueue> queue_;
    std::set<std::string> trackedTurnIds_;
};

}  // namespace tide_marks
