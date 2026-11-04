#pragma once

#include "core/types.hpp"
#include "core/entity_registry.hpp"
#include "content/content_pack.hpp"
#include "core/event_bus.hpp"
#include "npc/action_validator.hpp"
#include "npc/behavior_tree.hpp"
#include "npc/goal_selector.hpp"
#include "navigation/path_planner.hpp"
#include "provider/provider.hpp"
#include "provider/turn_queue.hpp"
#include "world/components.hpp"
#include "world/ids.hpp"
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

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

    static WorldService load(std::string contentRoot = ContentPackLoader::defaultRoot(),
                             std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());
    static WorldService fromContent(const WorldContentPack& content,
                                    std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());
    static WorldService fromSnapshot(const Json& json,
                                     std::string contentRoot = ContentPackLoader::defaultRoot(),
                                     std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());

    [[nodiscard]] const WorldState& state() const { return state_; }
    [[nodiscard]] const WorldContentPack& content() const { return content_; }
    [[nodiscard]] std::string providerName() const { return provider_->name(); }
    [[nodiscard]] EventBus& eventBus() { return eventBus_; }
    [[nodiscard]] const EntityRegistry& entities() const { return entities_; }
    [[nodiscard]] bool hasNpc(const std::string& npcId) const;
    [[nodiscard]] DecisionContext decisionContext(const std::string& npcId) const;

    void advanceMinutes(std::int64_t minutes);
    CommandResult submit(const Command& command);
    DialogueTurn converse(const std::string& npcId, const std::string& playerInput,
                          const std::string& turnId);
    DialogueTurn deliberate(const std::string& npcId, const std::string& trigger,
                            const std::string& turnId = {});
    // Main-thread boundary for decisions produced by the worker. Returns the
    // number of results consumed; no Provider call is performed here.
    std::size_t pumpAsyncTurns();
    [[nodiscard]] std::size_t pendingAsyncTurns() const;

    CommandResult createPromise(const std::string& from, const std::string& to,
                                const std::string& description, int dueInDays,
                                const std::string& turnId);
    CommandResult decidePromise(const std::string& promiseId, PromiseStatus decision,
                                const std::string& actor, const std::string& turnId,
                                const std::string& detail = {});
    CommandResult createEntity(const std::string& id, const std::string& archetypeId,
                               Json metadata = Json::object(), const std::string& turnId = {});
    CommandResult setEntityLifecycle(const std::string& id, EntityLifecycle lifecycle,
                                     const std::string& turnId = {});
    CommandResult setLinkState(const std::string& linkId, bool traversable, bool locked,
                               const std::string& turnId = {});
    CommandResult transferResource(const std::string& source, const std::string& target,
                                   const std::string& resource, int amount,
                                   const std::string& actor = kWorldActor,
                                   const std::string& turnId = {});
    CommandResult moveNpc(const std::string& npcId, const std::string& destination,
                          const std::string& actor = kWorldActor,
                          const std::string& turnId = {});
    [[nodiscard]] std::optional<std::vector<std::string>> planRoute(const std::string& from,
                                                                     const std::string& to) const;

    [[nodiscard]] Json snapshotJson() const;
    void saveSnapshot(const std::string& path) const;
    void writeReplay(const std::string& path) const;
    static WorldService replay(const std::string& path,
                               std::string contentRoot = ContentPackLoader::defaultRoot(),
                               std::unique_ptr<IProvider> provider = std::make_unique<MockProvider>());

private:
    class TransactionScope;

    struct TransactionFrame {
        WorldState backup;
        std::size_t backupEventCount{0};
        std::size_t backupDeferredEventCount{0};
    };

    WorldState state_;
    std::unique_ptr<IProvider> provider_;
    std::unique_ptr<TurnQueue> turnQueue_;
    EventBus eventBus_;
    EntityRegistry entities_;
    WorldContentPack content_;
    // Each nested scope is a savepoint. A failed recipe can roll back its own
    // partial changes without cancelling the enclosing world-time transition.
    std::vector<TransactionFrame> transactionFrames_;
    std::vector<WorldEvent> deferredEvents_;
    std::set<std::string> pendingAsyncTurnIds_;

    void beginTransaction();
    void commitTransaction();
    void rollbackTransaction();
    void rebuildEntityRegistry();
    void syncNpcEntityState();
    void addEvent(std::string type, std::string actor, std::string target,
                  std::string detail, Json data = Json::object());
    void projectStimuli(const WorldEvent& event);
    void expireStimuli();
    void processAutonomousTurns();
    void enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger);
    [[nodiscard]] TurnRequest makeTurnRequest(const std::string& npcId, const std::string& input,
                                              const std::string& turnId, const std::string& channelId,
                                              const std::string& trigger) const;
    DialogueTurn commitTurn(const TurnRequest& request, ProviderResponse response);
    CommandResult applySecurityOperator(const std::string& operation, const std::string& actor,
                                        const std::string& target, const Json& args,
                                        const std::string& turnId);
    DialogueTurn executeTurn(const std::string& npcId, const std::string& input,
                             const std::string& turnId, const std::string& channelId,
                             const std::string& trigger);
    void processHourBoundary(int day, int hour);
    void processActivity(const ActivityContent& activity);
    [[nodiscard]] bool applyActionEffects(const std::string& npcId, const std::string& actionId);
    [[nodiscard]] bool applyActionEffects(const std::string& npcId, const std::string& actionId,
                                           const std::map<std::string, int>* reservedResources);
    [[nodiscard]] bool evaluateBehaviorCondition(const std::string& npcId,
                                                 const BehaviorNodeContent& node) const;
    [[nodiscard]] bool applyMetaAction(const std::string& npcId, const std::string& actionId,
                                       const BehaviorNodeContent& node,
                                       const std::map<std::string, int>* reservedResources);
    [[nodiscard]] const BehaviorRecipeContent* recipeForAction(const std::string& actionId) const;
    void tickProcesses();
    void resumeSuspendedProcesses();
    void completeProcess(const std::string& npcId, ProcessExecution process);
    void suspendProcess(const std::string& npcId, ProcessExecution& process, const std::string& reason);
    void cancelProcess(const std::string& npcId, ProcessExecution& process, const std::string& reason);
    void releaseReservations(ProcessExecution& process);
    void applyNeedRelief(const std::string& npcId, const std::string& actionId);
    [[nodiscard]] const NpcContent* authoredNpc(const std::string& npcId) const;
    [[nodiscard]] int resourceValue(const std::string& resource) const;
    [[nodiscard]] int availableResourceValue(const std::string& resource) const;
    void setResource(const std::string& resource, int value);
    void processDayBoundary(int day);
    void reviseNpcStrategies(int day);
    void applyPromiseDeadline();
    CommandResult reject(std::string code, std::string message) const;
};

std::string summarize(const WorldState& state);

}  // namespace tide_marks
