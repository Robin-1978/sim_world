#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tide_marks {
using Json = nlohmann::json;

inline constexpr std::int64_t kMinutesPerHour = 60;
inline constexpr std::int64_t kMinutesPerDay = 1440;

struct GameTime {
    std::int64_t minute{0};

    [[nodiscard]] int day() const;
    [[nodiscard]] int hour() const;
    [[nodiscard]] int minuteOfHour() const;
    [[nodiscard]] std::string toString() const;
    static GameTime fromDayHour(int day, int hour, int minute = 0);
    friend bool operator<(GameTime a, GameTime b) { return a.minute < b.minute; }
    friend bool operator<=(GameTime a, GameTime b) { return a.minute <= b.minute; }
    friend bool operator==(GameTime a, GameTime b) { return a.minute == b.minute; }
};

struct RelationshipAxes {
    double trust{0.35};
    double respect{0.35};
    double fear{0.0};
    double owed{0.0};
};

struct Memory {
    std::string kind;
    std::string text;
    GameTime at;
    double confidence{1.0};
};

// A knowledge fact is an agent-scoped belief, never a second WorldState fact.
// Its source event and confidence make provenance and later correction
// explicit.
struct KnowledgeFact {
    std::string id;
    std::string subject;
    std::string predicate;
    std::string value;
    std::uint64_t sourceSequence{0};
    std::string sourceType;
    GameTime learnedAt;
    double confidence{0.0};
    std::string scope{"direct"};
    bool confirmed{false};
};

// Experience is an abstraction over one or more episodes. It can influence
// deliberation as a prior, but it cannot authorize an action or rewrite facts.
struct Experience {
    std::string id;
    std::string domain;
    std::string trigger;
    std::string action;
    std::string outcome;
    std::vector<std::uint64_t> sourceSequences;
    GameTime learnedAt;
    GameTime lastUsed;
    double confidence{0.0};
    bool personal{true};
};

struct NpcDefinition {
    std::string id;
    std::string name;
    std::string role;
    std::string home;
    std::string workplace;
    std::string publicGoal;
    std::string hiddenGoal;
    std::string boundary;
    std::string voice;
};

struct NeedState {
    double deficit{0.0};
    GameTime lastUpdated;
    std::uint64_t lastReliefRevision{0};
    double sensitivity{1.0};
    std::map<std::string, double> expectedRelief;
    double evidenceConfidence{1.0};
    std::uint64_t adaptationVersion{0};
    GameTime lastAdapted;
};

struct ExternalStimulus {
    std::string id;
    std::string sourceEvent;
    std::uint64_t sourceSequence{0};
    std::string category;
    std::string actor;
    std::string target;
    GameTime observedAt;
    GameTime expiresAt;
    std::string visibilityScope;
    double sourceConfidence{1.0};
    double relevance{1.0};
    double intensity{1.0};
    double urgency{1.0};
    double novelty{1.0};
    double valence{0.0};
    double salience{0.0};
    double effectiveSalience{0.0};
    bool active{true};
};

enum class ProcessStatus { Running, Suspended, Completed, Failed, Cancelled };

std::string toString(ProcessStatus status);
ProcessStatus processStatusFromString(const std::string& value);

enum class GoalStatus { Proposed, Active, Suspended, Completed, Failed, Cancelled };

std::string toString(GoalStatus status);
GoalStatus goalStatusFromString(const std::string& value);

// Goal and ProcessExecution are views over one activity lifecycle. The
// NpcActivity phase below is the only lifecycle authority.
struct GoalState {
    std::string id;
    std::string intent;
    std::string target;
    double priority{0.0};
    GoalStatus status{GoalStatus::Proposed};
    GameTime createdAt;
    GameTime deadline;
    std::string reason;
};

// A process is the durable execution state of an accepted intent.  It is
// deliberately separate from a transaction: each start, progress, interrupt,
// and completion is a short transaction; the process spans game time.
struct ProcessExecution {
    std::string id;
    std::string goalId;
    std::string actionId;
    std::string recipeId;
    std::string planId;
    std::string trigger;
    GameTime startedAt;
    GameTime completesAt;
    GameTime suspendedAt{-1};
    double progress{0.0};
    ProcessStatus status{ProcessStatus::Running};
    std::string behaviorNodeId;
    std::size_t behaviorStep{0};
    std::string behaviorStatus{"running"};
    std::string interruptionReason;
    bool interruptible{true};
    double interruptSalienceThreshold{1.0};
    std::string reservationMode{"none"};
    std::map<std::string, int> reservedResources;
    std::map<std::string, int> requiredResources;
    std::uint64_t proposalWorldRevision{0};
    std::uint64_t proposalNpcStateVersion{0};
};

enum class ActivityStatus { Proposed, Active, Suspended, Completed, Failed, Cancelled };

std::string toString(ActivityStatus status);
ActivityStatus activityStatusFromString(const std::string& value);

// One activity slot per NPC. ActivityStatus is the lifecycle authority; Goal and
// ProcessExecution carry the reason and timed work, while FSM mode and BT status
// are projections derived from this slot by WorldService.
struct NpcActivity {
    ActivityStatus status{ActivityStatus::Proposed};
    std::optional<GoalState> goal;
    std::optional<ProcessExecution> process;
};

// The only operation that changes the lifecycle phase. It keeps Goal and
// ProcessExecution status fields as projections of the same activity phase.
void setActivityStatus(NpcActivity& activity, ActivityStatus status);

struct NpcState {
    // Only the content-pack reference lives in runtime state. Static authored
    // data is resolved through WorldContentPack, so content updates are never
    // shadowed by a stale copy inside a save.
    std::string definitionId;
    std::string location;
    std::string scheduleLabel;
    int goalStage{0};
    std::map<std::string, NeedState> needs;
    RelationshipAxes playerRelation;
    std::vector<Memory> memories;
    std::vector<KnowledgeFact> knowledge;
    std::vector<Experience> experiences;
    std::vector<std::string> openPromises;
    std::uint64_t stateVersion{0};
    std::string mode{"working"};
    std::string strategy{"routine"};
    std::map<std::string, double> peerTrust;
    std::map<std::string, double> stimulusSensitivity;
    std::vector<ExternalStimulus> stimuli;
    GameTime lastAutonomousTurn{-1};
    NpcActivity activity;
};

enum class PromiseStatus { Proposed, Accepted, Renegotiated, Refused, Fulfilled, Broken };
std::string toString(PromiseStatus status);
PromiseStatus promiseStatusFromString(const std::string& value);

struct Promise {
    std::string id;
    std::string from;
    std::string to;
    std::string description;
    GameTime due;
    PromiseStatus status{PromiseStatus::Proposed};
    int version{0};
};

enum class EntityLifecycle { Active, Dormant, Destroyed };

struct WorldEntityState {
    std::string id;
    std::string kind;
    EntityLifecycle lifecycle{EntityLifecycle::Active};
    Json metadata = Json::object();
};

struct RuntimeLinkState {
    bool traversable{true};
    bool locked{false};
};

struct SecurityState {
    double integrity{1.0};
    double availability{1.0};
    double threat{0.0};
    double exposure{0.0};
    bool evacuated{false};
};

struct ResourceState {
    int amount{0};
    int reserved{0};
};

std::string toString(EntityLifecycle lifecycle);
EntityLifecycle entityLifecycleFromString(const std::string& value);

struct WorldEvent {
    int schemaVersion{1};
    std::uint64_t sequence{0};
    GameTime at;
    std::string type;
    std::string actor;
    std::string target;
    std::string detail;
    std::uint64_t worldRevision{0};
    Json data = Json::object();
};

// Active declarative overlays are runtime state. Their operations must travel
// with a snapshot so a restored world can rebuild the exact content view and
// retain a rollback path without trusting process-local memory.
struct ActiveOverlayState {
    std::string proposalId;
    std::string baseContentVersion;
    std::string overlayVersion;
    Json operations = Json::array();
};

struct WorldState {
    std::string worldId{"world"};
    // The concrete Game Instance is part of runtime identity.  A snapshot may
    // only be resumed with the same instance definition and content version.
    std::string instanceId{"default"};
    std::string contentVersion;
    GameTime now;
    std::uint64_t revision{0};
    std::map<std::string, ResourceState> resources;
    // Entity-owned quantities are separate from public resource pools and are
    // changed only through registered resource operators.
    std::map<std::string, std::map<std::string, int>> inventories;
    std::map<std::string, SecurityState> security;
    std::vector<ActiveOverlayState> activeOverlays;
    // Runtime entity identity and lifecycle are authoritative world facts.
    // Typed components remain projected through EntityRegistry and are only
    // mutated by a transaction/system.
    std::map<std::string, WorldEntityState> entities;
    // Endpoints and link kind are authored content.  The current access state
    // belongs to this world instance and is therefore authoritative here.
    std::map<std::string, RuntimeLinkState> links;
    std::map<std::string, NpcState> npcs;
    std::map<std::string, Promise> promises;
    std::vector<WorldEvent> events;
    std::set<std::string> processedTurnIds;
};

}  // namespace tide_marks
