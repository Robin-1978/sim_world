#pragma once

#include "core/types.hpp"

#include <string>

namespace tide_marks {

class WorldService;
struct CommandResult;

// Applies content-defined security and conflict operators. It resolves
// actor/target identities and validates parameters, but the WorldService
// remains the sole transaction and event boundary.
class SecuritySystem {
public:
    explicit SecuritySystem(WorldService& world) : world_(world) {}

    CommandResult applyOperator(const std::string& operation, const std::string& actor,
                                const std::string& target, const Json& args,
                                const std::string& turnId);

private:
    WorldService& world_;
};

}  // namespace tide_marks
