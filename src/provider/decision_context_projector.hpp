#pragma once

#include "content/content_pack.hpp"
#include "provider/provider.hpp"

#include <cstdint>
#include <string>

namespace tide_marks {

// Builds the bounded, actor-scoped view sent to a provider.  The projector is
// deliberately read-only: it has no WorldService reference, cannot submit a
// command, and does not expose the complete WorldState.  WorldService remains
// responsible for deciding the revision/prompt versions attached to a request.
class DecisionContextProjector {
public:
    [[nodiscard]] static DecisionContext build(const WorldState& state,
                                               const WorldContentPack& content,
                                               const std::string& npcId,
                                               std::uint64_t worldRevision,
                                               std::string promptVersion);
};

}  // namespace tide_marks
