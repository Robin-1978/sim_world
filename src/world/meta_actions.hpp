#pragma once

#include <string>
#include <vector>

namespace tide_marks {

struct MetaActionSpec {
    std::string id;
    std::string category;
    std::string description;
    bool implemented{false};
};

class MetaActionRegistry {
public:
    // Design-only actions are rejected by content loading until implemented.
    [[nodiscard]] static const std::vector<MetaActionSpec>& all();
    [[nodiscard]] static bool contains(const std::string& id);
    [[nodiscard]] static bool isImplemented(const std::string& id);
};

}  // namespace tide_marks
