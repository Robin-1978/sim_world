#pragma once

#include <map>
#include <string>
#include <vector>

namespace tide_marks {

struct TransformComponent { double x{0.0}; double y{0.0}; double z{0.0}; double yaw{0.0}; };
struct LocationComponent { std::string locationId; std::string regionId; };
struct RenderableComponent { std::string assetId; bool visible{true}; };
struct InventoryComponent { std::map<std::string, int> quantities; };
struct NpcComponent { std::string definitionId; std::string mode{"working"}; };
// Spatial components are intentionally domain-neutral.  A village room is a
// `place` and a door is a `portal` in the content pack, but the runtime does
// not need to know those words.  The same components can represent a market,
// ship cabin, region boundary, lift, bridge, or any future authored topology.
struct SpatialComponent {
    std::string displayName;
    double minX{-10.0};
    double maxX{10.0};
    double minZ{-10.0};
    double maxZ{10.0};
    std::vector<std::string> tags;
};

struct LinkComponent {
    std::vector<std::string> endpoints;
    bool traversable{true};
    bool locked{false};
    std::string requiredItem;
    std::vector<std::string> tags;
};

}  // namespace tide_marks
