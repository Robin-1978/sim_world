#pragma once

#include <string>
#include <string_view>

namespace tide_marks {

// Actor identifiers are part of the world contract. They are named here so a
// typo in a command path cannot silently create a second identity.
inline const std::string kPlayerId{"player"};
inline const std::string kWorldActor{"world"};

[[nodiscard]] inline std::string npcEntityId(const std::string& npcId) { return "npc:" + npcId; }
[[nodiscard]] inline std::string spatialEntityId(const std::string& spatialId) { return "spatial:" + spatialId; }
[[nodiscard]] inline std::string linkEntityId(const std::string& linkId) { return "link:" + linkId; }

}  // namespace tide_marks
