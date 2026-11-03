#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tide_marks {
using Json = nlohmann::json;

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

struct NpcState {
    NpcDefinition definition;
    std::string location;
    std::string scheduleLabel;
    int goalStage{0};
    int resource{0};
    double pressure{0.2};
    RelationshipAxes playerRelation;
    std::vector<Memory> memories;
    std::vector<std::string> openPromises;
    std::uint64_t stateVersion{0};
    std::string mode{"working"};
    std::string strategy{"routine"};
    std::map<std::string, double> peerTrust;
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

struct VillageState {
    int grain{100};
    int boatIntegrity{72};
    int recordsIntegrity{80};
    bool lanternLit{true};
    int reputation{0};
    int cooperation{0};
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

struct WorldState {
    std::string worldId{"world"};
    std::string contentVersion{"village-p0.2"};
    GameTime now;
    std::uint64_t revision{0};
    VillageState village;
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
