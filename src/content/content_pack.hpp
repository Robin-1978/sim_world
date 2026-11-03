#pragma once

#include "core/types.hpp"
#include "provider/provider.hpp"
#include "world/components.hpp"

#include <map>
#include <string>
#include <vector>

namespace tide_marks {

struct ScheduleSlot {
    int startMinute{0};
    int endMinute{0};
    std::string location;
};

struct NeedContent {
    std::string id;
    std::string taxonomy;
    double initialDeficit{0.0};
    double decayPerHour{0.0};
    double criticalThreshold{0.8};
    double weight{1.0};
    double priority{0.0};
    std::map<std::string, double> satisfiers;
    std::string sourceKind{"authored"};
    std::string sourceId;
    std::string sourceVersion;
};

struct NpcContent {
    NpcDefinition definition;
    std::string initialLocation;
    std::string initialScheduleLabel;
    int initialGoalStage{0};
    std::string initialStrategy{"routine"};
    std::map<std::string, double> initialPeerTrust;
    std::vector<NeedContent> needs;
    std::vector<ScheduleSlot> schedule;
    std::vector<ActionCandidate> actions;
};

struct SpatialContent {
    std::string id;
    std::string kind{"place"};
    SpatialComponent spatial;
};

struct LinkContent {
    std::string id;
    std::string kind{"portal"};
    LinkComponent link;
};

struct EntityArchetypeContent {
    std::string id;
    std::string kind;
    Json defaultMetadata = Json::object();
};

struct ResourceContent {
    std::string id;
    std::string unit;
    std::string kind;
    std::string owner;
    std::string location;
    int initial{0};
    int minimum{0};
    int maximum{100};
    bool divisible{true};
    std::vector<std::string> qualityAxes;
    std::vector<std::string> substitutes;
    Json decay = Json::object();
};

struct EffectContent {
    std::string operation;
    std::string resource;
    int amount{0};
    int maximum{100};
    std::string effectAction;
    std::vector<std::string> targets;
};

struct ActivityContent {
    std::string id;
    std::string trigger;
    int hour{-1};
    std::string actor;
    std::string target;
    std::string conditionResource;
    int conditionBelow{0};
    std::string eventType;
    std::string detail;
    std::vector<EffectContent> effects;
};

struct WorldContentPack {
    std::string id;
    std::string version;
    std::string contentVersion;
    std::string worldId;
    GameTime startTime;
    std::map<std::string, ResourceContent> resources;
    std::vector<NpcContent> npcs;
    std::vector<ActivityContent> activities;
    std::vector<SpatialContent> spatialEntities;
    std::vector<LinkContent> links;
    std::vector<EntityArchetypeContent> entityArchetypes;
};

class ContentPackLoader {
public:
    [[nodiscard]] static std::string defaultRoot();
    [[nodiscard]] static WorldContentPack load(const std::string& rootPath);
};

}  // namespace tide_marks
