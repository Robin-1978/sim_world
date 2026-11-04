#pragma once

#include "core/types.hpp"

#include <cstdint>
#include <string>

namespace tide_marks {

class WorldService;
struct CommandResult;

// Owns agent-scoped knowledge and experience updates. Every entry must cite
// an already committed WorldEvent; these records guide later decisions but
// never become a second source of world facts.
class CognitionSystem {
public:
    explicit CognitionSystem(WorldService& world) : world_(world) {}

    CommandResult assertKnownFact(const std::string& npcId, const std::string& subject,
                                  const std::string& predicate, const std::string& value,
                                  std::uint64_t sourceSequence, const std::string& scope,
                                  double confidence, const std::string& actor,
                                  const std::string& turnId);
    CommandResult recordExperience(const std::string& npcId, const std::string& domain,
                                   const std::string& trigger, const std::string& action,
                                   const std::string& outcome, std::uint64_t sourceSequence,
                                   double confidence, bool personal, const std::string& actor,
                                   const std::string& turnId);

private:
    WorldService& world_;
};

}  // namespace tide_marks
