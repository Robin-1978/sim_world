#pragma once

#include "core/types.hpp"
#include "provider/provider.hpp"
#include "npc/behavior_recipe.hpp"
#include "npc/conversation.hpp"
#include "world/components.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tide_marks {

struct ScheduleSlot {
    int startMinute{0};
    int endMinute{0};
    std::string location;
};

struct NeedSatisfier {
    double relief{0.0};
    std::vector<std::string> metaActions;
};

struct NeedContent {
    std::string id;
    std::string domain;
    std::string dimension;
    double initialDeficit{0.0};
    double decayPerHour{0.0};
    double criticalThreshold{0.8};
    double weight{1.0};
    double priority{0.0};
    std::map<std::string, NeedSatisfier> satisfiers;
    std::string sourceKind{"authored"};
    std::string sourceId;
    std::string sourceVersion;
};

struct DriveAdaptationPolicy {
    bool enabled{true};
    double expectationLearningRate{0.0};
    double maxExpectationStep{0.0};
    int cooldownMinutes{0};
};

struct StimulusPolicy {
    std::size_t maxActivePerNpc{32};
    double noveltyDecay{0.5};
    double habituationStep{0.1};
    double minSensitivity{0.25};
    double maxSensitivity{1.0};
    int repeatWindowMinutes{720};
};

struct StimulusRule {
    std::string id;
    std::string eventPrefix;
    std::string category;
    std::string visibility;
    double relevance{1.0};
    double intensity{1.0};
    double urgency{1.0};
    double valence{0.0};
    double sourceConfidence{1.0};
    int ttlMinutes{720};
};

struct NpcDeliberationPolicy {
    bool enabled{true};
    double minStimulusSalience{0.5};
    double minDrivePressure{0.5};
    double minInterruptSalience{0.85};
    int cooldownMinutes{360};
    int maxDecisionAgeMinutes{120};
    bool coalescePending{true};
};

struct SimulationPolicy {
    int nearTickMinutes{1};
    int farTickMinutes{15};
};

// A decision channel is a content-defined interaction contract. It tells the
// runtime how to project an input into the provider, how to record the result,
// and which world-action default applies. The channel id is data; the runtime
// never branches on a story-specific trigger string.
struct DecisionChannelPolicy {
    std::string id;
    bool conversation{false};
    bool classifySpeechAct{false};
    bool defaultWorldActionAllowed{true};
    std::string defaultSpeechAct{"autonomous"};
    std::string eventType{"intent"};
    std::string memoryKind{"intent"};
    std::string eventTarget{"self"};
    std::string providerPolicy;
};

struct NpcContent {
    NpcDefinition definition;
    std::string initialLocation;
    std::string initialScheduleLabel;
    int initialGoalStage{0};
    std::string initialStrategy{"routine"};
    std::string simulationTier{"near"};
    std::map<std::string, double> initialPeerTrust;
    std::vector<NeedContent> needs;
    std::vector<ScheduleSlot> schedule;
    std::vector<ActionCandidate> actions;
};

struct SocialGroupContent {
    std::string id;
    std::string kind;
    std::string name;
    std::map<std::string, double> metrics;
    std::map<std::string, std::string> initialMembers;
    std::map<std::string, RelationshipAxes> initialRelationships;
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
    std::string linkId;
    bool traversable{true};
    bool locked{false};
    std::string metaActionId;
    Json args = Json::object();
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
    // When set, the activity is an opportunity for a configured decision
    // channel. The runtime records the trigger and asks the provider; it does
    // not apply the listed effects as a hidden NPC script.
    std::string decisionChannel;
};

struct StrategyRuleContent {
    std::string id;
    std::string npcId;
    std::string peerId;
    double minPeerTrust{0.0};
    std::string strategy;
    int goalStage{0};
    std::string memory;
    std::string eventType;
};

struct AnomalyRuleContent {
    std::string id;
    std::string operation;
    std::string subject;
    std::string metric;
    double threshold{0.0};
    std::string severity{"warning"};
    std::string description;
};

struct RelationshipDelta {
    double trust{0.0};
    double respect{0.0};
    double fear{0.0};
    double owed{0.0};
};

// Promise outcomes change relationship axes through authored policy, not
// hard-coded constants inside the runtime.
struct PromisePolicy {
    int renegotiationExtensionMinutes{static_cast<int>(kMinutesPerDay)};
    RelationshipDelta brokenPromiser;
    RelationshipDelta brokenPromisee;
    RelationshipDelta refusedActor;
    RelationshipDelta acceptedActor;
};

struct NpcInstanceContent {
    std::string location;
    std::string scheduleLabel;
    int goalStage{0};
    std::string strategy;
    std::map<std::string, double> peerTrust;
};

// A Game Instance selects a content pack and supplies one concrete opening
// state. Content files keep archetype defaults; the instance is what makes a
// particular run reproducible and replaceable.
struct GameInstanceContent {
    std::string id;
    std::string displayName;
    std::string contentPackId;
    std::string contentVersion;
    std::string worldId;
    std::optional<GameTime> startTime;
    std::map<std::string, int> resourceInitial;
    std::map<std::string, std::map<std::string, int>> inventoryInitial;
    std::map<std::string, NpcInstanceContent> npcs;
};

struct WorldContentPack {
    std::string id;
    std::string version;
    std::string contentVersion;
    std::string worldId;
    // Optional presentation data is still authored content. Runtime systems do
    // not inspect these fields; clients use them to replace a default scene
    // without branching on a story-specific world id.
    std::string sceneAsset;
    std::string cameraMode{"slanted_top_down"};
    GameTime startTime;
    DriveAdaptationPolicy driveAdaptation;
    StimulusPolicy stimulusPolicy;
    NpcDeliberationPolicy npcDeliberation;
    SimulationPolicy simulationPolicy;
    ConversationPolicy conversationPolicy;
    std::map<std::string, DecisionChannelPolicy> decisionChannels;
    std::string conversationChannelId;
    std::string autonomousChannelId;
    PromisePolicy promisePolicy;
    std::vector<StimulusRule> stimulusRules;
    std::map<std::string, ResourceContent> resources;
    std::vector<NpcContent> npcs;
    std::vector<SocialGroupContent> socialGroups;
    std::vector<ActivityContent> activities;
    std::vector<StrategyRuleContent> strategyRules;
    std::vector<AnomalyRuleContent> anomalyRules;
    std::vector<SpatialContent> spatialEntities;
    std::vector<LinkContent> links;
    std::vector<EntityArchetypeContent> entityArchetypes;
    // Behavior alpha-names are content. Runtime only knows the finite set of
    // generic operators in world/meta_actions.hpp.
    std::map<std::string, MetaActionDefinition> metaActions;
    std::map<std::string, ConditionDefinition> conditions;
    std::vector<BehaviorRecipeContent> behaviorRecipes;
    std::optional<GameInstanceContent> instance;
};

class ContentPackLoader {
public:
    [[nodiscard]] static std::string defaultRoot();
    [[nodiscard]] static WorldContentPack load(const std::string& rootPath);
};

}  // namespace tide_marks
