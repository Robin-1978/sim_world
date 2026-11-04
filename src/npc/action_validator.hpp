#pragma once

#include "provider/provider.hpp"

namespace tide_marks {

struct ValidationResult {
    bool valid{false};
    std::string code;
    std::string message;
};

class ActionValidator {
public:
    [[nodiscard]] static ValidationResult validate(const DecisionContext& context,
                                                    const std::string& actionId,
                                                    const Json& parameters = Json::object());
};

}  // namespace tide_marks
