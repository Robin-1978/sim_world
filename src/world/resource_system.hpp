#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <string>

namespace tide_marks {

// Generic quantity and ownership mechanics. This system does not open a
// transaction or append an event; WorldService owns those boundaries.
class ResourceSystem {
public:
    struct TransferResult {
        bool accepted{false};
        std::string code;
        std::string message;
        std::string sourceId;
        std::string targetId;
        int sourceBefore{0};
        int sourceAfter{0};
        int targetBefore{0};
        int targetAfter{0};
    };

    [[nodiscard]] static int amount(const WorldState& state, const std::string& resource);
    [[nodiscard]] static int available(const WorldState& state, const std::string& resource);
    [[nodiscard]] static std::string resolveParty(const WorldState& state, const std::string& party);
    [[nodiscard]] static bool activeParty(const WorldState& state, const std::string& party);
    [[nodiscard]] static int balance(const WorldState& state, const std::string& party,
                                     const std::string& resource);
    [[nodiscard]] static TransferResult transfer(WorldState& state, const WorldContentPack& content,
                                                 const std::string& source, const std::string& target,
                                                 const std::string& resource, int quantity);
    [[nodiscard]] static bool consume(WorldState& state, const WorldContentPack& content,
                                      const std::string& resource, int quantity, bool reserved);
    [[nodiscard]] static bool produce(WorldState& state, const WorldContentPack& content,
                                      const std::string& resource, int quantity);
    [[nodiscard]] static bool reserve(WorldState& state, const WorldContentPack& content,
                                      const std::string& resource, int quantity);
    [[nodiscard]] static bool release(WorldState& state, const WorldContentPack& content,
                                      const std::string& resource, int quantity);
};

}  // namespace tide_marks
