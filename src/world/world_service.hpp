#pragma once

#include "core/types.hpp"
#include "npc/action_validator.hpp"
#include "npc/behavior_tree.hpp"
#include "npc/goal_selector.hpp"
#include "provider/provider.hpp"
#include <memory>
#include <optional>

namespace tide_marks {
struct Command {
    std::string turnId;
    std::string kind;
    std::string actor;
    std::string target;
    std::string promiseId;
    std::string detail;
    std::uint64_t expectedWorldRevision{0};
    std::uint64_t expectedNpcStateVersion{0};
};

struct CommandResult {
    bool accepted{false};
    std::string code;
    std::string message;
    std::optional<WorldEvent> event;
};

struct DialogueTurn {
    bool accepted{false};
    std::string npcId;
    std::string actionId;
    std::string intent;
    std::string motivation;
    std::string dialogue;
    std::string provider;
    std::string error;
    std::optional<WorldEvent> event;
};

class WorldService {
public:
    explicit WorldService(std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());

    static WorldService createVillage(std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());
    static WorldService fromSnapshot(const Json& json,
                                     std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());

    [[nodiscard]] const WorldState& state() const { return state_; }
    [[nodiscard]] std::string providerName() const { return provider_->name(); }
    [[nodiscard]] bool hasNpc(const std::string& npcId) const;
    [[nodiscard]] DecisionContext decisionContext(const std::string& npcId) const;

    void advanceMinutes(std::int64_t minutes);
    CommandResult submit(const Command& command);
    DialogueTurn converse(const std::string& npcId, const std::string& playerInput,
                          const std::string& turnId);

    CommandResult createPromise(const std::string& from, const std::string& to,
                                const std::string& description, int dueInDays,
                                const std::string& turnId);
    CommandResult decidePromise(const std::string& promiseId, PromiseStatus decision,
                                const std::string& actor, const std::string& turnId,
                                const std::string& detail = {});

    [[nodiscard]] Json snapshotJson() const;
    void saveSnapshot(const std::string& path) const;
    void writeReplay(const std::string& path) const;
    static WorldService replay(const std::string& path,
                               std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());

private:
    WorldState state_;
    std::unique_ptr<IProvider> provider_;

    void addEvent(std::string type, std::string actor, std::string target,
                  std::string detail, Json data = Json::object());
    void processHourBoundary(int day, int hour);
    void processDayBoundary(int day);
    void applyPromiseDeadline();
    CommandResult reject(std::string code, std::string message) const;
};

Json toJson(const WorldState& state);
WorldState worldStateFromJson(const Json& json);
std::string summarize(const WorldState& state);

}  // namespace tide_marks
