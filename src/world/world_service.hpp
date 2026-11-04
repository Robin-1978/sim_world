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
#include "world/persistence.hpp"
#include "world/event_journal.hpp"
#include "world/transaction_manager.hpp"
#include "world/simulation_scheduler.hpp"
#include "world/ids.hpp"
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tide_marks {
struct EvolutionProposal;
struct EvolutionSandboxResult;
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

struct SimulationMetrics {
    std::uint64_t nearNpcTicks{0};
    std::uint64_t farNpcTicks{0};
    std::uint64_t skippedNpcTicks{0};
    std::uint64_t scheduledBoundaries{0};
    std::int64_t simulatedMinutes{0};
};

class WorldService {
public:
    explicit WorldService(std::unique_ptr<IProvider> provider = nullptr,
                          std::unique_ptr<IWorldPersistence> persistence = nullptr);

    static WorldService load(std::string contentRoot = ContentPackLoader::defaultRoot(),
                             std::unique_ptr<IProvider> provider = nullptr);
    static WorldService fromContent(const WorldContentPack& content,
                                    std::unique_ptr<IProvider> provider = nullptr);
    static WorldService fromSnapshot(const Json& json,
                                     std::string contentRoot = ContentPackLoader::defaultRoot(),
                                     std::unique_ptr<IProvider> provider = nullptr);
    static WorldService fromContentSnapshot(const WorldContentPack& content, const Json& json,
                                             std::unique_ptr<IProvider> provider = nullptr);

    [[nodiscard]] const WorldState& state() const { return state_; }
    [[nodiscard]] const WorldContentPack& content() const { return content_; }
    [[nodiscard]] std::string providerName() const { return provider_->name(); }
    [[nodiscard]] EventBus& eventBus() { return eventBus_; }
    [[nodiscard]] const EntityRegistry& entities() const { return entities_; }
    [[nodiscard]] bool hasNpc(const std::string& npcId) const;
    [[nodiscard]] DecisionContext decisionContext(const std::string& npcId) const;

    void advanceMinutes(std::int64_t minutes);
    void advanceTo(GameTime target);
    CommandResult submit(const Command& command);
    DialogueTurn converse(const std::string& npcId, const std::string& playerInput,
                          const std::string& turnId);
    DialogueTurn deliberate(const std::string& npcId, const std::string& trigger,
                            const std::string& turnId = {});
    // Explicit Level 1/Level 2 evolution entry point. It only asks the
    // configured provider for a structured proposal; no world fact changes.
    [[nodiscard]] ProviderResponse requestEvolutionProposal(const std::string& evolutionLevel,
                                                             const std::string& trigger,
                                                             const Json& validationFeedback = Json::object()) const;
    // Main-thread boundary for decisions produced by the worker. Returns the
    // number of results consumed; no Provider call is performed here.
    std::size_t pumpAsyncTurns();
    [[nodiscard]] std::size_t pendingAsyncTurns() const;
    [[nodiscard]] const SimulationMetrics& simulationMetrics() const { return simulationMetrics_; }

    CommandResult createPromise(const std::string& from, const std::string& to,
                                const std::string& description, int dueInDays,
                                const std::string& turnId);
    CommandResult decidePromise(const std::string& promiseId, PromiseStatus decision,
                                const std::string& actor, const std::string& turnId,
                                const std::string& detail = {});
    CommandResult proposeExchange(const std::string& proposer, const std::string& counterparty,
                                  std::map<std::string, int> offered,
                                  std::map<std::string, int> requested, int dueInMinutes,
                                  const std::string& actor, const std::string& turnId = {});
    CommandResult decideExchange(const std::string& contractId, ExchangeContractStatus decision,
                                 const std::string& actor, const std::string& turnId = {},
                                 const std::string& reason = {});
    CommandResult settleExchange(const std::string& contractId, const std::string& actor,
                                 const std::string& turnId = {});
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
    CommandResult tradeResources(const std::string& seller, const std::string& buyer,
                                 const std::string& sellerResource, int sellerAmount,
                                 const std::string& buyerResource, int buyerAmount,
                                 const std::string& actor = kWorldActor,
                                 const std::string& turnId = {});
    CommandResult adjustRelationship(const std::string& npcId, const std::string& target,
                                     RelationshipAxes delta, const std::string& actor = kWorldActor,
                                     const std::string& turnId = {}, const std::string& reason = {});
    CommandResult adjustGroupMetric(const std::string& groupId, const std::string& metric,
                                    double delta, const std::string& actor = kWorldActor,
                                    const std::string& turnId = {}, const std::string& reason = {});
    CommandResult setGroupMembership(const std::string& groupId, const std::string& npcId,
                                     const std::string& role, bool active,
                                     const std::string& actor = kWorldActor,
                                     const std::string& turnId = {});
    CommandResult moveNpc(const std::string& npcId, const std::string& destination,
                          const std::string& actor = kWorldActor,
                          const std::string& turnId = {});
    CommandResult activateOverlay(const EvolutionProposal& proposal, const EvolutionSandboxResult& sandbox);
    CommandResult rollbackOverlay(const std::string& proposalId);
    CommandResult assertKnownFact(const std::string& npcId, const std::string& subject,
                                  const std::string& predicate, const std::string& value,
                                  std::uint64_t sourceSequence, const std::string& scope,
                                  double confidence, const std::string& actor = kWorldActor,
                                  const std::string& turnId = {});
    CommandResult recordExperience(const std::string& npcId, const std::string& domain,
                                   const std::string& trigger, const std::string& action,
                                   const std::string& outcome, std::uint64_t sourceSequence,
                                   double confidence, bool personal = true,
                                   const std::string& actor = kWorldActor,
                                   const std::string& turnId = {});
    [[nodiscard]] std::optional<std::vector<std::string>> planRoute(const std::string& from,
                                                                     const std::string& to) const;

    [[nodiscard]] Json snapshotJson() const;
    void saveSnapshot(const std::string& path) const;
    void writeReplay(const std::string& path) const;
    static WorldService replay(const std::string& path,
                               std::string contentRoot = ContentPackLoader::defaultRoot(),
                               std::unique_ptr<IProvider> provider = nullptr);

private:
    class TransactionScope;

    WorldState state_;
    std::unique_ptr<IProvider> provider_;
    std::unique_ptr<TurnQueue> turnQueue_;
    std::unique_ptr<IWorldPersistence> persistence_;
    EventBus eventBus_;
    EntityRegistry entities_;
    // Immutable content selected by the game instance before approved
    // overlays are applied. It is never overwritten by runtime state.
    WorldContentPack baseContent_;
    WorldContentPack content_;
    TransactionManager transactionManager_;
    EventJournal eventJournal_;
    // Each nested scope is a savepoint. A failed recipe can roll back its own
    // partial changes without cancelling the enclosing world-time transition.
    Json replayBaseline_ = Json::object();
    std::vector<WorldEvent> replayBaselineEvents_;
    std::set<std::string> pendingAsyncTurnIds_;
    std::vector<std::tuple<std::string, std::string, std::string>> pendingDecisionTriggers_;
    SimulationMetrics simulationMetrics_;
    void beginTransaction();
    void commitTransaction();
    void rollbackTransaction();
    void markTransactionChanged();
    void rebuildEntityRegistry();
    void syncNpcEntityState();
    WorldEvent addEvent(std::string type, std::string actor, std::string target,
                        std::string detail, Json data = Json::object());
    void projectStimuli(const WorldEvent& event);
    void expireStimuli();
    void processAutonomousTurns(const std::set<std::string>& dueNpcIds);
    void enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger,
                               const std::string& channelId = {});
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
    void processHourBoundary(int day, int hour, const std::set<std::string>& dueNpcIds);
    void processActivity(const ActivityContent& activity);
    [[nodiscard]] bool applyActionEffects(const std::string& npcId, const std::string& actionId);
    [[nodiscard]] bool applyActionEffects(const std::string& npcId, const std::string& actionId,
                                           const std::map<std::string, int>* reservedResources,
                                           const Json* parameters = nullptr);
    [[nodiscard]] bool evaluateBehaviorCondition(const std::string& npcId,
                                                 const BehaviorNodeContent& node) const;
    [[nodiscard]] bool applyMetaAction(const std::string& npcId, const std::string& actionId,
                                       const BehaviorNodeContent& node,
                                       const std::map<std::string, int>* reservedResources,
                                       const Json* parameters = nullptr);
    [[nodiscard]] const BehaviorRecipeContent* recipeForAction(const std::string& actionId) const;
    void tickProcesses(const std::set<std::string>& dueNpcIds);
    void resumeSuspendedProcesses(const std::set<std::string>& dueNpcIds);
    void completeProcess(const std::string& npcId, ProcessExecution process);
    void suspendProcess(const std::string& npcId, ProcessExecution& process, const std::string& reason,
                        Json eventData = Json::object());
    void cancelProcess(const std::string& npcId, ProcessExecution& process, const std::string& reason);
    void releaseReservations(ProcessExecution& process);
    // Plans every suspension needed to acquire the complete reservation set.
    // The method is read-only; callers apply the plan only after it succeeds.
    [[nodiscard]] std::optional<std::vector<std::string>> planResourcePreemption(
        const std::string& requesterId, const std::map<std::string, int>& requirements,
        int requesterPriority) const;
    void applyNeedRelief(const std::string& npcId, const std::string& actionId);
    [[nodiscard]] const NpcContent* authoredNpc(const std::string& npcId) const;
    [[nodiscard]] std::vector<std::string> overlayIds() const;
    [[nodiscard]] int resourceValue(const std::string& resource) const;
    [[nodiscard]] int availableResourceValue(const std::string& resource) const;
    void setResource(const std::string& resource, int value);
    void processDayBoundary(int day, const std::set<std::string>& dueNpcIds);
    void reviseNpcStrategies(int day);
    void applyPromiseDeadline();
    void applyExchangeDeadlines();
    CommandResult reject(std::string code, std::string message) const;
};

std::string summarize(const WorldState& state);

}  // namespace tide_marks
