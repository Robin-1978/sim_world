#pragma once

#include <set>
#include <string>

namespace tide_marks {

// Runtime owns only generic, executable fact-transition primitives. Action
// identities and their bindings live in the Behavior Pack.
class MetaActionRegistry {
public:
    [[nodiscard]] static const std::set<std::string>& executableOperators();
    [[nodiscard]] static bool isExecutableOperator(const std::string& operation);
    [[nodiscard]] static bool isExecutableConditionOperator(const std::string& operation);
};

}  // namespace tide_marks
