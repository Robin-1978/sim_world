#include "tide_marks.hpp"
#include "npc/drive_system.hpp"
#include "world/meta_actions.hpp"
#include "evolution/evolution.hpp"
#include "provider/turn_queue.hpp"
#include "world/world_evolution_system.hpp"

#if defined(TIDE_MARKS_HAS_BULLET)
#include "physics/bullet_physics.hpp"
#endif

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

using namespace tide_marks;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

class CountingProvider final : public IProvider {
public:
    explicit CountingProvider(int& calls) : calls_(calls) {}
    ProviderResponse decide(const DecisionContext& context, const std::string& input) override {
        ++calls_;
        return MockProvider{}.decide(context, input);
    }
    [[nodiscard]] std::string name() const override { return "counting"; }

private:
    int& calls_;
};

class RecordingPersistence final : public IWorldPersistence {
public:
    void saveSnapshot(const std::string& path, const Json& snapshot) const override {
        snapshotPath = path;
        snapshotValue = snapshot;
    }
    void saveReplay(const std::string& path, const std::vector<Json>& lines) const override {
        replayPath = path;
        replayLines = lines;
    }
    void saveDurableCommit(const std::string& path, const Json& snapshot,
                           const std::vector<Json>& lines) const override {
        ++durableCommitCount;
        durableCommitPath = path;
        durableCommitSnapshot = snapshot;
        durableCommitReplay = lines;
    }

    mutable std::string snapshotPath;
    mutable Json snapshotValue;
    mutable std::string replayPath;
    mutable std::vector<Json> replayLines;
    mutable std::string durableCommitPath;
    mutable Json durableCommitSnapshot;
    mutable std::vector<Json> durableCommitReplay;
    mutable int durableCommitCount{0};
};

class DelayedProvider final : public IProvider {
public:
    explicit DelayedProvider(std::chrono::milliseconds delay) : delay_(delay) {}
    ProviderResponse decide(const DecisionContext& context, const std::string& input) override {
        ++calls;
        std::this_thread::sleep_for(delay_);
        return MockProvider{}.decide(context, input);
    }
    [[nodiscard]] std::string name() const override { return "delayed"; }

    std::atomic<int> calls{0};

private:
    std::chrono::milliseconds delay_;
};

class BlockingProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string& input) override {
        ++calls;
        {
            std::unique_lock lock(mutex_);
            started_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return released_; });
        }
        return MockProvider{}.decide(context, input);
    }
    [[nodiscard]] std::string name() const override { return "blocking"; }

    void waitUntilStarted() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return started_; });
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    std::atomic<int> calls{0};

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool started_{false};
    bool released_{false};
};

class SpeechActProvider final : public IProvider {
public:
    SpeechActProvider(std::string speechAct, std::string actionId)
        : speechAct_(std::move(speechAct)), actionId_(std::move(actionId)) {}

    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        IntentProposal proposal;
        proposal.actionId = actionId_;
        proposal.speechAct = speechAct_;
        proposal.intent = speechAct_;
        proposal.rationale = "The provider interpreted the utterance.";
        proposal.expectedOutcome = "Runtime validates the selected speech act and action.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.worldActionRequested = !proposal.actionId.empty();
        return ProviderResponse{true, std::move(proposal), "The model chose how to answer.", {}};
    }
    [[nodiscard]] std::string name() const override { return "semantic-test"; }

private:
    std::string speechAct_;
    std::string actionId_;
};

class EmptyDialogueProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        IntentProposal proposal;
        proposal.speechAct = "information_request";
        proposal.intent = "respond";
        proposal.rationale = "The test provider intentionally omitted the player-facing answer.";
        proposal.expectedOutcome = "Runtime must reject an empty conversation response.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.worldActionRequested = false;
        return ProviderResponse{true, std::move(proposal), "   \n", {}};
    }
    [[nodiscard]] std::string name() const override { return "semantic-test"; }
};

class FailingProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        observedContext = context;
        return ProviderResponse{false, {}, {}, "provider_unavailable"};
    }
    [[nodiscard]] std::string name() const override { return "failing"; }

    DecisionContext observedContext;
};

class ScriptedActionProvider final : public IProvider {
public:
    explicit ScriptedActionProvider(std::vector<std::string> actions,
                                    std::map<std::string, Json> parameters = {})
        : actions_(std::move(actions)), parameters_(std::move(parameters)) {}

    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        if (next_ >= actions_.size()) return ProviderResponse{false, {}, {}, "script_exhausted"};
        IntentProposal proposal;
        proposal.actionId = actions_.at(next_++);
        proposal.speechAct = "request";
        proposal.intent = proposal.actionId;
        proposal.rationale = "A deterministic test provider selected the authored proposal.";
        proposal.expectedOutcome = "Runtime validates the authored action.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.worldActionRequested = true;
        if (const auto parameters = parameters_.find(proposal.actionId); parameters != parameters_.end()) {
            proposal.parameters = parameters->second;
        }
        return ProviderResponse{true, std::move(proposal), "The test provider selected an action.", {}};
    }
    [[nodiscard]] std::string name() const override { return "scripted-test"; }

private:
    std::vector<std::string> actions_;
    std::map<std::string, Json> parameters_;
    std::size_t next_{0};
};

class RepairingLlamaProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        ++calls;
        return makeProposal(context, "offer_ration", Json{{"unexpected", true}});
    }

    ProviderResponse decide(const DecisionContext& context, const std::string&,
                            const Json& validationFeedback) override {
        ++calls;
        sawFeedback = sawFeedback || !validationFeedback.empty();
        return validationFeedback.empty()
                   ? makeProposal(context, context.candidates.empty() ? "" : context.candidates.front().actionId,
                                  Json{{"unexpected", true}})
                   : makeProposal(context, context.candidates.empty() ? "" : context.candidates.front().actionId,
                                  Json::object());
    }

    [[nodiscard]] std::string name() const override { return "llama.cpp"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override {
        return ProviderCapabilities{true, false, true};
    }

    int calls{0};
    bool sawFeedback{false};

private:
    static ProviderResponse makeProposal(const DecisionContext& context,
                                         const std::string& action,
                                         Json parameters) {
        IntentProposal proposal;
        proposal.actionId = action;
        proposal.speechAct = context.speechAct;
        proposal.intent = action;
        proposal.rationale = "A test LLM repaired its authored parameter contract.";
        proposal.expectedOutcome = "Runtime executes the repaired authored plan.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.worldActionRequested = true;
        proposal.parameters = std::move(parameters);
        return ProviderResponse{true, std::move(proposal), "The model selected a repaired plan.", {}};
    }
};

class RepairingExpectedReliefProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        ++calls;
        return makeProposal(context, {"resource_cost", 0.2});
    }

    ProviderResponse decide(const DecisionContext& context, const std::string&,
                            const Json& validationFeedback) override {
        ++calls;
        sawFeedback = sawFeedback || !validationFeedback.empty();
        return validationFeedback.empty() ? makeProposal(context, {"resource_cost", 0.2})
                                          : makeProposal(context, {});
    }

    [[nodiscard]] std::string name() const override { return "llama.cpp"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override {
        return ProviderCapabilities{true, false, true};
    }

    int calls{0};
    bool sawFeedback{false};

private:
    static ProviderResponse makeProposal(const DecisionContext& context,
                                         std::pair<std::string, double> relief) {
        IntentProposal proposal;
        proposal.actionId = "offer_ration";
        proposal.speechAct = context.speechAct;
        proposal.intent = "offer_ration";
        proposal.rationale = "A test LLM repaired its expected relief contract.";
        proposal.expectedOutcome = "Runtime executes the repaired authored plan.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.worldActionRequested = true;
        if (!relief.first.empty()) proposal.expectedRelief[relief.first] = relief.second;
        return ProviderResponse{true, std::move(proposal), "The model selected a repaired plan.", {}};
    }
};

class NpcActionProvider final : public IProvider {
public:
    explicit NpcActionProvider(std::map<std::string, std::string> actions)
        : actions_(std::move(actions)) {}

    ProviderResponse decide(const DecisionContext& context, const std::string&) override {
        const auto action = actions_.find(context.npcId);
        if (action == actions_.end()) return ProviderResponse{false, {}, {}, "unsupported_npc"};
        IntentProposal proposal;
        proposal.actionId = action->second;
        proposal.speechAct = "request";
        proposal.intent = action->second;
        proposal.rationale = "A deterministic test provider selected the authored arbitration case.";
        proposal.expectedOutcome = "Runtime validates the full resource acquisition plan.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.worldActionRequested = true;
        return ProviderResponse{true, std::move(proposal), "The test provider selected an action.", {}};
    }
    [[nodiscard]] std::string name() const override { return "npc-scripted-test"; }

private:
    std::map<std::string, std::string> actions_;
};

class EvolutionProposalProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string& input) override {
        return MockProvider{}.decide(context, input);
    }

    ProviderResponse proposeEvolution(const Json& context, const std::string& level) override {
        observedLevel = level;
        observedContext = context;
        observedTargets.push_back(context.value("target_npc", ""));
        const auto evidence = context.at("evidence_events").at(0).at("sequence");
        const auto targetNpc = context.value("target_npc", "mira");
        Json proposal;
        if (level == "level1") {
            Json operation{{"operation", "overlay_strategy"}, {"id", "llm.strategy.rule"},
                           {"npc_id", targetNpc}, {"peer_id", targetNpc == "mira" ? "lorn" : "mira"},
                           {"min_peer_trust", 0.0},
                           {"strategy", "cooperate"}, {"goal_stage", 1},
                           {"memory", "The model proposed a cooperative strategy."}};
            proposal = Json::object();
            proposal["id"] = "llm.strategy.overlay";
            proposal["kind"] = "strategy";
            proposal["evidence_event_sequences"] = Json::array({evidence});
            proposal["operations"] = Json::array({operation});
            proposal["rationale"] = "The observed social evidence supports a bounded strategy change.";
        } else {
            Json operation{{"operation", "create_entity_archetype"}, {"id", "llm.market_stall"},
                           {"kind", "facility"}};
            proposal = Json::object();
            proposal["id"] = "llm.archetype";
            proposal["kind"] = "declarative_capability";
            proposal["evidence_event_sequences"] = Json::array({evidence});
            proposal["operations"] = Json::array({operation});
            proposal["rationale"] = "The model proposed a declarative facility grounded in the event history.";
        }
        return ProviderResponse{true, {}, proposal.at("rationale").get<std::string>(), {}, proposal};
    }

    ProviderResponse proposeReflection(const Json& context, const std::string& targetNpcId) override {
        observedReflectionTargets.push_back(targetNpcId);
        Json result{{"schema_version", 1}, {"npc_id", targetNpcId},
                    {"evidence_event_sequences", Json::array()},
                    {"cognition_updates", Json{{"belief_updates", Json::array()},
                                                   {"experience_updates", Json::array()},
                                                   {"memory_updates", Json::array()}}},
                    {"strategy_adjustment", Json::object()}, {"goal_adjustment", Json::object()},
                    {"unresolved_issues", context.value("unresolved_issues", Json::array())},
                    {"resolved_issues", Json::array()}, {"innovation_hypotheses", Json::array()},
                    {"innovation_actions", Json::array()},
                    {"rationale", "No durable update was justified by this reflection window."}};
        ProviderResponse response;
        response.ok = true;
        response.reflectionResult = std::move(result);
        return response;
    }

    [[nodiscard]] std::string name() const override { return "evolution-test-llm"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override {
        return ProviderCapabilities{true, true, true};
    }
    std::string observedLevel;
    Json observedContext = Json::object();
    std::vector<std::string> observedTargets;
    std::vector<std::string> observedReflectionTargets;
};

class ReflectionProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context, const std::string& input) override {
        return MockProvider{}.decide(context, input);
    }

    ProviderResponse proposeReflection(const Json& context, const std::string& targetNpcId) override {
        reflectionContexts.push_back(context);
        const auto evidence = context.value("new_evidence", Json::array());
        const auto source = evidence.empty() ? 0ULL : evidence.at(0).at("sequence").get<std::uint64_t>();
        Json result{{"schema_version", 1}, {"npc_id", targetNpcId},
                    {"evidence_event_sequences", source == 0 ? Json::array() : Json::array({source})},
                    {"cognition_updates", Json{{"belief_updates", source == 0 ? Json::array() : Json::array({Json{
                         {"subject", "harbor"}, {"predicate", "safe"}, {"value", "known"},
                         {"source_sequence", source}, {"scope", "witness"}, {"confidence", 0.8}}})},
                                                   {"experience_updates", Json::array()}, {"memory_updates", Json::array()}}},
                    {"strategy_adjustment", Json{{"strategy", "reflective"}}},
                    {"goal_adjustment", Json::object()},
                    {"unresolved_issues", Json::array({"verify the harbor signal"})},
                    {"resolved_issues", Json::array()}, {"innovation_hypotheses", Json::array()},
                    {"innovation_actions", Json::array()},
                    {"rationale", "The evidence supports a bounded belief update."}};
        ProviderResponse response;
        response.ok = true;
        response.reflectionResult = std::move(result);
        return response;
    }

    [[nodiscard]] std::string name() const override { return "reflection-test-llm"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override {
        return ProviderCapabilities{true, true, true};
    }
    std::vector<Json> reflectionContexts;
};

void village_has_three_independent_npcs() {
    const auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    expect(service.content().instance && service.content().instance->id == "tide_marks.village.instance",
           "default world is launched through an explicit game instance");
    expect(service.state().npcs.size() == 3, "village has exactly three important NPCs");
    const auto definitionFor = [&service](const std::string& id) -> const NpcDefinition* {
        for (const auto& npc : service.content().npcs) {
            if (npc.definition.id == id) return &npc.definition;
        }
        return nullptr;
    };
    expect(definitionFor("mira") != nullptr && definitionFor("lorn") != nullptr &&
               definitionFor("mira")->hiddenGoal != definitionFor("lorn")->hiddenGoal,
           "NPC hidden goals are distinct");
    expect(service.state().npcs.at("mira").location == "granary", "Mira starts at the granary");
    const std::vector<NeedContent> needs{
        NeedContent{"food_security", "physiological", "food", 0.25, 0.02, 0.7, 1.0, 0.1,
                    {{"offer_ration", NeedSatisfier{0.2, {"relieve_need"}}}}}};
    const auto ranking = GoalSelector::rank(service.state().npcs.at("mira"), needs);
    expect(!ranking.empty() && ranking.front().score >= ranking.back().score, "GoalSelector ranks authored goals");
    expect(service.content().sceneAsset == "../assets/blender/village.glb" &&
               service.content().cameraMode == "slanted_top_down",
           "presentation defaults are selected by content rather than the client");
}

void goal_ranking_aggregates_all_satisfied_needs() {
    NpcState npc;
    NeedContent food;
    food.id = "food";
    food.domain = "physiological";
    food.dimension = "food";
    food.weight = 1.0;
    food.priority = 0.1;
    food.criticalThreshold = 0.7;
    food.satisfiers["shared"] = NeedSatisfier{0.4, {"relieve_need"}};
    food.satisfiers["food_only"] = NeedSatisfier{0.4, {"relieve_need"}};
    NeedContent social;
    social.id = "social";
    social.domain = "social";
    social.dimension = "belonging";
    social.weight = 0.7;
    social.priority = 0.05;
    social.criticalThreshold = 0.8;
    social.satisfiers["shared"] = NeedSatisfier{0.3, {"relieve_need"}};
    npc.needs["food"] = NeedState{0.9, GameTime{}, 0, 1.0, {{"shared", 0.4}, {"food_only", 0.4}}, 0.5};
    npc.needs["social"] = NeedState{0.8, GameTime{}, 0, 0.7, {{"shared", 0.3}}, 0.5};
    const std::vector<NeedContent> needs{food, social};
    const auto foodEvaluation = DriveSystem::evaluate(food, npc.needs.at("food"), "shared");
    const auto socialEvaluation = DriveSystem::evaluate(social, npc.needs.at("social"), "shared");
    const auto ranking = GoalSelector::rank(npc, needs);
    const auto shared = std::find_if(ranking.begin(), ranking.end(), [](const auto& choice) {
        return choice.actionId == "shared";
    });
    const auto foodOnly = std::find_if(ranking.begin(), ranking.end(), [](const auto& choice) {
        return choice.actionId == "food_only";
    });
    expect(shared != ranking.end() && foodOnly != ranking.end(), "goal ranking includes all satisfiers");
    if (shared != ranking.end() && foodOnly != ranking.end()) {
        const double expected = foodEvaluation.utilityContribution + socialEvaluation.utilityContribution;
        expect(std::abs(shared->score - expected) < 1e-9,
               "one action aggregates utility from every need it satisfies");
        expect(shared->needContributions.size() == 2 && shared->score > foodOnly->score,
               "multi-need utility changes candidate order and remains attributable");
    }
}

void drive_confidence_is_applied_once() {
    NeedContent need;
    need.id = "food";
    need.domain = "physiological";
    need.weight = 1.0;
    need.priority = 0.2;
    need.criticalThreshold = 0.8;
    need.satisfiers["share"] = NeedSatisfier{0.4, {}};
    NeedState state;
    state.deficit = 0.9;
    state.sensitivity = 1.0;
    state.evidenceConfidence = 0.5;
    state.expectedRelief["share"] = 0.4;
    const auto evaluation = DriveSystem::evaluate(need, state, "share");
    const double expected = evaluation.effectiveWeight * evaluation.expectedRelief * evaluation.confidence;
    expect(std::abs(evaluation.utilityContribution - expected) < 1e-9,
           "drive confidence is applied once at the final utility boundary");
    expect(std::abs(evaluation.expectedRelief - 0.4) < 1e-9,
           "expected relief remains the learned amount instead of embedding confidence");
}

void private_traits_change_stable_action_order() {
    NpcState npc;
    NeedContent need;
    need.id = "social_need";
    need.domain = "social";
    need.satisfiers = {
        {"cooperate", NeedSatisfier{0.4, {}}},
        {"explore", NeedSatisfier{0.4, {}}},
    };
    ActionCandidate cooperate;
    cooperate.actionId = "cooperate";
    cooperate.traitEffects["cooperation"] = 1.0;
    ActionCandidate explore;
    explore.actionId = "explore";
    explore.traitEffects["cooperation"] = -1.0;

    const auto cooperative = GoalSelector::rank(npc, {need}, {cooperate, explore},
                                                { {"cooperation", 0.9} }, 1.5);
    const auto independent = GoalSelector::rank(npc, {need}, {cooperate, explore},
                                                { {"cooperation", 0.1} }, 1.5);
    expect(!cooperative.empty() && !independent.empty() && cooperative.front().actionId == "cooperate" &&
               independent.front().actionId == "explore",
           "the same state and candidates produce stable divergent rankings from private traits");
    expect(cooperative.front().utilityTerms.contains("trait_total") &&
               cooperative.front().utilityTerms.at("trait_total") <= 1.0,
           "nonlinear trait preference is bounded and auditable");

    const auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto mira = service.decisionContext("mira");
    const auto eve = service.decisionContext("eve");
    expect(mira.privateTraits.value("cooperation", 0.0) > eve.privateTraits.value("cooperation", 0.0) &&
               mira.privateTraits.value("secrecy", 0.0) < eve.privateTraits.value("secrecy", 0.0),
           "content supplies distinct private traits to each NPC context");
}

void authored_utility_profile_is_bounded_and_auditable() {
    NpcState npc;
    NeedContent need{"food", "physiological", "food", 0.8, 0.0, 0.9, 1.0, 0.0,
                     {{"trade", NeedSatisfier{0.5, {}}}, {"risky", NeedSatisfier{0.5, {}}}}};
    npc.needs["food"] = NeedState{0.8, {}, 0, 1.0, {}, 1.0};
    ActionCandidate trade;
    trade.actionId = "trade";
    trade.futureValue = 0.2;
    trade.resourceCost = 0.1;
    ActionCandidate risky;
    risky.actionId = "risky";
    risky.futureValue = 0.2;
    risky.risk = 0.4;
    const auto ranked = GoalSelector::rank(npc, {need}, {trade, risky});
    expect(ranked.size() == 2 && ranked.front().actionId == "trade",
           "bounded authored utility terms participate in deterministic action ranking");
    expect(ranked.front().utilityTerms.at("future_value") == 0.2 &&
               ranked.front().utilityTerms.at("resource_cost") == -0.1,
           "utility ranking exposes each authored cost and benefit term for audit");
}

void offline_progression_changes_village() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto before = summarize(service.state());
    service.advanceMinutes(2 * kMinutesPerDay);
    const auto after = summarize(service.state());
    expect(before != after, "two days change the deterministic world");
    int changed = 0;
    for (const auto& [id, npc] : service.state().npcs) {
        if (npc.goalStage > 0 || npc.stateVersion > 0) ++changed;
    }
    expect(changed >= 2, "at least two NPCs changed while player was away");
    expect(service.state().events.size() >= 9, "life events are recorded");
}

void needs_are_data_driven_and_replayable() {
    // This test isolates deterministic need decay and the explicit player
    // action. Autonomous LLM turns are covered separately; leaving them
    // enabled here would let a mock decision relieve the very deficit this
    // assertion is measuring.
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.enabled = false;
    auto service = WorldService::fromContent(content, std::make_unique<MockProvider>());
    const auto before = service.state().npcs.at("mira").needs.at("food_security").deficit;
    service.advanceMinutes(60);
    const auto after = service.state().npcs.at("mira").needs.at("food_security").deficit;
    expect(after > before, "need deficit advances from game time");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(),
                       [](const auto& event) { return event.type == "need.deprived"; }),
           "need advancement is recorded as a world event");
    const auto turn = service.converse("mira", "please offer a ration", "need-relief");
    expect(turn.accepted, "an authored action can be committed");
    expect(service.state().npcs.at("mira").activity.process.has_value(),
           "an accepted duration-backed intent persists as an active process");
    service.advanceMinutes(10);
    expect(service.state().npcs.at("mira").mode == "executing",
           "a running process keeps the NPC in executing mode across hour boundaries");
    service.advanceMinutes(20);
    expect(service.state().npcs.at("mira").needs.at("food_security").deficit < after,
           "committed action relieves the matching need");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(),
                       [](const auto& event) { return event.type == "need.relieved"; }),
           "need relief is recorded as a world event");
    service.advanceMinutes(2 * kMinutesPerDay);
    expect(std::any_of(service.state().events.begin(), service.state().events.end(),
                       [](const auto& event) { return event.type == "need.critical"; }),
           "critical need transitions are recorded as a world event");
    const auto snapshot = service.snapshotJson();
    const auto restored = WorldService::fromSnapshot(snapshot);
    expect(restored.state().npcs.at("mira").needs.at("food_security").deficit ==
               service.state().npcs.at("mira").needs.at("food_security").deficit,
           "need state survives snapshot restore");
}

void accepted_intent_runs_without_reasking_llm() {
    int providerCalls = 0;
    auto service = WorldService::load(ContentPackLoader::defaultRoot(),
                                      std::make_unique<CountingProvider>(providerCalls));
    const auto turn = service.converse("mira", "please offer a ration", "long-process");
    expect(turn.accepted, "LLM proposal starts an authored process");
    expect(providerCalls == 1, "provider is consulted once for the persistent intent");
    const auto* process = service.state().npcs.at("mira").activity.process ?
                              &*service.state().npcs.at("mira").activity.process : nullptr;
    expect(process != nullptr && process->status == ProcessStatus::Running,
           "runtime owns the running process after the proposal returns");
    const auto snapshot = service.snapshotJson();
    auto restored = WorldService::fromSnapshot(snapshot);
    expect(restored.state().npcs.at("mira").activity.process.has_value(),
           "an in-flight process survives snapshot restore");
    expect(restored.state().npcs.at("mira").activity.process->reservationMode == "input_resources",
           "a process restores its authored reservation policy");
    restored.advanceMinutes(29);
    expect(restored.state().resources.at("grain").amount == 100,
           "long-running effects are not committed before completion");
    expect(restored.state().resources.at("grain").reserved == 5,
           "inputs are reserved while the process runs");
    restored.advanceMinutes(1);
    expect(restored.state().resources.at("grain").amount == 95,
           "completion transaction applies the authored resource effect");
    expect(restored.state().resources.at("grain").reserved == 0,
           "completion releases the process reservation");
    expect(!restored.state().npcs.at("mira").activity.process,
           "completed process leaves the active slot");
    expect(std::any_of(restored.state().events.begin(), restored.state().events.end(), [](const auto& event) {
               return event.type == "npc.process_completed";
           }),
           "completion is recorded as a world event");
    expect(providerCalls == 1, "world time advances without another model call");
}

void reservation_policy_is_authored_per_process() {
    auto service = WorldService::load(
        ContentPackLoader::defaultRoot(),
        std::make_unique<SpeechActProvider>("request", "rest"));
    const auto turn = service.converse("mira", "please sleep", "reservation-none");
    expect(turn.accepted, "a long process without resource inputs can start");
    const auto& process = service.state().npcs.at("mira").activity.process;
    expect(process && process->actionId == "rest" && process->reservationMode == "none",
           "duration does not implicitly enable resource reservation");
    expect(process && process->reservedResources.empty() && service.state().resources.at("grain").reserved == 0,
           "a none-policy process leaves the shared resource ledger untouched");
}

void resource_preemption_is_declared_and_atomic() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    BehaviorRecipeContent lowRecipe;
    lowRecipe.id = "low_grain_work";
    lowRecipe.planId = lowRecipe.id;
    lowRecipe.durationMinutes = 120;
    lowRecipe.interruptible = true;
    lowRecipe.interruptSalienceThreshold = 1.0;
    lowRecipe.reservationMode = "input_resources";
    lowRecipe.priority = 1;
    lowRecipe.root = BehaviorNodeContent{
        "sequence", "low_grain_work.sequence", "", Json::object(), 0,
        {BehaviorNodeContent{"meta_action", "low_grain_work.consume", "consume_resource",
                             Json{{"resource", "grain"}, {"amount", 100}}, 0, {}}}};
    content.behaviorRecipes.push_back(lowRecipe);
    auto lorn = std::find_if(content.npcs.begin(), content.npcs.end(), [](const auto& npc) {
        return npc.definition.id == "lorn";
    });
    expect(lorn != content.npcs.end(), "the arbitration fixture has a second NPC");
    if (lorn == content.npcs.end()) return;
    lorn->actions.push_back(ActionCandidate{"low_grain_work", lowRecipe.planId,
                                             "test", lowRecipe.durationMinutes,
                                             lowRecipe.interruptible,
                                             lowRecipe.interruptSalienceThreshold,
                                             lowRecipe.reservationMode, lowRecipe.id,
                                             {{"grain", 100}}, lowRecipe.priority,
                                             lowRecipe.preemptionPolicy});
    auto service = WorldService::fromContent(
        content, std::make_unique<NpcActionProvider>(std::map<std::string, std::string>{
            {"lorn", "low_grain_work"}, {"mira", "offer_ration"}}));
    expect(service.converse("lorn", "start the low priority work", "preempt-low").accepted,
           "a lower-priority process can reserve the shared resource");
    expect(service.state().resources.at("grain").reserved == 100,
           "the low-priority process owns the full reservation");
    expect(service.converse("mira", "urgent ration", "preempt-high").accepted,
           "a configured higher-priority process can acquire a lower-priority reservation");
    expect(service.state().npcs.at("lorn").activity.status == ActivityStatus::Suspended &&
               service.state().npcs.at("lorn").activity.process->interruptionReason == "resource_preempted",
           "preemption suspends the selected lower-priority process");
    expect(service.state().resources.at("grain").reserved == 5,
           "preemption releases the victim reservation before acquiring the new one");
    const auto preempted = std::count_if(service.state().events.begin(), service.state().events.end(),
                                         [](const auto& event) {
                                             return event.type == "npc.process_suspended" &&
                                                    event.data.value("reason", "") == "resource_preempted";
                                         });
    expect(preempted == 1, "preemption is represented by one auditable suspension event");

    auto impossibleContent = content;
    for (auto& npc : impossibleContent.npcs) {
        if (npc.definition.id != "mira") continue;
        for (auto& action : npc.actions) {
            if (action.actionId != "offer_ration") continue;
            action.reservations["grain"] = 101;
            action.preemptionPolicy = "lower_priority";
            action.priority = 10;
        }
    }
    auto impossible = WorldService::fromContent(
        impossibleContent, std::make_unique<NpcActionProvider>(std::map<std::string, std::string>{
            {"lorn", "low_grain_work"}, {"mira", "offer_ration"}}));
    expect(impossible.converse("lorn", "start", "atomic-low").accepted,
           "the atomicity case starts its victim process");
    const auto rejected = impossible.converse("mira", "impossible", "atomic-high");
    expect(!rejected.accepted && rejected.error == "insufficient_resource",
           "an impossible preemption request is rejected");
    expect(impossible.state().npcs.at("lorn").activity.status == ActivityStatus::Active &&
               impossible.state().resources.at("grain").reserved == 100,
           "a rejected request leaves the victim and reservations unchanged");
    expect(std::none_of(impossible.state().events.begin(), impossible.state().events.end(),
                        [](const auto& event) { return event.data.value("reason", "") == "resource_preempted"; }),
           "an unfulfillable plan emits no partial preemption event");
}

void exchange_contracts_are_durable_and_atomic() {
    auto service = WorldService::load(TIDE_MARKS_TEST_DATA_DIR "/port-pack",
                                      std::make_unique<MockProvider>());
    const auto proposed = service.proposeExchange("sable", "rhea", {{"energy", 1}},
                                                 {{"coolant", 1}}, 30, "sable", "contract-propose");
    expect(proposed.accepted, "a configured exchange proposal is recorded");
    const auto proposerCannotAccept = service.decideExchange(
        proposed.message, ExchangeContractStatus::Accepted, "sable", "contract-proposer-accept");
    expect(!proposerCannotAccept.accepted && proposerCannotAccept.code == "counterparty_required",
           "the proposer cannot accept its own offer");
    const auto strangerCannotAccept = service.decideExchange(
        proposed.message, ExchangeContractStatus::Accepted, "outsider", "contract-stranger-accept");
    expect(!strangerCannotAccept.accepted && strangerCannotAccept.code == "unauthorized",
           "a non-party cannot decide an exchange");
    const auto accepted = service.decideExchange(proposed.message, ExchangeContractStatus::Accepted,
                                                 "npc:rhea", "contract-accept");
    expect(accepted.accepted && service.state().exchangeContracts.at(proposed.message).status ==
               ExchangeContractStatus::Accepted,
           "the counterparty can accept a live exchange");
    const auto settled = service.settleExchange(proposed.message, "rhea", "contract-settle");
    expect(settled.accepted && service.state().exchangeContracts.at(proposed.message).status ==
               ExchangeContractStatus::Settled,
           "an accepted exchange settles through the atomic transfer path");
    expect(service.state().inventories.at("npc:sable").at("energy") == 2 &&
               service.state().inventories.at("npc:sable").at("coolant") == 1 &&
               service.state().inventories.at("npc:rhea").at("energy") == 1 &&
               service.state().inventories.at("npc:rhea").at("coolant") == 1,
           "settlement exchanges both sides without partial ownership changes");
    auto restored = WorldService::fromSnapshot(service.snapshotJson(),
                                               TIDE_MARKS_TEST_DATA_DIR "/port-pack",
                                               std::make_unique<MockProvider>());
    expect(restored.state().exchangeContracts.at(proposed.message).status ==
               ExchangeContractStatus::Settled,
           "contract status survives snapshot restore");

    auto insufficient = WorldService::load(TIDE_MARKS_TEST_DATA_DIR "/port-pack",
                                           std::make_unique<MockProvider>());
    const auto impossible = insufficient.proposeExchange("sable", "rhea", {{"energy", 99}},
                                                         {{"coolant", 1}}, 30, "sable", "contract-bad");
    expect(impossible.accepted, "a contract can be proposed before delivery is possible");
    expect(insufficient.decideExchange(impossible.message, ExchangeContractStatus::Accepted,
                                       "rhea", "contract-bad-accept").accepted,
           "the counterparty can accept the contract before settlement");
    const auto failed = insufficient.settleExchange(impossible.message, "rhea", "contract-bad-settle");
    expect(!failed.accepted && failed.code == "insufficient_resource",
           "settlement rejects when a party lacks its delivery terms");
    expect(insufficient.state().inventories.at("npc:sable").at("energy") == 3 &&
               insufficient.state().inventories.at("npc:rhea").at("coolant") == 2 &&
               insufficient.state().exchangeContracts.at(impossible.message).status ==
                   ExchangeContractStatus::Accepted,
           "failed settlement preserves both inventories and the accepted contract");
    expect(std::any_of(insufficient.state().events.begin(), insufficient.state().events.end(),
                       [&impossible](const auto& event) {
                           return event.type == "exchange.settlement_rejected" &&
                                  event.target == impossible.message &&
                                  event.data.value("code", "") == "insufficient_resource";
                       }),
           "a failed settlement records its reason without moving resources");

    auto secondSideFailure = WorldService::load(TIDE_MARKS_TEST_DATA_DIR "/port-pack",
                                                std::make_unique<MockProvider>());
    const auto secondSideProposal = secondSideFailure.proposeExchange(
        "sable", "rhea", {{"energy", 1}}, {{"coolant", 99}}, 30, "sable", "contract-second-bad");
    expect(secondSideFailure.decideExchange(secondSideProposal.message,
                                            ExchangeContractStatus::Accepted, "rhea",
                                            "contract-second-bad-accept").accepted,
           "the second-side failure fixture starts from an accepted contract");
    const auto secondSideRejected = secondSideFailure.settleExchange(
        secondSideProposal.message, "rhea", "contract-second-bad-settle");
    expect(!secondSideRejected.accepted && secondSideRejected.code == "insufficient_resource" &&
               secondSideFailure.state().inventories.at("npc:sable").at("energy") == 3 &&
               secondSideFailure.state().inventories.at("npc:rhea").at("coolant") == 2,
           "failure after the first staged transfer rolls both sides back");

    insufficient.advanceMinutes(30);
    expect(insufficient.state().exchangeContracts.at(impossible.message).status ==
               ExchangeContractStatus::Expired,
           "unsettled contracts expire at their authored deadline");
    expect(std::any_of(insufficient.state().events.begin(), insufficient.state().events.end(),
                       [&impossible](const auto& event) {
                           return event.type == "exchange.expired" &&
                                  event.target == impossible.message &&
                                  event.data.value("proposer", "") == "sable" &&
                                  event.data.value("counterparty", "") == "rhea";
                       }),
           "expiry identifies the contract while retaining both parties as evidence");
    const auto lateAcceptance = insufficient.decideExchange(
        impossible.message, ExchangeContractStatus::Accepted, "rhea", "contract-late-accept");
    expect(!lateAcceptance.accepted && lateAcceptance.code == "exchange_expired",
           "an expired contract cannot be revived");

    auto inactive = WorldService::load(TIDE_MARKS_TEST_DATA_DIR "/port-pack",
                                       std::make_unique<MockProvider>());
    const auto inactiveProposal = inactive.proposeExchange(
        "sable", "rhea", {{"energy", 1}}, {{"coolant", 1}}, 30, "sable", "contract-inactive");
    expect(inactive.decideExchange(inactiveProposal.message, ExchangeContractStatus::Accepted,
                                   "rhea", "contract-inactive-accept").accepted,
           "the inactive-party fixture starts from an accepted contract");
    expect(inactive.setEntityLifecycle(npcEntityId("rhea"), EntityLifecycle::Dormant,
                                       "contract-dormant-rhea").accepted,
           "an exchange party can become dormant after acceptance");
    const auto inactiveSettlement = inactive.settleExchange(
        inactiveProposal.message, "sable", "contract-inactive-settle");
    expect(!inactiveSettlement.accepted && inactiveSettlement.code == "inactive_exchange_party",
           "settlement revalidates both parties at commit time");
    const auto inactiveCancellation = inactive.decideExchange(
        inactiveProposal.message, ExchangeContractStatus::Cancelled, "rhea",
        "contract-inactive-cancel");
    expect(!inactiveCancellation.accepted && inactiveCancellation.code == "inactive_exchange_party",
           "an inactive party cannot mutate a contract after losing lifecycle authority");
}

void autonomous_deliberation_is_triggered_not_per_tick() {
    int providerCalls = 0;
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    content.decisionChannels.at(content.autonomousChannelId).allowInterveningWorldChanges = false;
    content.npcDeliberation.cooldownMinutes = 360;
    auto service = WorldService::fromContent(content, std::make_unique<CountingProvider>(providerCalls));
    service.advanceMinutes(60);
    expect(providerCalls == 3, "a high-pressure hourly boundary triggers one autonomous deliberation per eligible NPC");
    service.advanceMinutes(60);
    expect(providerCalls == 3, "ordinary tick progression does not repeatedly call the model");
}

void async_turn_queue_does_not_commit_expired_results() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    content.decisionChannels.at(content.autonomousChannelId).allowInterveningWorldChanges = false;
    auto provider = std::make_unique<DelayedProvider>(std::chrono::milliseconds(60));
    auto* providerRaw = provider.get();
    auto service = WorldService::fromContent(content, std::move(provider));

    service.advanceMinutes(60);
    expect(service.pendingAsyncTurns() > 0, "autonomous decisions remain pending while the provider works");
    const auto revisionBeforeMutation = service.state().revision;
    expect(service.createPromise("player", "mira", "confirm the delayed decision", 1, "async-mutation").accepted,
           "the main thread can commit a fact while provider work is pending");
    expect(service.state().revision > revisionBeforeMutation, "the intervening fact advances world revision");
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    const auto consumed = service.pumpAsyncTurns();
    expect(consumed > 0 && providerRaw->calls.load() > 0, "completed provider results are drained on the main thread");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.intent_expired";
           }),
           "a result from an obsolete world revision is audited as expired");
    for (const auto& [id, npc] : service.state().npcs) {
        expect(!npc.activity.process, "an expired asynchronous proposal cannot start an activity for " + id);
    }
}

void async_turn_queue_commits_results_with_current_context() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    auto provider = std::make_unique<DelayedProvider>(std::chrono::milliseconds(40));
    auto service = WorldService::fromContent(content, std::move(provider));

    service.advanceMinutes(60);
    expect(service.pendingAsyncTurns() > 0, "autonomous decisions are queued without blocking the world boundary");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto consumed = service.pumpAsyncTurns();
    expect(consumed > 0, "completed autonomous decisions are consumed on the main thread");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.intent";
           }),
           "a current asynchronous proposal commits through the normal intent path");
    expect(std::any_of(service.state().npcs.begin(), service.state().npcs.end(), [](const auto& entry) {
               return entry.second.activity.status == ActivityStatus::Active ||
                      entry.second.activity.status == ActivityStatus::Completed;
           }),
           "an accepted autonomous proposal creates a persistent NPC activity");
}

void independent_autonomous_turns_survive_unrelated_commits() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    content.npcDeliberation.cooldownMinutes = 360;
    auto service = WorldService::fromContent(
        content, std::make_unique<DelayedProvider>(std::chrono::milliseconds(20)));

    service.advanceMinutes(60);
    expect(service.pendingAsyncTurns() > 0,
           "one autonomous boundary leaves independent NPC decisions queued or ready");
    service.waitForAsyncTurns(1000);
    const auto committed = std::count_if(
        service.state().events.begin(), service.state().events.end(), [](const auto& event) {
            return event.type == "npc.intent";
        });
    expect(committed >= 2,
           "unrelated NPC decisions can commit after another actor advances world revision");
    expect(std::none_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.intent_expired";
           }),
           "independent autonomous decisions are not misclassified as stale");
}

void async_llm_action_validation_retries_without_blocking_world_tick() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    auto provider = std::make_unique<RepairingLlamaProvider>();
    auto* raw = provider.get();
    auto service = WorldService::fromContent(content, std::move(provider));
    const auto before = service.state().now.minute;
    service.advanceMinutes(60);
    expect(service.state().now.minute == before + 60,
           "the asynchronous repair path never holds the world tick for a model call");
    for (int attempt = 0; attempt < 20 && service.pendingAsyncTurns() > 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        service.pumpAsyncTurns();
    }
    expect(raw->calls >= 2 && raw->sawFeedback,
           "an asynchronous LLM action receives one structured validator repair request");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.intent";
           }),
           "the repaired asynchronous proposal commits through the ordinary intent path");
}

void async_turn_queue_coalesces_pending_requests_per_npc_and_channel() {
    BlockingProvider provider;
    TurnQueue queue(provider);
    const auto makeRequest = [](std::string turnId) {
        TurnRequest request;
        request.turnId = std::move(turnId);
        request.npcId = "mira";
        request.channelId = "autonomous_reappraisal";
        request.coalescePending = true;
        return request;
    };
    const auto first = queue.submit(makeRequest("first"));
    expect(first.accepted && first.supersededTurnIds.empty(), "the first queue request is accepted");
    provider.waitUntilStarted();
    const auto second = queue.submit(makeRequest("second"));
    const auto third = queue.submit(makeRequest("third"));
    expect(second.accepted && second.supersededTurnIds.empty() &&
               third.accepted && third.supersededTurnIds == std::vector<std::string>{"second"},
           "only the latest pending autonomous context for an NPC is retained");
    provider.release();
    std::vector<TurnResult> results;
    for (int attempt = 0; attempt < 100 && results.size() < 2; ++attempt) {
        auto ready = queue.drain();
        results.insert(results.end(), std::make_move_iterator(ready.begin()),
                       std::make_move_iterator(ready.end()));
        if (results.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(results.size() == 2 && results.front().request.turnId == "first" &&
               results.back().request.turnId == "third" && provider.calls.load() == 2,
           "the in-flight request completes and only the newest coalesced request is executed");
}

void async_turn_queue_rejects_decisions_past_game_deadline() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    content.npcDeliberation.maxDecisionAgeMinutes = 1;
    auto service = WorldService::fromContent(
        content, std::make_unique<DelayedProvider>(std::chrono::milliseconds(250)));
    service.advanceMinutes(60);
    expect(service.pendingAsyncTurns() > 0, "the slow autonomous provider is queued");
    service.advanceMinutes(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    service.advanceMinutes(1);
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.intent_expired" &&
                      event.data.value("error", "") == "decision_expired" &&
                      event.data.value("current_minute", 0LL) > event.data.value("deadline", 0LL);
           }),
           "a result that misses its game-time deadline is rejected at the main-thread commit boundary");
}

void time_step_partitioning_preserves_deterministic_world_state() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.enabled = false;
    auto singleJump = WorldService::fromContent(content, std::make_unique<MockProvider>());
    auto partitioned = WorldService::fromContent(content, std::make_unique<MockProvider>());
    const auto initialRevision = singleJump.state().revision;
    singleJump.advanceMinutes(kMinutesPerDay);
    for (int hour = 0; hour < 24; ++hour) partitioned.advanceMinutes(kMinutesPerHour);
    expect(singleJump.snapshotJson() == partitioned.snapshotJson(),
           "without external intervention, one large advance and hourly partitions commit identical facts");
    expect(singleJump.state().revision == initialRevision + static_cast<std::uint64_t>(kMinutesPerDay),
           "each committed simulation minute advances the world revision exactly once");
}

void high_salience_stimulus_interrupts_and_releases_reservations() {
    int providerCalls = 0;
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    for (auto& rule : content.stimulusRules) {
        if (rule.eventPrefix == "promise.") {
            rule.intensity = 1.0;
            rule.urgency = 1.0;
            rule.ttlMinutes = 20;
        }
    }
    auto service = WorldService::fromContent(content, std::make_unique<CountingProvider>(providerCalls));
    expect(service.converse("mira", "offer a ration", "interrupt-start").accepted,
           "a persistent process starts before interruption testing");
    service.advanceMinutes(10);
    expect(service.state().resources.at("grain").reserved == 5,
           "running process owns its input reservation");
    expect(service.createPromise("player", "mira", "bring the missing ledger", 1, "interrupt-stimulus").accepted,
           "a new social obligation enters the world");
    service.advanceMinutes(1);
    const auto& suspended = service.state().npcs.at("mira").activity;
    expect(suspended.status == ActivityStatus::Suspended && suspended.process &&
               suspended.process->status == ProcessStatus::Suspended,
           "a high-salience committed stimulus suspends the interruptible activity");
    expect(service.state().resources.at("grain").reserved == 0,
           "suspension releases reservations in the same transaction");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.process_suspended";
           }),
           "suspension is recorded as a world event");
    expect(service.state().npcs.at("mira").mode == "interrupted",
           "FSM projects the suspended activity as interrupted");
    service.advanceMinutes(30);
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "npc.process_resumed";
           }) || service.state().npcs.at("mira").activity.status == ActivityStatus::Active,
           "the suspended activity resumes after the interruption clears");
    expect(providerCalls == 1, "interruption and resume do not re-query the model inside the tick loop");
}

void behavior_tree_supports_sequence_timeout_and_interrupt() {
    int actions = 0;
    std::vector<std::unique_ptr<BehaviorNode>> sequenceChildren;
    sequenceChildren.push_back(std::make_unique<Condition>("safe"));
    sequenceChildren.push_back(std::make_unique<Action>("work"));
    auto sequence = std::make_unique<Sequence>(std::move(sequenceChildren));
    TickContext context;
    context.condition = [](const std::string& name) { return name == "safe"; };
    context.action = [&actions](const std::string&) { ++actions; return NodeStatus::Success; };
    expect(sequence->tick(context) == NodeStatus::Success, "sequence completes");
    expect(actions == 1, "sequence runs action once");
    auto interruptible = std::make_unique<Interruptible>(std::make_unique<Action>("walk"));
    context.interrupted = true;
    expect(interruptible->tick(context) == NodeStatus::Failure, "interrupt stops action");
    auto timeout = std::make_unique<Timeout>(std::make_unique<Action>("wait"), 1);
    context.interrupted = false;
    context.action = [](const std::string&) { return NodeStatus::Running; };
    expect(timeout->tick(context) == NodeStatus::Running, "timeout allows first running tick");
    expect(timeout->tick(context) == NodeStatus::Failure, "timeout fails after deadline");
}

void provider_and_action_validation_are_safe() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto context = service.decisionContext("mira");
    expect(!context.internalDrives.empty() && !context.externalStimuli.empty() &&
               !context.structuralConstraints.empty(),
           "decision context separates drives, stimuli, and structural constraints");
    const auto turn = service.converse("mira", "Can you spare a ration?", "dialogue-1");
    expect(turn.accepted, "mock provider returns a valid candidate action");
    const auto activeContext = service.decisionContext("mira");
    expect(activeContext.currentActivity.at("action_id") == "offer_ration" &&
               activeContext.currentActivity.contains("process_id"),
           "decision context exposes the same active activity the player can ask about");
    service.advanceMinutes(30);
    expect(service.state().resources.at("grain").amount == 95,
           "validated ration action changes grain atomically");
    const auto duplicate = service.converse("mira", "again", "dialogue-1");
    expect(!duplicate.accepted && duplicate.error == "duplicate_turn", "duplicate turn is idempotent");
    const auto unknown = service.submit(Command{"bad-command", "unknown", "mira", {}, {}, {}});
    expect(!unknown.accepted && unknown.code == "unknown_command", "unknown command is rejected");
    const auto stale = service.submit(Command{"stale", "advance", "mira", {}, {}, "1", 1, 1});
    expect(!stale.accepted && stale.code == "state_conflict", "stale world revisions are rejected atomically");

    const std::vector<std::string> contentRoots{
        ContentPackLoader::defaultRoot(),
        TIDE_MARKS_TEST_DATA_DIR "/minimal-pack",
        TIDE_MARKS_TEST_DATA_DIR "/port-pack",
        TIDE_MARKS_TEST_DATA_DIR "/glass-orchard-pack"};
    for (const auto& root : contentRoots) {
        const auto content = ContentPackLoader::load(root);
        const auto rule = std::find_if(
            content.conversationPolicy.rules.begin(), content.conversationPolicy.rules.end(),
            [](const auto& item) { return item.act == SpeechAct::InformationRequest; });
        expect(rule != content.conversationPolicy.rules.end() &&
                   !rule->description.empty() && !rule->worldActionAllowed,
               "all content packs define semantic information requests as conversation-only");
    }

    auto semantic = WorldService::load(
        ContentPackLoader::defaultRoot(),
        std::make_unique<SpeechActProvider>("information_request", ""));
    const int semanticGrainBefore = semantic.state().resources.at("grain").amount;
    const auto semanticInfo = semantic.converse(
        "mira", "Please offer a ration.", "semantic-information");
    expect(semanticInfo.accepted && semanticInfo.event.has_value() &&
               semanticInfo.actionId.empty() && semanticInfo.event->data.at("action_id") == "" &&
               semanticInfo.event->data.at("speech_act") == "information_request" &&
               semantic.state().resources.at("grain").amount == semanticGrainBefore,
           "the model-selected information_request stays conversational without text matching");

    auto overreach = WorldService::load(
        ContentPackLoader::defaultRoot(),
        std::make_unique<SpeechActProvider>("inquiry", "offer_ration"));
    const auto rejectedAction = overreach.converse(
        "mira", "Please offer a ration.", "semantic-overreach");
    expect(!rejectedAction.accepted && rejectedAction.error == "speech_act_disallows_world_action",
           "Runtime rejects a world action when the model-selected speech act does not permit it");

    auto emptyDialogue = WorldService::load(
        ContentPackLoader::defaultRoot(), std::make_unique<EmptyDialogueProvider>());
    const auto eventsBeforeEmptyDialogue = emptyDialogue.state().events.size();
    const auto emptyTurn = emptyDialogue.converse("mira", "What are you doing?", "empty-dialogue");
    expect(!emptyTurn.accepted && emptyTurn.error == "missing_dialogue" &&
               emptyDialogue.state().events.size() == eventsBeforeEmptyDialogue,
           "Runtime rejects an empty player-facing dialogue instead of committing a blank response");

    auto failingProvider = std::make_unique<FailingProvider>();
    auto* failingRaw = failingProvider.get();
    auto unavailable = WorldService::load(ContentPackLoader::defaultRoot(), std::move(failingProvider));
    const int grainBeforeFailure = unavailable.state().resources.at("grain").amount;
    const auto eventsBeforeFailure = unavailable.state().events.size();
    const auto failedTurn = unavailable.converse(
        "mira", "Please offer a ration.", "provider-down");
    expect(!failedTurn.accepted && failedTurn.error == "provider_unavailable" &&
               unavailable.state().resources.at("grain").amount == grainBeforeFailure &&
               unavailable.state().events.size() == eventsBeforeFailure,
           "a failed conversation provider does not infer intent or commit facts");
    expect(failingRaw->observedContext.speechActClassificationRequired &&
               failingRaw->observedContext.speechAct.empty() &&
               !failingRaw->observedContext.speechActs.empty() &&
               !failingRaw->observedContext.speechActs.front().description.empty(),
           "Runtime passes the semantic vocabulary to the provider without preclassifying player text");
}

void decision_context_projects_location_relevant_resources() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto mira = service.decisionContext("mira");
    const auto lorn = service.decisionContext("lorn");
    expect(mira.runtimeMetadata.at("world_id") == service.state().worldId &&
               mira.runtimeMetadata.at("instance_id") == service.state().instanceId &&
               mira.runtimeMetadata.at("content_version") == service.content().contentVersion &&
               mira.runtimeMetadata.at("simulation_version") == kSimulationVersion &&
               mira.runtimeMetadata.at("world_revision") == mira.worldRevision &&
               mira.runtimeMetadata.at("npc_state_version") == mira.npcStateVersion,
           "decision requests carry auditable runtime identity separately from actor-visible facts");
    expect(!mira.worldSnapshot.contains("world_id") && !mira.worldSnapshot.contains("instance_id") &&
               !mira.worldSnapshot.contains("content_version"),
           "engineering identity is not mixed into the NPC world-fact projection");
    expect(mira.worldSnapshot["public_resources"].contains("grain") &&
               !mira.worldSnapshot["public_resources"].contains("boat_integrity") &&
               lorn.worldSnapshot["public_resources"].contains("boat_integrity") &&
               !lorn.worldSnapshot["public_resources"].contains("grain"),
           "LLM world snapshots project resources at the NPC location instead of dumping every location");
    expect(mira.worldSnapshot["public_resources"].contains("cooperation") &&
               mira.worldSnapshot["public_resources"].at("grain").at("location") == "granary" &&
               mira.perception.find("boat_integrity") == std::string::npos &&
               lorn.perception.find("grain") == std::string::npos &&
               mira.structuralConstraints.find("boat_integrity") == std::string::npos &&
               lorn.structuralConstraints.find("grain") == std::string::npos,
           "locationless collective resources remain visible and resource provenance stays explicit");
}

void decision_context_projects_only_startable_behavior_plans() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    auto snapshot = service.snapshotJson();
    snapshot["resources"]["grain"]["amount"] = 0;
    snapshot["resources"]["grain"]["reserved"] = 0;
    auto blocked = WorldService::fromSnapshot(snapshot);
    const auto mira = blocked.decisionContext("mira");
    const auto ration = std::find_if(mira.candidates.begin(), mira.candidates.end(),
                                     [](const auto& candidate) { return candidate.actionId == "offer_ration"; });
    expect(ration != mira.candidates.end() && !ration->availableNow &&
               ration->unavailableReason == "insufficient_resource" &&
               std::none_of(mira.planCatalog.begin(), mira.planCatalog.end(), [](const auto& plan) {
                   return plan.value("action_id", "") == "offer_ration";
               }),
           "a plan whose declared reservation cannot start is retained for Runtime diagnostics but hidden from the model");

    auto ferrySnapshot = service.snapshotJson();
    ferrySnapshot["resources"]["boat_integrity"]["amount"] = 100;
    auto ferryBlocked = WorldService::fromSnapshot(ferrySnapshot);
    const auto lorn = ferryBlocked.decisionContext("lorn");
    const auto repair = std::find_if(lorn.candidates.begin(), lorn.candidates.end(),
                                     [](const auto& candidate) { return candidate.actionId == "repair_ferry"; });
    expect(repair != lorn.candidates.end() && !repair->availableNow &&
               repair->unavailableReason == "precondition_failed" &&
               std::none_of(lorn.planCatalog.begin(), lorn.planCatalog.end(), [](const auto& plan) {
                   return plan.value("action_id", "") == "repair_ferry";
               }),
           "a behavior tree whose first condition is false is omitted from the provider decision catalog");
}

void decision_context_obeys_authored_visibility_budgets() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.decisionContextPolicy.maxVisibleEntities = 1;
    content.decisionContextPolicy.maxVisibleLinks = 1;
    content.decisionContextPolicy.maxRelationships = 1;
    auto observer = content.npcs.front();
    observer.definition.id = "observer";
    observer.definition.name = "Observer";
    observer.initialLocation = content.npcs.front().initialLocation;
    observer.initialScheduleLabel = observer.initialLocation;
    content.npcs.push_back(std::move(observer));
    auto service = WorldService::fromContent(content, std::make_unique<MockProvider>());
    const auto context = service.decisionContext(content.npcs.front().definition.id);
    expect(context.worldSnapshot.at("visible_entities").size() == 1,
           "the subject-specific context budget limits visible co-located entities");
    expect(context.worldSnapshot.at("visible_links").size() == 1,
           "the subject-specific context budget limits adjacent topology");
    expect(context.selfState.at("relationships").size() <= 1,
           "relationship projection obeys the same authored context budget");
}

void action_validator_rejects_malformed_nested_schema_without_throwing() {
    DecisionContext context;
    ActionCandidate candidate;
    candidate.actionId = "fixture_action";
    candidate.parameterSchema = Json::object();
    candidate.parameterSchema["type"] = "object";
    candidate.parameterSchema["properties"]["destinations"] =
        Json{{"type", "array"}, {"items", "not-an-object"}};
    context.candidates.push_back(candidate);
    const auto validation = ActionValidator::validate(
        context, "fixture_action", Json{{"destinations", Json::array({"archive"})}});
    expect(!validation.valid && validation.code == "invalid_action_parameter_schema",
           "malformed nested parameter schemas fail closed with structured validation feedback");
}

void action_validator_enforces_unique_array_parameters() {
    DecisionContext context;
    ActionCandidate candidate;
    candidate.actionId = "fixture_action";
    candidate.parameterSchema = Json{
        {"type", "object"},
        {"additionalProperties", false},
        {"required", Json::array({"destinations"})},
        {"properties", Json{{"destinations", Json{{"type", "array"}, {"minItems", 1},
                                                       {"uniqueItems", true},
                                                       {"items", Json{{"type", "string"}}}}}}}};
    context.candidates.push_back(candidate);
    const auto accepted = ActionValidator::validate(
        context, "fixture_action", Json{{"destinations", Json::array({"archive", "dock"})}});
    const auto rejected = ActionValidator::validate(
        context, "fixture_action", Json{{"destinations", Json::array({"archive", "archive"})}});
    expect(accepted.valid && !rejected.valid && rejected.code == "invalid_action_parameter_value",
           "uniqueItems and minItems are enforced for configured array action parameters");
}

void action_validator_enforces_recursive_schema_constraints() {
    DecisionContext context;
    ActionCandidate candidate;
    candidate.actionId = "recursive_action";
    candidate.parameterSchema = Json{
        {"type", "object"},
        {"required", Json::array({"route"})},
        {"properties", Json{{"route", Json{{"type", "object"}, {"additionalProperties", false},
                                               {"required", Json::array({"kind", "stops"})},
                                               {"properties", Json{
                                                   {"kind", Json{{"const", "ferry"}}},
                                                   {"stops", Json{{"type", "array"}, {"uniqueItems", true},
                                                                    {"items", Json{{"type", "string"}, {"minLength", 2}}}}}}}}}}}};
    context.candidates.push_back(candidate);
    const auto accepted = ActionValidator::validate(
        context, "recursive_action",
        Json{{"route", Json{{"kind", "ferry"}, {"stops", Json::array({"dock", "cove"})}}}});
    const auto rejected = ActionValidator::validate(
        context, "recursive_action",
        Json{{"route", Json{{"kind", "road"}, {"stops", Json::array({"dock", "dock"})}}}});
    expect(accepted.valid && !rejected.valid && rejected.code == "invalid_action_parameter_value",
           "nested object, const, item constraints, and uniqueItems are enforced recursively");
}

void llm_action_validation_returns_bounded_repair_feedback() {
    auto provider = std::make_unique<RepairingLlamaProvider>();
    auto* raw = provider.get();
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::move(provider));
    const auto turn = service.deliberate("mira", "test_invalid_parameters", "llm-repair");
    expect(turn.accepted && !turn.actionId.empty() && raw->calls == 2 && raw->sawFeedback,
           "a real LLM action validation error is returned once for a bounded repair");
    expect(service.state().events.back().data.at("action_id") == turn.actionId,
           "the repaired proposal is the only action committed to the event log");
}

void llm_expected_relief_validation_returns_bounded_repair_feedback() {
    auto provider = std::make_unique<RepairingExpectedReliefProvider>();
    auto* raw = provider.get();
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::move(provider));
    const auto turn = service.deliberate("mira", "test_invalid_expected_relief", "llm-relief-repair");
    expect(turn.accepted && !turn.actionId.empty() && raw->calls == 2 && raw->sawFeedback,
           "an invalid expected-relief key receives one bounded structured repair");
}

void meta_actions_are_loaded_from_content() {
    const auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    expect(content.metaActions.contains("consume_resource"),
           "the content pack defines its own resource-consumption action");
    expect(content.metaActions.at("consume_resource").operation == "resource.consume",
           "a content action binds to a generic Runtime operator");
    expect(content.metaActions.contains("relieve_need") &&
               content.metaActions.at("relieve_need").operation == "need.relieve",
           "need relief is configured without a C++ story-action branch");
    expect(content.metaActions.contains("set_link_state") &&
               content.metaActions.at("set_link_state").operation == "link.set_state",
           "structural transitions are configured by action id");
    expect(!content.metaActions.contains("transfer_resource"),
           "an undeclared action is absent rather than supplied by C++");
    expect(MetaActionRegistry::executableOperators().contains("trade.execute") &&
               MetaActionRegistry::executableOperators().contains("exchange.create") &&
               MetaActionRegistry::executableOperators().contains("exchange.transition") &&
               MetaActionRegistry::executableOperators().contains("exchange.settle") &&
               MetaActionRegistry::executableOperators().contains("group.metric_adjust") &&
               MetaActionRegistry::executableOperators().contains("group.membership_adjust"),
           "social and exchange operators are registered execution capabilities");
    const auto repair = std::find_if(content.behaviorRecipes.begin(), content.behaviorRecipes.end(),
                                     [](const auto& recipe) { return recipe.id == "repair_ferry"; });
    const auto ration = std::find_if(content.behaviorRecipes.begin(), content.behaviorRecipes.end(),
                                     [](const auto& recipe) { return recipe.id == "offer_ration"; });
    const auto rest = std::find_if(content.behaviorRecipes.begin(), content.behaviorRecipes.end(),
                                   [](const auto& recipe) { return recipe.id == "rest"; });
    expect(repair != content.behaviorRecipes.end() && repair->reservationMode == "input_resources",
           "a resource-consuming process opts into input reservation through content");
    expect(repair != content.behaviorRecipes.end() && repair->priority == 20 &&
               ration != content.behaviorRecipes.end() && ration->priority == 10 &&
               ration->preemptionPolicy == "lower_priority",
           "arbitration priority and preemption policy load from the behavior pack");
    expect(rest != content.behaviorRecipes.end() && rest->reservationMode == "none",
           "a long process without resource inputs does not reserve merely because it has duration");
}

void needs_cover_domains_and_explicit_execution_paths() {
    const auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    std::set<std::string> domains;
    for (const auto& npc : content.npcs) {
        for (const auto& need : npc.needs) {
            domains.insert(need.domain);
            expect(!need.satisfiers.empty(), "every authored need has an executable satisfier");
            for (const auto& [actionId, satisfier] : need.satisfiers) {
                (void)actionId;
                expect(!satisfier.metaActions.empty(), "every satisfier names its meta actions");
                expect(std::find(satisfier.metaActions.begin(), satisfier.metaActions.end(), "relieve_need") !=
                           satisfier.metaActions.end(),
                       "every satisfier commits need relief explicitly");
            }
        }
    }
    expect(domains == std::set<std::string>{"cognitive", "physiological", "psychological", "social"},
           "village content covers physiological, psychological, social and cognitive drives");
}

void bounded_drive_expectation_adapts_from_outcomes() {
    NpcState npc;
    NeedContent need;
    need.id = "food_security";
    need.domain = "physiological";
    need.dimension = "food";
    need.weight = 1.0;
    need.criticalThreshold = 0.8;
    need.satisfiers["eat"] = NeedSatisfier{0.5, {"relieve_need"}};
    NeedState state;
    state.deficit = 0.2;
    state.sensitivity = 1.0;
    state.expectedRelief["eat"] = 0.5;
    npc.needs["food_security"] = state;
    DriveAdaptationPolicy policy;
    policy.enabled = true;
    policy.expectationLearningRate = 0.25;
    policy.maxExpectationStep = 0.05;
    policy.cooldownMinutes = 0;
    const auto changes = DriveSystem::relieve(npc, {need}, "eat", GameTime{}, policy);
    expect(changes.size() == 1 && changes.front().adapted,
           "prediction error produces a bounded drive adaptation");
    expect(npc.needs.at("food_security").expectedRelief.at("eat") < 0.5,
           "learned expected relief moves toward the observed outcome");
    expect(npc.needs.at("food_security").expectedRelief.at("eat") >= 0.45,
           "one adaptation step remains within the authored bound");
    expect(npc.needs.at("food_security").adaptationVersion == 1,
           "drive adaptation carries a version for audit and rollback");
}

void external_stimuli_and_structures_are_observable_and_persistent() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    service.advanceMinutes(14 * kMinutesPerHour);
    expect(!service.state().links.at("ferry_square").traversable,
           "authored structural activity automatically closes the ferry at night");
    const auto context = service.decisionContext("mira");
    expect(!context.drives.empty() && !context.stimuli.empty() && !context.constraints.empty(),
           "decision context exposes drives, external stimuli and structural constraints");
    expect(std::any_of(context.constraints.begin(), context.constraints.end(), [](const auto& constraint) {
               return constraint.id == "link:ferry_square:blocked";
           }),
           "a closed authored link appears as a structured constraint");
    expect(!service.state().npcs.at("mira").stimuli.empty(),
           "committed events project into character stimuli");
    service.advanceMinutes(10 * kMinutesPerHour);
    expect(service.state().links.at("ferry_square").traversable,
           "the next authored transition reopens the ferry");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "stimulus.habituated";
           }),
           "repeated stimuli produce bounded habituation evidence");
    auto restored = WorldService::fromSnapshot(service.snapshotJson());
    expect(restored.state().npcs.at("mira").stimuli.size() == service.state().npcs.at("mira").stimuli.size(),
           "external stimulus history survives snapshot restore");
    expect(restored.state().npcs.at("mira").stimulusSensitivity ==
               service.state().npcs.at("mira").stimulusSensitivity,
           "stimulus sensitivity survives snapshot restore");
}

void promise_policy_comes_from_content() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.promisePolicy.acceptedActor.trust = 0.5;
    content.promisePolicy.renegotiationExtensionMinutes = 60;
    auto service = WorldService::fromContent(content, std::make_unique<MockProvider>());
    const double before = service.state().npcs.at("mira").relationships.at("player").trust;
    const auto made = service.createPromise("player", "mira", "bring a ledger", 1, "policy-create");
    expect(made.accepted, "policy test creates a promise");
    expect(service.decidePromise(made.message, PromiseStatus::Accepted, "mira", "policy-accept").accepted,
           "policy test accepts the promise");
    const double after = service.state().npcs.at("mira").relationships.at("player").trust;
    expect(after > before + 0.49 && after < before + 0.51,
           "relationship change follows the authored promise policy");
    const auto dueBefore = service.state().promises.at(made.message).due.minute;
    expect(service.decidePromise(made.message, PromiseStatus::Renegotiated, "player", "policy-renegotiate").accepted,
           "policy test renegotiates the promise");
    expect(service.state().promises.at(made.message).due.minute == dueBefore + 60,
           "renegotiation extension follows the authored policy");
}

void promises_cover_accept_renegotiate_refuse_and_break() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto made = service.createPromise("mira", "lorn", "repair the east ferry", 1, "promise-create");
    expect(made.accepted, "promise can be created");
    const auto id = made.message;
    expect(service.decidePromise(id, PromiseStatus::Accepted, "lorn", "promise-accept").accepted,
           "promise can be accepted");
    expect(service.decidePromise(id, PromiseStatus::Renegotiated, "lorn", "promise-renegotiate").accepted,
           "promise can be renegotiated");
    expect(service.state().promises.at(id).status == PromiseStatus::Renegotiated, "renegotiation extends deadline");
    const auto refused = service.createPromise("mira", "eve", "share the registry", 1, "promise-two");
    expect(service.decidePromise(refused.message, PromiseStatus::Refused, "eve", "promise-refuse").accepted,
           "promise can be refused");
    service.advanceMinutes(3 * kMinutesPerDay);
    expect(service.state().promises.at(id).status == PromiseStatus::Broken, "accepted promise can break at deadline");

    const auto playerPromise = service.createPromise("player", "mira", "bring a new ledger", 1, "player-promise");
    expect(playerPromise.accepted, "player can make a promise to an NPC");
    expect(service.decidePromise(playerPromise.message, PromiseStatus::Accepted, "mira", "player-accept").accepted,
           "NPC can accept player promise");
    expect(service.decidePromise(playerPromise.message, PromiseStatus::Renegotiated, "player", "player-renegotiate").accepted,
           "player can renegotiate the same promise");
    expect(service.decidePromise(playerPromise.message, PromiseStatus::Refused, "player", "player-refuse").accepted,
           "player can refuse a proposed or renegotiated promise");
}

void snapshot_and_replay_are_deterministic() {
    auto service = WorldService::load(
        ContentPackLoader::defaultRoot(),
        std::make_unique<SpeechActProvider>("information_request", ""));
    service.converse("eve", "Tell me about the record", "replay-dialogue");
    service.advanceMinutes(2 * kMinutesPerDay);
    const auto path = std::filesystem::temp_directory_path() / "tide_marks_test_replay.jsonl";
    const auto snapshotPath = std::filesystem::temp_directory_path() / "tide_marks_test_snapshot.json";
    const auto durablePath = std::filesystem::temp_directory_path() / "tide_marks_test_durable.json";
    service.writeReplay(path.string());
    service.saveSnapshot(snapshotPath.string());
    service.saveSnapshot(snapshotPath.string());
    service.saveDurableCommit(durablePath.string());
    expect(std::filesystem::exists(path) && std::filesystem::exists(snapshotPath) &&
               !std::filesystem::exists(path.string() + ".tmp") &&
               !std::filesystem::exists(snapshotPath.string() + ".tmp"),
           "snapshot and replay writes replace complete files without leaving staging files behind");
    std::vector<Json> replayLines;
    {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) replayLines.push_back(Json::parse(line));
    }
    expect(replayLines.size() == 2 && replayLines.at(0).at("kind") == "replay_header" &&
               replayLines.at(0).at("schema_version") == 2 &&
               replayLines.at(0).at("transaction_count") == 1 &&
               replayLines.at(1).at("kind") == "transaction" &&
               replayLines.at(1).at("patch").is_array() &&
               replayLines.at(1).at("events").is_array() &&
               !replayLines.at(1).at("events").empty(),
           "replay writes one baseline and one aggregate commit frame instead of one record per tick");
    const auto snapshot = service.snapshotJson();
    expect(snapshot.at("save_schema_version").get<int>() == 19 &&
               snapshot.at("world_revision").is_number_unsigned(),
           "snapshot declares its schema and explicit world revision");
    expect(snapshot.at("instance_id") == "tide_marks.village.instance",
           "snapshot records the concrete game instance identity");
    expect(snapshot.at("event_sequence").get<std::size_t>() == service.state().events.size(),
           "snapshot records its event-log cutoff");
    const auto& savedNpc = snapshot.at("npcs").at("mira");
    expect(savedNpc.contains("definition_id") && !savedNpc.contains("definition"),
           "snapshot stores a content reference instead of a definition copy");
    auto cursorSnapshot = service.snapshotJson();
    cursorSnapshot["npcs"]["mira"]["reflection_cursor"] =
        Json{{"last_event_sequence", service.state().events.size()},
             {"last_reflection_day", service.state().now.day()},
             {"reflection_revision", service.state().revision}};
    cursorSnapshot["npcs"]["mira"]["innovation_adoptions"] = Json::array({
        Json{{"proposal_id", "innovation.test"}, {"recipe_id", "private_recipe"},
             {"status", "resource_blocked"}, {"started_at", service.state().now.minute},
             {"updated_at", service.state().now.minute}, {"reason", "waiting for timber"}}});
    auto restoredCursor = WorldService::fromSnapshot(cursorSnapshot);
    expect(restoredCursor.state().npcs.at("mira").reflectionCursor.lastEventSequence ==
                   service.state().events.size() &&
               restoredCursor.state().npcs.at("mira").reflectionCursor.lastReflectionDay ==
                   service.state().now.day() &&
               restoredCursor.state().npcs.at("mira").innovationAdoptions.size() == 1 &&
               restoredCursor.state().npcs.at("mira").innovationAdoptions.front().status ==
                   InnovationAdoptionStatus::ResourceBlocked,
           "daily reflection cursors and adoption state survive snapshot restore at an explicit event boundary");
    expect(restoredCursor.transitionInnovationAdoption("mira", "innovation.test",
                                                       InnovationAdoptionStatus::Executing,
                                                       "timber arrived").accepted &&
               restoredCursor.transitionInnovationAdoption("mira", "innovation.test",
                                                           InnovationAdoptionStatus::Completed,
                                                           "recipe finished").accepted,
           "resource-blocked innovation adoption can resume and complete through governed transitions");
    expect(!restoredCursor.transitionInnovationAdoption("mira", "innovation.test",
                                                        InnovationAdoptionStatus::Executing,
                                                        "retry").accepted,
           "completed innovation adoption cannot be silently reopened");
    auto brokenSnapshot = service.snapshotJson();
    brokenSnapshot["npcs"]["mira"]["definition_id"] = "missing_definition";
    bool rejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(brokenSnapshot));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "a snapshot that references removed content is rejected");
    auto wrongVersion = service.snapshotJson();
    wrongVersion["content_version"] = "other-content-version";
    bool versionRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(wrongVersion));
    } catch (const std::runtime_error&) {
        versionRejected = true;
    }
    expect(versionRejected, "a snapshot from a different content version is rejected");
    auto wrongInstance = service.snapshotJson();
    wrongInstance["instance_id"] = "another.instance";
    bool instanceRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(wrongInstance));
    } catch (const std::runtime_error&) {
        instanceRejected = true;
    }
    expect(instanceRejected, "a snapshot from a different game instance is rejected");
    auto wrongWorld = service.snapshotJson();
    wrongWorld["world_id"] = "another.world";
    bool worldRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(wrongWorld));
    } catch (const std::runtime_error&) {
        worldRejected = true;
    }
    expect(worldRejected, "a snapshot from a different world is rejected");
    auto oldSchema = service.snapshotJson();
    oldSchema["save_schema_version"] = 9;
    bool oldSchemaRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(oldSchema));
    } catch (const std::runtime_error&) {
        oldSchemaRejected = true;
    }
    expect(oldSchemaRejected, "an obsolete snapshot schema is rejected instead of guessed");
    const auto replayed = WorldService::replay(path.string());
    expect(service.snapshotJson() == replayed.snapshotJson(),
           "replay applies the commit patch and event range to reconstruct the exact final state");
    const auto durableReplayed = WorldService::replay(durablePath.string());
    expect(service.snapshotJson() == durableReplayed.snapshotJson(),
           "replay also accepts the explicit durable checkpoint artifact");
    replayLines.at(1)["state_digest"] = "fnv1a64:tampered";
    const auto tamperedPath = std::filesystem::temp_directory_path() / "tide_marks_test_replay_tampered.jsonl";
    {
        std::ofstream output(tamperedPath, std::ios::trunc);
        for (const auto& line : replayLines) output << line.dump() << '\n';
    }
    bool tamperedReplayRejected = false;
    try {
        static_cast<void>(WorldService::replay(tamperedPath.string()));
    } catch (const std::runtime_error&) {
        tamperedReplayRejected = true;
    }
    expect(tamperedReplayRejected, "replay rejects a commit frame whose reconstructed state digest changed");
    std::filesystem::remove(path);
    std::filesystem::remove(durablePath);
    std::filesystem::remove(tamperedPath);
    std::filesystem::remove(snapshotPath);
    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
    expect(summarize(service.state()) == summarize(restored.state()), "snapshot restores identical state");
}

void persistence_adapter_receives_committed_representations() {
    auto persistence = std::make_unique<RecordingPersistence>();
    auto* persistenceRaw = persistence.get();
    auto service = WorldService(std::make_unique<MockProvider>(), std::move(persistence));
    service.saveSnapshot("memory://snapshot");
    service.writeReplay("memory://replay");
    service.saveDurableCommit("memory://commit");
    expect(persistenceRaw->snapshotPath == "memory://snapshot" &&
               persistenceRaw->snapshotValue.at("world_revision") == service.state().revision,
           "WorldService sends a committed Snapshot through the persistence boundary");
    expect(persistenceRaw->replayPath == "memory://replay" && persistenceRaw->replayLines.size() == 1 &&
               persistenceRaw->replayLines.front().at("kind") == "replay_header",
           "WorldService sends Replay lines through the same injectable persistence boundary");
    expect(persistenceRaw->durableCommitPath == "memory://commit" &&
               persistenceRaw->durableCommitSnapshot.at("world_revision") == service.state().revision &&
               persistenceRaw->durableCommitReplay.size() == 1,
           "WorldService can export a committed Snapshot and Replay as one durable checkpoint");
}

void json_file_persistence_creates_run_directories() {
    const auto root = std::filesystem::temp_directory_path() / "tide_marks_missing_run_parent";
    std::filesystem::remove_all(root);
    const auto snapshotPath = root / "instance-a" / "run-1" / "snapshot.json";
    const auto durablePath = root / "instance-a" / "run-1" / "checkpoint.json";
    JsonFilePersistence persistence;
    const Json snapshot{{"world_revision", 2}, {"event_sequence", 1}, {"events", Json::array()}};
    persistence.saveSnapshot(snapshotPath.string(), snapshot);
    expect(std::filesystem::exists(snapshotPath),
           "JSON persistence creates an external run directory before writing a snapshot");
    std::ifstream input(snapshotPath);
    Json restored;
    input >> restored;
    expect(restored == snapshot, "JSON persistence preserves the snapshot payload in a created run directory");
    persistence.saveDurableCommit(durablePath.string(), snapshot, {Json{{"kind", "replay_header"}}});
    expect(std::filesystem::exists(durablePath),
           "JSON persistence creates the run directory for a durable checkpoint as well");
    std::filesystem::remove_all(root);
}

void durable_checkpoint_policy_is_boundary_driven_and_opt_in() {
    auto persistence = std::make_unique<RecordingPersistence>();
    auto* persistenceRaw = persistence.get();
    auto service = WorldService(std::make_unique<MockProvider>(), std::move(persistence));
    service.configureDurableCheckpoint(DurableCheckpointPolicy{
        DurableCheckpointMode::PeriodicMinutes, "memory://automatic", 60});
    service.advanceMinutes(30);
    expect(persistenceRaw->durableCommitCount == 0,
           "a periodic durable checkpoint does not write before its configured interval");
    service.advanceMinutes(30);
    expect(persistenceRaw->durableCommitCount == 1 &&
               persistenceRaw->durableCommitPath == "memory://automatic" &&
               persistenceRaw->durableCommitSnapshot.at("world_revision") == service.state().revision,
           "a periodic durable checkpoint writes after a committed scheduler boundary");
    service.configureDurableCheckpoint(DurableCheckpointPolicy{});
    service.advanceMinutes(60);
    expect(persistenceRaw->durableCommitCount == 1,
           "disabling the policy stops automatic durable writes while simulation continues");
}

void sqlite_persistence_roundtrips_versioned_artifacts() {
    const auto path = (std::filesystem::temp_directory_path() / "tide_marks-persistence-test.sqlite").string();
    std::filesystem::remove(path);
    SqlitePersistence persistence(path);
    const Json snapshot{{"save_schema_version", 19}, {"world_revision", 4}, {"content_version", "test-v1"}};
    const std::vector<Json> firstReplay{
        Json{{"kind", "replay_header"}, {"schema_version", 2}},
        Json{{"kind", "transaction"}, {"world_revision", 4}}};
    persistence.saveSnapshot("run/test/snapshot", snapshot);
    persistence.saveReplay("run/test/replay", firstReplay);
    expect(persistence.loadSnapshot("run/test/snapshot") == snapshot,
           "SQLite persistence round-trips a versioned snapshot payload");
    expect(persistence.loadReplay("run/test/replay") == firstReplay,
           "SQLite persistence round-trips ordered replay lines");
    const std::vector<Json> replacement{Json{{"kind", "replay_header"}, {"schema_version", 2}}};
    persistence.saveReplay("run/test/replay", replacement);
    expect(persistence.loadReplay("run/test/replay") == replacement,
           "SQLite replay replacement is atomic for one run path");
    const Json durableSnapshot{{"world_id", "test-world"}, {"instance_id", "test-instance"},
                               {"content_version", "test-v1"}, {"world_revision", 4},
                               {"event_sequence", 1}, {"processed_turn_ids", Json::array({"turn-1"})},
                               {"active_overlays", Json::array()},
                               {"events", Json::array({Json{{"sequence", 1}, {"type", "test.event"}}})},
                               {"resources", Json::object()}};
    persistence.saveDurableCommit("run/test/commit", durableSnapshot, replacement);
    const auto durable = persistence.loadDurableCommit("run/test/commit");
    expect(durable.at("world_revision") == 4 && durable.at("events").size() == 1 &&
               durable.at("processed_turn_ids").at(0) == "turn-1" &&
               durable.at("durable_replay") == replacement,
           "SQLite durable commit round-trips state, event history, idempotency and replay together");
    bool missingSnapshotRejected = false;
    try {
        static_cast<void>(persistence.loadSnapshot("run/missing/snapshot"));
    } catch (const std::runtime_error&) {
        missingSnapshotRejected = true;
    }
    expect(missingSnapshotRejected, "SQLite reports a missing snapshot without leaking or double-closing its handle");
    bool missingDurableCommitRejected = false;
    try {
        static_cast<void>(persistence.loadDurableCommit("run/missing/commit"));
    } catch (const std::runtime_error&) {
        missingDurableCommitRejected = true;
    }
    expect(missingDurableCommitRejected,
           "SQLite reports a missing durable checkpoint without leaking or double-closing its handle");
    std::filesystem::remove(path);
}

void sqlite_instance_imports_content_and_restores_without_source_root() {
    const auto databasePath = std::filesystem::temp_directory_path() / "tide_marks_instance_bundle.sqlite";
    std::error_code ignored;
    std::filesystem::remove(databasePath, ignored);
    auto created = WorldService::createSqliteInstance(databasePath.string(),
                                                      ContentPackLoader::defaultRoot(),
                                                      std::make_unique<MockProvider>());
    const auto imported = SqlitePersistence(databasePath.string()).loadContentDocuments("instance");
    expect(imported.contains("content-pack.json") && imported.contains("world/village.json") &&
               imported.contains("behaviors/village.json") &&
               imported.at("world/village.json").at("evolution_prompts").contains("npc_strategy") &&
               imported.at("world/village.json").at("evolution_prompts").contains("world_substrate"),
           "a new SQLite instance imports the complete immutable content document bundle");
    const auto initialSummary = summarize(created.state());
    auto restored = WorldService::loadSqliteInstance(databasePath.string(), std::make_unique<MockProvider>());
    expect(summarize(restored.state()) == initialSummary && restored.content().id == created.content().id,
           "an instance restores its content and committed facts from SQLite without a content-root argument");
    std::filesystem::remove(databasePath, ignored);
}

void event_history_has_causal_identity() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    service.converse("mira", "please offer a ration", "causal-ration");
    service.advanceMinutes(30);
    const auto& events = service.state().events;
    expect(!events.empty(), "causal test produced committed events");
    bool sawChain = false;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        expect(event.schemaVersion == 4, "runtime events use the current event schema");
        expect(event.eventId == "event:" + std::to_string(event.sequence),
               "every event has a stable event id");
        expect(event.sequence == index + 1, "event sequence remains contiguous");
        expect(!event.correlationId.empty(), "every event has a correlation id");
        expect(event.contentVersion == service.state().contentVersion,
               "every event records the content version active at commit time");
        expect(event.simulationVersion == kSimulationVersion,
               "every event records its simulation semantics version");
        expect(event.worldRevision > 0 && event.worldRevision <= service.state().revision,
               "every event points to a committed world revision");
        if (event.causationSequence == 0) continue;
        const auto previous = std::find_if(events.begin(), events.end(), [&event](const auto& candidate) {
            return candidate.sequence == event.causationSequence;
        });
        expect(previous != events.end(), "event causation points to an existing event");
        if (previous != events.end()) {
            expect(previous->correlationId == event.correlationId,
                   "causation and child event share a transaction correlation");
            sawChain = true;
        }
    }
    expect(sawChain, "a multi-effect process records a causal event chain");
    const auto snapshot = service.snapshotJson();
    const auto restored = WorldService::fromSnapshot(snapshot);
    expect(restored.state().events.size() == events.size(), "snapshot preserves event history length");
    for (std::size_t index = 0; index < events.size() && index < restored.state().events.size(); ++index) {
        expect(restored.state().events[index].eventId == events[index].eventId &&
                   restored.state().events[index].correlationId == events[index].correlationId &&
                   restored.state().events[index].causationSequence == events[index].causationSequence &&
                   restored.state().events[index].contentVersion == events[index].contentVersion &&
                   restored.state().events[index].simulationVersion == events[index].simulationVersion,
               "snapshot preserves event causal identity");
    }
    auto broken = service.snapshotJson();
    broken["events"].back()["causation_sequence"] = 999999;
    bool rejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(broken));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "snapshot rejects event causation that does not precede its effect");
    auto wrongRevision = service.snapshotJson();
    wrongRevision["events"].back()["world_revision"] = 999999;
    bool wrongRevisionRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(wrongRevision));
    } catch (const std::runtime_error&) {
        wrongRevisionRejected = true;
    }
    expect(wrongRevisionRejected, "snapshot rejects event revisions beyond the committed world");
}

void physics_events_are_deterministic_and_drained() {
    DeterministicPhysicsWorld physics;
    physics.addBody(PhysicsBody{"player", 0.0, 0.0, 0.5, true});
    physics.addBody(PhysicsBody{"ferry", 2.0, 0.0, 1.0, false});
    physics.applyImpulse("player", 1.0, 0.0);
    physics.step(0.02);
    const auto events = physics.drainEvents();
    expect(events.size() == 1 && events.front().type == "collision", "physics reports the ferry interaction");
    expect(physics.drainEvents().empty(), "physics event queue is drained for replay safety");
#if defined(TIDE_MARKS_HAS_BULLET)
    auto bullet = makeBulletPhysicsWorld();
    bullet->addBody(PhysicsBody{"player", 0.0, 0.0, 0.5, true});
    bullet->addBody(PhysicsBody{"ferry", 0.7, 0.0, 0.8, false});
    bullet->step(0.02);
    expect(!bullet->drainEvents().empty(), "Bullet reports the authored ferry collision");
#endif
}

void event_bus_is_ordered_and_filterable() {
    EventBus bus;
    std::vector<std::string> seen;
    const auto dayToken = bus.subscribe("life.day", [&seen](const WorldEvent& event) { seen.push_back("day:" + event.type); });
    const auto allToken = bus.subscribe("*", [&seen](const WorldEvent& event) { seen.push_back("all:" + event.type); });
    (void)dayToken;
    (void)allToken;
    bus.publish(WorldEvent{1, 1, GameTime{}, "life.ration", "mira", "village", "", 1, Json::object()});
    bus.publish(WorldEvent{1, 2, GameTime{}, "life.day", "world", "village", "", 2, Json::object()});
    expect(bus.pending() == 2, "event bus queues before dispatch");
    bus.dispatch();
    expect((seen == std::vector<std::string>{"all:life.ration", "day:life.day", "all:life.day"}),
           "event bus dispatches FIFO with exact and wildcard subscriptions");
    expect(bus.pending() == 0, "event bus drains its queue");

    EventBus isolated;
    std::size_t deliveredAfterFailure = 0;
    (void)isolated.subscribe("life.day", [](const WorldEvent&) { throw std::runtime_error("projection failed"); });
    (void)isolated.subscribe("life.day", [&deliveredAfterFailure](const WorldEvent&) { ++deliveredAfterFailure; });
    isolated.publish(WorldEvent{1, 1, GameTime{}, "life.day", "world", "village", "", 1, Json::object()});
    isolated.dispatch();
    expect(deliveredAfterFailure == 1, "a failed subscriber does not block later subscribers");
    expect(isolated.deliveryFailures() == 1, "subscriber failures are observable without escaping dispatch");
}

void world_service_publishes_committed_events() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    std::size_t delivered = 0;
    bool observedCommittedState = true;
    const auto token = service.eventBus().subscribe("life.day", [&service, &delivered, &observedCommittedState](const WorldEvent& event) {
        ++delivered;
        observedCommittedState = observedCommittedState && service.state().now.minute == event.at.minute;
    });
    (void)token;
    service.advanceMinutes(2 * kMinutesPerDay);
    expect(delivered == 2, "WorldService publishes each committed day event");
    expect(observedCommittedState,
           "each event is published after its own time-step transaction is committed");

    auto submitted = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    std::vector<std::int64_t> eventMinutes;
    (void)submitted.eventBus().subscribe("need.deprived", [&eventMinutes](const WorldEvent& event) {
        eventMinutes.push_back(event.at.minute);
    });
    const auto accepted = submitted.submit(Command{"advance-once", "advance", "world", {}, {}, "60"});
    const auto afterFirst = submitted.state().now.minute;
    const auto duplicate = submitted.submit(Command{"advance-once", "advance", "world", {}, {}, "60"});
    expect(accepted.accepted && !eventMinutes.empty(), "submitted advance publishes its committed steps");
    expect(std::adjacent_find(eventMinutes.begin(), eventMinutes.end(), std::greater<>()) == eventMinutes.end(),
           "submitted advance publishes event timestamps in ascending order");
    expect(!duplicate.accepted && submitted.state().now.minute == afterFirst,
           "a duplicate advance turn does not advance the world again");
}

void global_and_local_planners_are_deterministic() {
    GlobalPlanner global;
    global.addEdge("village", "marsh", 2.0);
    global.addEdge("marsh", "harbor", 2.0);
    global.addEdge("village", "harbor", 10.0);
    const auto route = global.plan("village", "harbor");
    expect(route.has_value() && *route == std::vector<std::string>{"village", "marsh", "harbor"},
           "global planner chooses the cheapest authored route");

    LocalPlanner local(5, 5);
    local.setBlocked({2, 0});
    local.setBlocked({2, 1});
    local.setBlocked({2, 2});
    const auto path = local.plan({0, 0}, {4, 0});
    expect(path.has_value() && path->front() == GridPoint{0, 0} && path->back() == GridPoint{4, 0},
           "local planner routes around obstacles");
}

void npc_state_machine_handles_interrupts() {
    auto mode = NpcMode::Working;
    mode = transition(mode, NpcSignal::StartConversation);
    expect(mode == NpcMode::InConversation, "NPC enters conversation mode");
    mode = transition(mode, NpcSignal::Interrupt);
    expect(mode == NpcMode::Interrupted, "interrupt preempts conversation");
    mode = transition(mode, NpcSignal::Resume);
    expect(mode == NpcMode::Working, "NPC resumes working after interruption");
    mode = transition(mode, NpcSignal::StartProcess);
    expect(mode == NpcMode::Executing, "NPC enters executing mode for a process");
    mode = transition(mode, NpcSignal::CancelProcess);
    expect(mode == NpcMode::Interrupted, "cancelling a process enters interrupted mode");
    mode = transition(mode, NpcSignal::Disable);
    expect(mode == NpcMode::Unavailable, "NPC can become unavailable");
}

void entity_registry_accepts_feature_components_without_core_changes() {
    struct WeatherComponent { int tide{0}; };
    EntityRegistry registry;
    expect(registry.create(EntityRecord{"fixture:test", "world_object", true}), "entity registry creates an entity");
    expect(registry.set<WeatherComponent>("fixture:test", WeatherComponent{3}), "custom component attaches generically");
    const auto* weather = registry.get<WeatherComponent>("fixture:test");
    expect(weather != nullptr && weather->tide == 3, "custom component can be queried by type");
    expect(registry.componentCount("fixture:test") == 1, "component count is tracked without type-specific setters");
    expect(registry.setActive("fixture:test", false), "entity lifecycle can mark an entity dormant");
    expect(!registry.isActive("fixture:test"), "dormant entities remain stored but are inactive");
    expect(registry.get<WeatherComponent>("fixture:test") != nullptr, "dormancy preserves components");
}

void entity_projection_tracks_committed_npc_movement() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    service.advanceMinutes(6 * kMinutesPerHour);
    const auto* location = service.entities().get<LocationComponent>("npc:mira");
    expect(location != nullptr && location->locationId == service.state().npcs.at("mira").location,
           "NPC component projection matches committed world state");
}

void world_entity_lifecycle_is_transactional_and_persistent() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto created = service.createEntity("object:field-note", "world_object",
                                               Json{{"author", "mira"}}, "entity-create");
    expect(created.accepted, "world can create an entity from an authored archetype");
    expect(service.entities().isActive("object:field-note"), "created entity is active in its projection");
    expect(service.state().entities.at("object:field-note").metadata.at("owned_by") == "world",
           "created entity merges archetype defaults with instance metadata");
    expect(service.state().entities.at("object:field-note").metadata.at("author") == "mira",
           "created entity retains command metadata");

    const auto invalid = service.createEntity("object:invalid", "missing_archetype");
    expect(!invalid.accepted && invalid.code == "unknown_archetype", "unregistered entity archetypes are rejected");

    expect(service.setEntityLifecycle("object:field-note", EntityLifecycle::Dormant).accepted,
           "active entity can become dormant");
    expect(service.entities().contains("object:field-note") && !service.entities().isActive("object:field-note"),
           "dormant entity keeps identity and components");
    expect(service.setEntityLifecycle("object:field-note", EntityLifecycle::Destroyed).accepted,
           "dormant entity can be destroyed");
    expect(!service.entities().contains("object:field-note"), "destroyed entity leaves active projection");
    expect(service.state().entities.at("object:field-note").lifecycle == EntityLifecycle::Destroyed,
           "destroyed entity identity remains auditable in world state");
    expect(!service.setEntityLifecycle("object:field-note", EntityLifecycle::Active).accepted,
           "destroyed entity identity cannot be revived");

    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
    expect(restored.state().entities.at("object:field-note").lifecycle == EntityLifecycle::Destroyed,
           "entity lifecycle survives snapshot restore");
    expect(!restored.entities().contains("object:field-note"), "destroyed entities are not projected after restore");
}

void resource_transfer_is_atomic_and_persistent() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto initial = service.state().resources.at("grain").amount;
    const auto delivered = service.transferResource("world", "mira", "grain", 7, kPlayerId, "transfer-1");
    expect(delivered.accepted, "registered transfer moves a resource from the public pool to an entity");
    expect(service.state().resources.at("grain").amount == initial - 7,
           "public resource quantity decreases by the transferred amount");
    expect(service.state().inventories.at("npc:mira").at("grain") == 7,
           "entity inventory receives the transferred quantity");
    expect(delivered.event && delivered.event->type == "resource.transferred",
           "transfer produces an auditable world event");
    const auto insufficient = service.transferResource("mira", "lorn", "grain", 99, "mira", "transfer-fail");
    expect(!insufficient.accepted && insufficient.code == "insufficient_resource",
           "insufficient entity inventory is rejected without partial mutation");
    expect(service.state().inventories.at("npc:lorn").empty(),
           "failed transfer leaves the destination inventory unchanged");
    auto restored = WorldService::fromSnapshot(service.snapshotJson());
    expect(restored.state().inventories.at("npc:mira").at("grain") == 7,
           "entity inventory survives snapshot restore");
    expect(restored.transferResource("mira", "world", "grain", 2, "mira", "transfer-2").accepted,
           "entity inventory can transfer back to the public pool");
}

void movement_operator_obeys_authored_topology() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto moved = service.moveNpc("mira", "village_square", "mira", "move-1");
    expect(moved.accepted && service.state().npcs.at("mira").location == "village_square",
           "movement operator commits an arrival along an authored route");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "entity.moved";
           }),
           "movement produces a fact event");
    const auto blocked = service.moveNpc("mira", "archive", "mira", "move-2");
    expect(blocked.accepted, "the authored village topology connects the archive");
    expect(!service.moveNpc("mira", "missing-place", "mira", "move-invalid").accepted,
           "movement rejects an unknown destination");
    expect(!service.moveNpc("mira", "granary", "eve", "move-unauthorized").accepted,
           "movement rejects an unrelated actor");

    const auto beforeLocation = service.state().npcs.at("mira").location;
    const auto moveEventsBeforeBlock = std::count_if(
        service.state().events.begin(), service.state().events.end(), [](const auto& event) {
            return event.type == "entity.moved";
        });
    expect(service.setLinkState("granary_square", false, false, "close-granary-square").accepted &&
               service.setLinkState("archive_square", false, false, "close-archive-square").accepted,
           "topology links can be closed through the runtime boundary");
    const auto blockedByTopology = service.moveNpc("mira", "village_square", "mira", "move-blocked");
    expect(!blockedByTopology.accepted && blockedByTopology.code == "no_route" &&
               service.state().npcs.at("mira").location == beforeLocation &&
               std::count_if(service.state().events.begin(), service.state().events.end(),
                             [](const auto& event) { return event.type == "entity.moved"; }) ==
                   moveEventsBeforeBlock,
           "movement cannot cross a fully closed authored topology and emits no move fact");
    expect(service.setLinkState("granary_square", true, false, "open-granary-square").accepted &&
               service.setLinkState("archive_square", true, false, "open-archive-square").accepted,
           "closed topology links can be restored without replacing the world");
    expect(service.moveNpc("mira", "village_square", "mira", "move-reopened").accepted,
           "restoring the authored topology makes the same route available again");
}

void behavior_plan_resolves_dynamic_targets_through_runtime() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.metaActions["goto_target"] = MetaActionDefinition{
        "goto_target", "movement", "Move to the proposed destination", "movement.traverse", Json::object()};
    BehaviorRecipeContent recipe;
    recipe.id = "goto_dynamic";
    recipe.planId = "goto_dynamic_plan";
    recipe.parameterSchema = Json{{"type", "object"}, {"required", Json::array({"destination"})},
                                  {"properties", Json{{"destination", Json{{"type", "string"}}}}},
                                  {"additionalProperties", false}};
    recipe.root = BehaviorNodeContent{
        "meta_action", "goto_dynamic.node", "goto_target",
        Json{{"destination", Json{{"bind", "destination"}}}}, 0, {}};
    content.behaviorRecipes.push_back(recipe);
    for (auto& npc : content.npcs) {
        if (npc.definition.id != "mira") continue;
        ActionCandidate candidate{"goto_dynamic", recipe.planId, "dynamic movement", 0, true, 1.0,
                                  "none", recipe.id, {}, 0, "none"};
        candidate.parameterSchema = recipe.parameterSchema;
        npc.actions.push_back(std::move(candidate));
    }
    auto provider = std::make_unique<ScriptedActionProvider>(
        std::vector<std::string>{"goto_dynamic"},
        std::map<std::string, Json>{{"goto_dynamic", Json{{"destination", "archive"}}}});
    auto service = WorldService::fromContent(content, std::move(provider));
    const auto turn = service.converse("mira", "go to the archive", "dynamic-goto");
    expect(turn.accepted && service.state().npcs.at("mira").location == "archive",
           "a generic behavior plan can bind a model-proposed destination through the authored topology");
}

void conflict_and_security_operators_change_facts_then_stimuli() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto attack = service.submit(Command{
        "attack-1", "security", "mira", "", "", R"({"operation":"conflict.attack","target":"lorn","damage":0.4})"});
    expect(attack.accepted, "a registered conflict attack commits through the command boundary");
    expect(service.state().security.at("npc:lorn").integrity < 1.0,
           "an attack changes target integrity facts");
    expect(std::any_of(service.state().npcs.at("lorn").stimuli.begin(), service.state().npcs.at("lorn").stimuli.end(),
                       [](const auto& stimulus) { return stimulus.category == "security_threat"; }),
           "the conflict fact projects a security stimulus to the participant");
    const auto defend = service.submit(Command{
        "defend-1", "security", "lorn", "", "", R"({"operation":"security.defend","target":"lorn"})"});
    expect(defend.accepted, "the target can defend itself through a registered operator");
    const auto repair = service.submit(Command{
        "repair-security-1", "security", "lorn", "", "", R"({"operation":"security.repair","target":"lorn","resource":"grain","amount":2,"integrity_restore":0.2})"});
    expect(repair.accepted, "repair consumes an authored resource and restores security facts");
    const auto integrityAfterRepair = service.state().security.at("npc:lorn").integrity;
    const auto sabotage = service.submit(Command{
        "sabotage-1", "security", "mira", "", "", R"({"operation":"conflict.sabotage","target":"lorn","damage":0.1})"});
    expect(sabotage.accepted && service.state().security.at("npc:lorn").integrity < integrityAfterRepair &&
               sabotage.event && sabotage.event->type == "conflict.sabotage",
           "sabotage is a distinct authored conflict operator with its own fact event");
    const auto threatBefore = service.state().security.at("npc:lorn").threat;
    const auto threat = service.submit(Command{
        "threaten-1", "security", "mira", "", "", R"({"operation":"security.threaten","target":"lorn","threat_gain":0.2})"});
    expect(threat.accepted && service.state().security.at("npc:lorn").threat > threatBefore &&
               threat.event && threat.event->type == "security.threaten",
           "threaten raises target threat without pretending to change integrity");
    const auto evacuate = service.submit(Command{
        "evacuate-1", "security", "lorn", "", "", R"({"operation":"security.evacuate","target":"lorn"})"});
    expect(evacuate.accepted && service.state().security.at("npc:lorn").evacuated &&
               evacuate.event && evacuate.event->type == "security.evacuate",
           "evacuate changes the target's safety fact and emits an auditable event");
    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
    expect(restored.state().security.at("npc:lorn").integrity == service.state().security.at("npc:lorn").integrity,
           "security facts survive snapshot restore");
    expect(restored.state().security.at("npc:lorn").evacuated &&
               std::any_of(restored.state().events.begin(), restored.state().events.end(), [](const auto& event) {
                   return event.type == "conflict.sabotage" || event.type == "security.threaten" ||
                          event.type == "security.evacuate";
               }),
           "security branch facts and their history survive snapshot restore");
}

void knowledge_and_experience_keep_provenance() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto promise = service.createPromise("player", "mira", "the ledger is safe", 1, "knowledge-source");
    expect(promise.accepted && promise.event.has_value(), "a committed event can become a learning source");
    const auto sourceSequence = promise.event->sequence;
    const auto fact = service.assertKnownFact("mira", "lorn", "owes", "ledger", sourceSequence,
                                              "direct", 0.9, "mira", "knowledge-1");
    expect(fact.accepted, "an NPC can record a sourced knowledge fact");
    const auto experience = service.recordExperience("mira", "social", "promise", "trust_lorn",
                                                     "promise_created", sourceSequence, 0.8, true,
                                                     "mira", "experience-1");
    expect(experience.accepted, "an NPC can abstract a sourced personal experience");
    expect(service.decisionContext("mira").knowledge.find("owes") != std::string::npos,
           "knowledge is projected into the decision context");
    expect(service.decisionContext("mira").experiences.find("trust_lorn") != std::string::npos,
           "experience is projected into the decision context");
    const auto snapshot = service.snapshotJson();
    const auto restored = WorldService::fromSnapshot(snapshot);
    expect(restored.state().npcs.at("mira").knowledge.size() == 1 &&
               restored.state().npcs.at("mira").knowledge.front().sourceSequence == sourceSequence,
           "knowledge provenance survives snapshot restore");
    expect(restored.state().npcs.at("mira").experiences.size() == 1 &&
               restored.state().npcs.at("mira").experiences.front().sourceSequences.front() == sourceSequence,
           "experience provenance survives snapshot restore");
    const auto rejected = service.assertKnownFact("mira", "secret", "known", "yes", 999999,
                                                  "direct", 1.0, "mira", "knowledge-invalid");
    expect(!rejected.accepted && rejected.code == "unknown_knowledge_source",
           "an NPC cannot invent a fact without a committed source event");
}

void successful_actions_automatically_create_experience() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto turn = service.converse("mira", "please offer a ration", "experience-action");
    expect(turn.accepted, "the action is committed before its experience is derived");
    service.advanceMinutes(30);
    const auto& experiences = service.state().npcs.at("mira").experiences;
    expect(!experiences.empty() && experiences.front().domain == "action" &&
               !experiences.front().sourceSequences.empty(),
           "a successful action records a reusable experience with event provenance");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "experience.observed";
           }),
           "automatic experience derivation is itself audited");
}

void extended_meta_actions_execute_from_content_recipes() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    const auto addAction = [&content](const std::string& actionId, const std::string& operation,
                                      Json args) {
        content.metaActions[actionId] = MetaActionDefinition{actionId, "test", actionId, operation, {}};
        BehaviorRecipeContent recipe;
        recipe.id = actionId;
        recipe.planId = actionId;
        recipe.durationMinutes = 0;
        recipe.root = BehaviorNodeContent{"meta_action", actionId + ".node", actionId, args, 0, {}};
        content.behaviorRecipes.push_back(std::move(recipe));
        for (auto& npc : content.npcs) {
            if (npc.definition.id != "mira") continue;
            npc.actions.push_back(ActionCandidate{actionId, actionId, actionId, 0, true, 1.0, "none",
                                                  actionId, {}});
        }
    };
    addAction("test_transfer", "resource.transfer",
              Json{{"source", "world"}, {"target", "self"}, {"resource", "grain"}, {"amount", 2}});
    addAction("test_move", "movement.traverse", Json{{"destination", "village_square"}});
    addAction("test_attack", "conflict.attack", Json{{"target", "lorn"}, {"damage", 0.2}});
    addAction("test_promise", "promise.create",
              Json{{"from", "self"}, {"to", "player"}, {"description", "test promise"}, {"due_days", 1}});
    addAction("test_promise_accept", "promise.transition",
              Json{{"promise_id", "promise-1"}, {"status", "accepted"}, {"detail", "accepted by recipe"}});
    addAction("test_knowledge", "knowledge.assert",
              Json{{"subject", "harbor"}, {"predicate", "status"}, {"value", "inspected"},
                   {"source_sequence", 1}, {"scope", "witness"}, {"confidence", 0.8}});
    addAction("test_experience", "experience.record",
              Json{{"domain", "trade"}, {"trigger", "inspection"}, {"outcome", "verified"},
                   {"source_sequence", 1}, {"confidence", 0.8}, {"personal", true}});
    addAction("test_production", "production.execute",
              Json{{"inputs", Json{{"grain", 5}}}, {"outputs", Json{{"boat_integrity", 3}}}});
    addAction("test_production_insufficient", "production.execute",
              Json{{"inputs", Json{{"grain", 1000}}}, {"outputs", Json{{"boat_integrity", 1}}}});
    addAction("test_production_capacity", "production.execute",
              Json{{"inputs", Json{{"grain", 1}}}, {"outputs", Json{{"boat_integrity", 1000}}}});
    addAction("test_exchange_create", "exchange.create",
              Json{{"proposer", "self"}, {"counterparty", "lorn"},
                   {"offered", Json{{"grain", 1}}}, {"requested", Json{{"boat_integrity", 1}}},
                   {"due_minutes", 30}});

    auto provider = std::make_unique<ScriptedActionProvider>(std::vector<std::string>{
        "test_transfer", "test_move", "test_attack", "test_promise", "test_promise_accept",
        "test_knowledge", "test_experience", "test_production", "test_production_insufficient",
        "test_production_capacity", "test_exchange_create"});
    auto service = WorldService::fromContent(content, std::move(provider));
    expect(service.converse("mira", "please test transfer", "meta-transfer").accepted &&
               service.state().inventories.at("npc:mira").at("grain") == 2,
           "a content recipe can invoke the resource-transfer operator");
    expect(service.converse("mira", "please test move", "meta-move").accepted &&
               service.state().npcs.at("mira").location == "village_square",
           "a content recipe can invoke the authored-topology movement operator");
    expect(service.converse("mira", "please test attack", "meta-attack").accepted &&
               service.state().security.at("npc:lorn").integrity < 1.0,
           "a content recipe can invoke the conflict operator");
    expect(service.converse("mira", "please test promise", "meta-promise").accepted &&
               service.state().promises.contains("promise-1"),
           "a content recipe can create a promise through the selected operator");
    expect(service.converse("mira", "please test accept promise", "meta-promise-accept").accepted &&
               service.state().promises.at("promise-1").status == PromiseStatus::Accepted,
           "a content recipe can transition a promise through the selected operator");
    expect(service.converse("mira", "please test knowledge", "meta-knowledge").accepted &&
               service.state().npcs.at("mira").knowledge.size() == 1,
           "a content recipe can assert sourced NPC knowledge");
    expect(service.converse("mira", "please test experience", "meta-experience").accepted &&
               !service.state().npcs.at("mira").experiences.empty(),
           "a content recipe can record sourced experience");
    const auto grainBefore = service.state().resources.at("grain").amount;
    const auto integrityBefore = service.state().resources.at("boat_integrity").amount;
    expect(service.converse("mira", "please test production", "meta-production").accepted &&
               service.state().resources.at("grain").amount == grainBefore - 5 &&
               service.state().resources.at("boat_integrity").amount == integrityBefore + 3,
           "a content recipe can execute a production input/output transition atomically");
    const auto failedInputGrain = service.state().resources.at("grain").amount;
    const auto failedInputIntegrity = service.state().resources.at("boat_integrity").amount;
    const auto productionEventsBeforeFailure = std::count_if(
        service.state().events.begin(), service.state().events.end(), [](const auto& event) {
            return event.type == "production.executed";
        });
    const auto insufficientProduction =
        service.converse("mira", "please demand exceeds supply", "meta-production-insufficient");
    expect(!insufficientProduction.accepted &&
               service.state().resources.at("grain").amount == failedInputGrain &&
               service.state().resources.at("boat_integrity").amount == failedInputIntegrity &&
               std::count_if(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
                   return event.type == "production.executed";
               }) == productionEventsBeforeFailure,
           "production rejects insufficient inputs atomically without facts or events");
    const auto capacityProduction =
        service.converse("mira", "please bin overflow", "meta-production-capacity");
    expect(!capacityProduction.accepted &&
               service.state().resources.at("grain").amount == failedInputGrain &&
               service.state().resources.at("boat_integrity").amount == failedInputIntegrity,
           "production rejects output overflow atomically without consuming inputs");
    expect(service.converse("mira", "please propose an exchange", "meta-exchange").accepted &&
               service.state().exchangeContracts.contains("exchange-1") &&
               service.state().exchangeContracts.at("exchange-1").status ==
                   ExchangeContractStatus::Proposed,
           "a content recipe can create a durable exchange proposal without moving terms");
    for (const auto& type : {"resource.transferred", "entity.moved", "conflict.attack", "promise.created",
                             "promise.accepted", "knowledge.asserted", "experience.recorded",
                             "production.executed", "exchange.proposed"}) {
        expect(std::any_of(service.state().events.begin(), service.state().events.end(), [&type](const auto& event) {
                   return event.type == type;
               }),
               std::string("the content recipe emits its auditable event: ") + type);
    }
}

void exchange_meta_actions_execute_from_scheduled_activity() {
    auto content = ContentPackLoader::load(TIDE_MARKS_TEST_DATA_DIR "/port-pack");
    content.metaActions["scheduled_exchange_create"] =
        MetaActionDefinition{"scheduled_exchange_create", "exchange", "scheduled_exchange_create",
                             "exchange.create", {}};
    content.metaActions["scheduled_exchange_accept"] =
        MetaActionDefinition{"scheduled_exchange_accept", "exchange", "scheduled_exchange_accept",
                             "exchange.transition", {}};
    content.metaActions["scheduled_exchange_settle"] =
        MetaActionDefinition{"scheduled_exchange_settle", "exchange", "scheduled_exchange_settle",
                             "exchange.settle", {}};
    const auto addActivity = [&content](std::string id, std::string actor, std::string metaAction,
                                        Json args) {
        EffectContent effect;
        effect.operation = "meta_action";
        effect.metaActionId = std::move(metaAction);
        effect.args = std::move(args);
        content.activities.push_back(ActivityContent{std::move(id), "hour", 10, std::move(actor),
                                                     "scheduled_exchange", "", 0,
                                                     "scheduled.exchange.activity",
                                                     "A scheduled exchange activity transition",
                                                     {std::move(effect)}});
    };
    addActivity("schedule_exchange_create", "sable", "scheduled_exchange_create",
                Json{{"proposer", "self"}, {"counterparty", "rhea"},
                     {"offered", Json{{"energy", 1}}}, {"requested", Json{{"coolant", 1}}},
                     {"due_minutes", 30}});
    addActivity("schedule_exchange_accept", "rhea", "scheduled_exchange_accept",
                Json{{"contract_id", "exchange-1"}, {"status", "accepted"},
                     {"reason", "scheduled acceptance"}});
    addActivity("schedule_exchange_settle", "rhea", "scheduled_exchange_settle",
                Json{{"contract_id", "exchange-1"}});

    auto service = WorldService::fromContent(content, std::make_unique<MockProvider>());
    service.advanceMinutes(60);
    expect(service.state().exchangeContracts.contains("exchange-1") &&
               service.state().exchangeContracts.at("exchange-1").status ==
                   ExchangeContractStatus::Settled,
           "scheduled activities can create, accept and settle a configured exchange");
    for (const auto& type : {"exchange.proposed", "exchange.accepted", "exchange.settled"}) {
        expect(std::any_of(service.state().events.begin(), service.state().events.end(),
                           [&type](const auto& event) { return event.type == type; }),
               std::string("scheduled exchange activity emits ") + type);
    }
}

void runtime_loads_a_world_without_village_identifiers() {
    const std::filesystem::path fixtureRoot = std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "minimal-pack";
    auto service = WorldService::load(
        fixtureRoot.string(), std::make_unique<SpeechActProvider>("request", "cool_service_lift"));
    expect(service.state().worldId == "fixture_world", "world identity comes from the content pack");
    expect(service.state().instanceId == "fixture.minimal.instance", "replacement instance identity is preserved");
    expect(service.state().now == GameTime::fromDayHour(4, 9), "world clock comes from the content pack");
    expect(service.state().npcs.contains("sable") && !service.state().npcs.contains("mira"),
           "world loads arbitrary authored NPCs without village identifiers");
    expect(service.state().npcs.at("sable").definitionId == "sable",
           "runtime NPC state stores the authored definition id");
    expect(service.content().instance && service.content().instance->id == "fixture.minimal.instance",
           "replacement world is launched through its own game instance");
    expect(service.content().decisionChannels.contains("player_dialogue") &&
               service.content().conversationChannelId == "player_dialogue" &&
               service.content().decisionChannels.at("player_dialogue").providerPolicy == "player_dialogue",
           "replacement world supplies its own decision channel policy");
    const auto fixtureSnapshot = service.snapshotJson();
    expect(fixtureSnapshot.at("npcs").at("sable").at("definition_id") == "sable" &&
               !fixtureSnapshot.at("npcs").at("sable").contains("definition"),
           "replacement world snapshot contains only its content reference");
    expect(service.state().npcs.at("sable").needs.contains("safety"),
           "arbitrary authored NPC needs load from JSON content");
    const auto& authoredNpc = *std::find_if(service.content().npcs.begin(), service.content().npcs.end(),
                                            [](const auto& npc) { return npc.definition.id == "sable"; });
    const auto& authoredNeed = *std::find_if(authoredNpc.needs.begin(), authoredNpc.needs.end(),
                                             [](const auto& need) { return need.id == "safety"; });
    expect(authoredNeed.domain == "operational_resilience",
           "a replacement world can author a domain outside the village's four default domains");
    const auto initialResources = service.state().resources;
    expect(initialResources.contains("energy") && initialResources.contains("coolant") &&
               !initialResources.contains("grain"),
           "arbitrary world resources load from JSON without village-specific fields");
    const auto turn = service.converse("sable", "please cool the service lift", "fixture-reservation");
    expect(turn.accepted && service.state().npcs.at("sable").activity.process &&
               service.state().npcs.at("sable").activity.process->actionId == "cool_service_lift",
           "the fixture world runs an arbitrary configured resource process");
    expect(service.state().resources.at("energy").reserved == 2,
           "reservation mode works for fixture resources without village identifiers");
    service.advanceMinutes(10);
    expect(service.state().resources.at("energy").amount == 5,
           "the generic runtime settles arbitrary resource inputs at process completion");
    expect(service.simulationMetrics().farNpcTicks == 2 &&
               service.state().npcs.at("sable").lastSimulationTick.minute == service.state().now.minute,
           "far simulation advances on the authored coarse cadence");
    expect(service.simulationMetrics().skippedNpcTicks == 8 &&
               service.simulationMetrics().simulatedMinutes == 10,
           "the scheduler records skipped far-tier ticks and simulated time");
    const auto settledMinute = service.state().now.minute;
    service.advanceTo(GameTime{settledMinute - 1});
    expect(service.state().now.minute == settledMinute,
           "advance_to is idempotent when the target is already in the past");
    const auto workshop = service.entities().record("spatial:workshop");
    const auto serviceLift = service.entities().record("link:service_lift");
    expect(workshop.has_value() && workshop->kind == "zone", "authored spatial kinds remain generic");
    expect(serviceLift.has_value() && serviceLift->kind == "lift", "authored link kinds remain generic");
    const auto* location = service.entities().get<LocationComponent>("npc:sable");
    expect(location != nullptr && location->locationId == "workshop" && location->regionId == "fixture_world",
           "NPC location projection uses authored IDs and world identity");
    const auto* link = service.entities().get<LinkComponent>("link:service_lift");
    expect(link != nullptr && link->endpoints == std::vector<std::string>{"workshop", "yard"},
           "N-ary authored links load into runtime components");
    expect(service.planRoute("workshop", "yard").has_value(), "active links provide a global route");
    expect(service.setLinkState("service_lift", false, false).accepted, "link state is a transactional world fact");
    expect(!service.planRoute("workshop", "yard").has_value(), "closed links block route planning");
    expect(service.setLinkState("service_lift", true, false).accepted, "link can be reopened by a command");
}

void social_groups_trade_and_relationships_are_shared_facts() {
    const std::filesystem::path fixtureRoot = std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "minimal-pack";
    auto service = WorldService::load(fixtureRoot.string());
    expect(service.state().socialGroups.contains("workshop_crew") &&
               service.state().socialGroups.at("workshop_crew").members.at("sable") == "operator",
           "social group membership loads from content without village identifiers");
    const auto relation = service.adjustRelationship(
        "sable", "workshop_crew", RelationshipAxes{-0.1, 0.15, 0.0, 0.05}, "sable", "social-relation",
        "Sable disagreed with the crew decision");
    expect(relation.accepted && service.state().npcs.at("sable").relationships.at("workshop_crew").respect > 0.49,
           "an NPC can hold a four-axis relationship with an organization");
    expect(service.adjustGroupMetric("workshop_crew", "morale", -0.2, "world", "social-morale",
                                     "The crew absorbed a setback").accepted &&
               service.state().socialGroups.at("workshop_crew").metrics.at("morale") < 0.41,
           "group metrics change through an audited runtime fact");
    expect(service.setGroupMembership("workshop_crew", "sable", "foreman", true, "world", "social-role").accepted &&
               service.state().socialGroups.at("workshop_crew").members.at("sable") == "foreman",
           "group membership roles are mutable facts");
    expect(service.transferResource("world", "player", "energy", 5, "world", "trade-setup-player").accepted &&
               service.transferResource("world", "sable", "coolant", 3, "world", "trade-setup-sable").accepted,
           "trade parties receive inventories through the generic resource ledger");
    expect(service.tradeResources("sable", "player", "coolant", 2, "energy", 3, "sable", "social-trade").accepted,
           "two resource transfers settle as one auditable trade");
    expect(service.state().inventories.at("npc:sable").at("coolant") == 1 &&
               service.state().inventories.at("player").at("coolant") == 2 &&
               service.state().inventories.at("npc:sable").at("energy") == 3 &&
               service.state().inventories.at("player").at("energy") == 2,
           "both sides of the trade settle in one transaction");
    const auto beforeFailedTrade = service.state().inventories;
    expect(!service.tradeResources("sable", "player", "coolant", 1, "energy", 99, "sable", "bad-trade").accepted,
           "a trade fails when one side cannot pay");
    expect(service.state().inventories == beforeFailedTrade,
           "failed trade compensation restores the first leg");
    const auto snapshot = service.snapshotJson();
    const auto restored = WorldService::fromSnapshot(snapshot, fixtureRoot.string());
    expect(restored.state().socialGroups.at("workshop_crew").metrics ==
               service.state().socialGroups.at("workshop_crew").metrics &&
               restored.state().npcs.at("sable").relationships.at("workshop_crew").respect ==
               service.state().npcs.at("sable").relationships.at("workshop_crew").respect,
           "social relations and group state survive snapshot restore");
}

void port_slice_is_a_replaceable_social_instance() {
    const auto fixtureRoot = std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "port-pack";
    auto service = WorldService::load(fixtureRoot.string());
    expect(service.content().id == "fixture.port" && service.state().npcs.size() == 2,
           "the harbor slice loads as a separate content pack with two NPCs");
    expect(service.state().inventories.at("npc:sable").at("energy") == 3 &&
               service.state().inventories.at("npc:rhea").at("coolant") == 2,
           "game instance inventories seed actor-owned resources");
    service.advanceMinutes(60);
    expect(service.state().inventories.at("npc:sable").at("energy") == 2 &&
               service.state().inventories.at("npc:sable").at("coolant") == 1 &&
               service.state().inventories.at("npc:rhea").at("energy") == 1 &&
               service.state().inventories.at("npc:rhea").at("coolant") == 1,
           "a scheduled activity settles an authored exchange through the generic operator");
    expect(service.state().socialGroups.at("dock_cooperative").metrics.at("morale") > 0.59 &&
               service.state().npcs.at("sable").relationships.at("rhea").trust > 0.44,
           "the exchange updates organization and cross-NPC relationship facts");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "trade.settled";
           }),
           "the harbor exchange is replayable through a settled trade event");
    const auto snapshot = service.snapshotJson();
    const auto restored = WorldService::fromSnapshot(snapshot, fixtureRoot.string());
    expect(restored.state().inventories.at("npc:sable").at("energy") ==
               service.state().inventories.at("npc:sable").at("energy") &&
               restored.state().inventories.at("npc:rhea").at("coolant") ==
                   service.state().inventories.at("npc:rhea").at("coolant") &&
               restored.state().socialGroups.at("dock_cooperative").metrics.at("morale") ==
                   service.state().socialGroups.at("dock_cooperative").metrics.at("morale"),
           "the replaceable harbor instance restores its social facts");
}

void glass_orchard_is_a_second_replaceable_world_instance() {
    const auto fixtureRoot = std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "glass-orchard-pack";
    auto service = WorldService::load(fixtureRoot.string());
    expect(service.content().id == "fixture.glass_orchard" && service.state().worldId == "glass_orchard_world" &&
               service.state().npcs.size() == 2,
           "a second fiction instance loads without village or harbor identifiers");
    expect(service.state().npcs.at("orin").needs.contains("route_stewardship") &&
               service.state().npcs.at("kel").needs.contains("canopy_resilience"),
           "the second instance supplies its own stewardship and ecological drives");
    expect(service.planRoute("lampsmith", "glass_orchard").has_value(),
           "the second instance supplies a connected cross-region topology");
    expect(service.state().inventories.at("npc:orin").at("lamp_oil") == 3 &&
               service.state().inventories.at("npc:kel").at("resin") == 2,
           "the second instance seeds actor-owned resources through the common ledger");
    service.advanceMinutes(60);
    expect(service.state().inventories.at("npc:orin").at("lamp_oil") == 2 &&
               service.state().inventories.at("npc:orin").at("resin") == 1 &&
               service.state().inventories.at("npc:kel").at("lamp_oil") == 1 &&
               service.state().inventories.at("npc:kel").at("resin") == 1,
           "the second instance settles its authored exchange with the generic operator");
    expect(service.state().socialGroups.at("ridge_compact").metrics.at("accord") > 0.49 &&
               service.state().npcs.at("orin").relationships.at("kel").trust > 0.42,
           "the second instance produces institution and relationship facts");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(), [](const auto& event) {
               return event.type == "trade.settled";
           }),
           "the second instance exchange is represented by an auditable trade event");
    const auto promise = service.createPromise("orin", "kel", "deliver the winter lamp oil", 1,
                                               "glass-orchard-promise");
    expect(promise.accepted &&
               service.decidePromise(promise.message, PromiseStatus::Accepted, "kel",
                                     "glass-orchard-promise-accept").accepted,
           "the second instance uses the same persistent social contract lifecycle");
    service.advanceMinutes(kMinutesPerDay);
    expect(service.state().promises.at(promise.message).status == PromiseStatus::Broken &&
               std::any_of(service.state().events.begin(), service.state().events.end(), [&promise](const auto& event) {
                   return event.type == "promise.broken" && event.data.value("promise_id", "") == promise.message;
               }),
           "an overdue promise produces a later world consequence without a second exchange rule");
    const auto snapshot = service.snapshotJson();
    auto restored = WorldService::fromSnapshot(snapshot, fixtureRoot.string());
    expect(restored.state().contentVersion == service.state().contentVersion &&
               restored.state().npcs.at("kel").needs.at("canopy_resilience").deficit ==
                   service.state().npcs.at("kel").needs.at("canopy_resilience").deficit,
           "the second instance restores custom drive state without a compatibility alias");
}

void offline_scheduler_baseline_is_measurable() {
    auto village = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    auto harbor = WorldService::load(
        (std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "port-pack").string());
    const auto villageStart = village.state().now.minute;
    const auto harborStart = harbor.state().now.minute;
    village.advanceTo(GameTime{villageStart + 2 * kMinutesPerDay});
    harbor.advanceTo(GameTime{harborStart + 2 * kMinutesPerDay});
    const auto villageSnapshotBytes = village.snapshotJson().dump().size();
    const auto harborSnapshotBytes = harbor.snapshotJson().dump().size();
    expect(village.simulationMetrics().simulatedMinutes == 2 * kMinutesPerDay &&
               harbor.simulationMetrics().simulatedMinutes == 2 * kMinutesPerDay,
           "multiple world instances advance through the same advance_to contract");
    expect(village.simulationMetrics().nearNpcTicks > village.simulationMetrics().farNpcTicks &&
               harbor.simulationMetrics().farNpcTicks > 0 && villageSnapshotBytes > 0 && harborSnapshotBytes > 0,
           "the baseline exposes near/far work and snapshot size for offline profiling");

    auto minimalContent = ContentPackLoader::load(
        (std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "minimal-pack").string());
    minimalContent.npcDeliberation.enabled = false;
    auto farOnly = WorldService::fromContent(minimalContent, std::make_unique<MockProvider>());
    farOnly.advanceMinutes(kMinutesPerDay);
    expect(farOnly.simulationMetrics().scheduledBoundaries <
               static_cast<std::uint64_t>(farOnly.simulationMetrics().simulatedMinutes),
           "far-only worlds coalesce quiet minutes at scheduled wakeup boundaries");
    expect(farOnly.simulationMetrics().simulatedMinutes == kMinutesPerDay,
           "coalesced scheduling preserves the requested elapsed world time");
}

void world_intelligence_detects_anomalies_and_runs_counterfactuals() {
    const auto fixtureRoot = std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "port-pack";
    auto service = WorldService::load(fixtureRoot.string());
    const auto initial = AnomalyDetector::scan(service.content(), service.state());
    expect(initial.size() == 1 && initial.front().ruleId == "cooperative_morale_low" &&
               initial.front().severity == "warning",
           "deterministic anomaly rules report the depleted cooperative morale");
    service.advanceMinutes(60);
    expect(AnomalyDetector::scan(service.content(), service.state()).empty(),
           "a committed exchange clears the modeled anomaly without LLM arbitration");
    const ScenarioProposal proposal{
        "scenario.morale.pressure", "llm", service.content().contentVersion, service.state().revision, 120,
        {{ "group_metric.delta", "dock_cooperative", "morale", -0.3,
           "Test a sustained morale shock without changing the source world" }}};
    expect(ScenarioManager::validate(proposal, service).valid,
           "a scenario may perturb only authored social facts");
    const auto beforeVersion = service.state().contentVersion;
    const auto beforeRevision = service.state().revision;
    const auto experiment = ScenarioManager::run(proposal, service);
    expect(experiment.passed && experiment.baselineMetrics.at("social_groups").is_object() &&
               experiment.scenarioMetrics.at("social_groups").is_object() &&
               experiment.delta.at("social_groups").at("dock_cooperative").at("morale").get<double>() < -0.29 &&
               experiment.baselineMetrics.contains("inventories") &&
               experiment.baselineMetrics.contains("locations") &&
               experiment.baselineEvents.is_array() && experiment.events.is_array(),
           "the counterfactual reports the modeled morale delta against a baseline run");
    expect(service.state().revision == beforeRevision && service.state().contentVersion == beforeVersion,
           "the source world remains unchanged by an exploratory scenario");
    const ScenarioProposal invalid{"scenario.invalid", "llm", service.content().contentVersion,
                                   service.state().revision, 10,
                                   {{ "group_metric.delta", "missing_group", "morale", 1.0, "bad" }}};
    expect(!ScenarioManager::validate(invalid, service).valid,
           "scenario experiments reject facts that are not modeled by the world");
    const auto stale = ScenarioProposal{"scenario.stale", "llm", service.content().contentVersion,
                                       service.state().revision - 1, 10,
                                       {{"group_metric.delta", "dock_cooperative", "morale", 0.1, "stale"}}};
    const auto staleValidation = ScenarioManager::validate(stale, service);
    expect(!staleValidation.valid && staleValidation.code == "revision_conflict",
           "counterfactuals reject stale world revisions instead of silently branching from the current world");
    expect(anomalyFindingToJson(initial.front()).at("rule_id") == "cooperative_morale_low" &&
               scenarioExperimentResultToJson(experiment).at("proposal_id") == proposal.id &&
               ScenarioManager::fromJson(ScenarioManager::toJson(proposal)).id == proposal.id,
           "anomaly and scenario results have stable JSON audit representations");
}

}  // namespace

void npc_activity_slot_is_single_owner() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    expect(service.converse("mira", "please offer a ration", "activity-owner").accepted,
           "an accepted intent creates an activity slot");
    const auto& activity = service.state().npcs.at("mira").activity;
    expect(activity.status == ActivityStatus::Active && activity.goal && activity.process,
           "goal and process are projections of one active slot");
    expect(activity.goal->status == GoalStatus::Active && activity.process->status == ProcessStatus::Running,
           "goal and process statuses agree with the slot");
    expect(service.state().npcs.at("mira").mode == "executing", "FSM is projected from the activity slot");
    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
    const auto& restoredActivity = restored.state().npcs.at("mira").activity;
    expect(restoredActivity.status == ActivityStatus::Active && restoredActivity.goal &&
               restoredActivity.goal->id == activity.goal->id,
           "activity slot restores with the same goal identity");
    auto brokenProjection = service.snapshotJson();
    brokenProjection["npcs"]["mira"]["activity"]["process"]["status"] = "suspended";
    bool brokenProjectionRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(brokenProjection));
    } catch (const std::runtime_error&) {
        brokenProjectionRejected = true;
    }
    expect(brokenProjectionRejected,
           "snapshot loading rejects activity projections that disagree with the lifecycle phase");
    auto proposedPayload = service.snapshotJson();
    proposedPayload["npcs"]["mira"]["activity"]["status"] = "proposed";
    bool proposedPayloadRejected = false;
    try {
        static_cast<void>(WorldService::fromSnapshot(proposedPayload));
    } catch (const std::runtime_error&) {
        proposedPayloadRejected = true;
    }
    expect(proposedPayloadRejected,
           "snapshot loading rejects a proposed activity that already owns executable state");
    auto completed = WorldService::fromSnapshot(service.snapshotJson());
    completed.advanceMinutes(30);
    const auto& after = completed.state().npcs.at("mira").activity;
    expect(after.status == ActivityStatus::Proposed && !after.goal && !after.process,
           "completion clears the single activity slot");
    expect(completed.state().npcs.at("mira").mode != "executing",
           "FSM leaves executing mode after completion");
}

void evolution_proposals_are_governed() {
    const auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    EvolutionProposal proposal{
        "proposal.recipe.1", "declarative_capability", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "assist_neighbor"},
                         {"available_to", Json::array({"mira"})},
                         {"root", Json{{"type", "meta_action"}, {"id", "assist"},
                                       {"name", "relieve_need"}}}}})};
    proposal.evidenceEventSequences = {service.state().events.back().sequence};
    const auto valid = EvolutionManager::validate(proposal, service.content(), service.state());
    expect(valid.valid, "declarative evolution proposal passes structural and evidence validation");

    auto parameterized = proposal;
    parameterized.id = "proposal.recipe.parameterized";
    parameterized.operations.at(0)["parameters"] = Json{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", Json{{"destinations", Json{{"type", "array"}, {"minItems", 1},
                                                       {"uniqueItems", true},
                                                       {"items", Json{{"type", "string"}}}}}}}};
    const auto parameterizedValidation = EvolutionManager::validate(parameterized, service.content(), service.state());
    expect(parameterizedValidation.valid,
           "evolved behavior parameters accept the bounded uniqueItems JSON Schema subset");
    auto duplicateActors = parameterized;
    duplicateActors.id = "proposal.recipe.duplicate_actors";
    duplicateActors.operations.at(0)["available_to"] = Json::array({"mira", "mira"});
    const auto duplicateActorValidation = EvolutionManager::validate(duplicateActors, service.content(), service.state());
    expect(duplicateActorValidation.code == "duplicate_behavior_recipe_actor",
           "evolved behavior propagation scopes reject duplicate actor identities");

    const auto proposalJson = EvolutionManager::toJson(proposal);
    expect(proposalJson.at("schema_version") == 1 &&
               EvolutionManager::fromJson(proposalJson).evidenceEventSequences == proposal.evidenceEventSequences,
           "evolution proposal JSON round-trips its schema version and source evidence");
    auto missingEvidenceJson = proposalJson;
    missingEvidenceJson.erase("evidence_event_sequences");
    bool missingEvidenceRejected = false;
    try {
        static_cast<void>(EvolutionManager::fromJson(missingEvidenceJson));
    } catch (const std::exception&) {
        missingEvidenceRejected = true;
    }
    expect(missingEvidenceRejected, "proposal parsing rejects missing source evidence before sandboxing");
    auto unsupportedSchemaJson = proposalJson;
    unsupportedSchemaJson["schema_version"] = 2;
    bool unsupportedSchemaRejected = false;
    try {
        static_cast<void>(EvolutionManager::fromJson(unsupportedSchemaJson));
    } catch (const std::exception&) {
        unsupportedSchemaRejected = true;
    }
    expect(unsupportedSchemaRejected, "proposal parsing rejects unsupported schema versions");
    auto missingSourceJson = proposalJson;
    missingSourceJson.erase("source");
    bool missingSourceRejected = false;
    try {
        static_cast<void>(EvolutionManager::fromJson(missingSourceJson));
    } catch (const std::exception&) {
        missingSourceRejected = true;
    }
    expect(missingSourceRejected, "proposal parsing requires an explicit model or actor source");

    auto noEvidence = proposal;
    noEvidence.evidenceEventSequences.clear();
    expect(EvolutionManager::validate(noEvidence, service.content(), service.state()).code == "evidence_required",
           "evolution proposals cannot enter governance without committed source evidence");
    auto unknownEvidence = proposal;
    unknownEvidence.evidenceEventSequences = {static_cast<std::uint64_t>(service.state().events.size() + 1000)};
    expect(EvolutionManager::validate(unknownEvidence, service.content(), service.state()).code ==
               "unknown_evidence_event",
           "evolution proposals cannot cite fabricated events");
    auto invalidKind = proposal;
    invalidKind.kind = "runtime_core";
    expect(EvolutionManager::validate(invalidKind, service.content(), service.state()).code ==
               "invalid_proposal_kind",
           "evolution kinds come from the governed registry");
    proposal.status = EvolutionProposalStatus::Validated;
    const auto approved = EvolutionManager::approve(proposal, "developer", true);
    expect(approved.valid && proposal.status == EvolutionProposalStatus::Approved,
           "only an approved sandbox result activates a proposal");
    EvolutionProposal forbidden = proposal;
    forbidden.status = EvolutionProposalStatus::Proposed;
    forbidden.operations = Json::array({Json{{"operation", "inject_code"}, {"id", "bad"}}});
    expect(!EvolutionManager::validate(forbidden, service.content(), service.state()).valid,
           "Runtime Core mutation is rejected before approval");
    expect(!EvolutionManager::approve(forbidden, "llm", true).valid,
           "LLM cannot approve its own evolution proposal");
    auto stale = proposal;
    stale.baseWorldRevision = service.state().revision + 1;
    expect(EvolutionManager::validate(stale, service.content(), service.state()).code ==
               "revision_conflict",
           "evolution proposals cannot silently bind to a different world revision");

    EvolutionProposal unknownAction{
        "proposal.recipe.unknown_action", "declarative_capability", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "invalid_action_recipe"},
                         {"available_to", Json::array({"mira"})},
                         {"root", Json{{"type", "meta_action"}, {"id", "action"},
                                       {"name", "not_a_registered_meta_action"}}}}})};
    unknownAction.evidenceEventSequences = proposal.evidenceEventSequences;
    const auto unknownActionValidation =
        EvolutionManager::validate(unknownAction, service.content(), service.state());
    expect(!unknownActionValidation.valid && unknownActionValidation.code == "unknown_meta_action_reference",
           "overlay recipes cannot invoke meta-actions absent from the active content registry");
    const auto unknownActionSandbox = EvolutionManager::sandbox(unknownAction, service);
    expect(!unknownActionSandbox.passed && unknownActionSandbox.code == "unknown_meta_action_reference",
           "the sandbox independently rejects unresolved overlay meta-actions");

    EvolutionProposal unknownCondition{
        "proposal.recipe.unknown_condition", "declarative_capability", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "invalid_condition_recipe"},
                         {"available_to", Json::array({"mira"})},
                         {"root", Json{{"type", "condition"}, {"id", "condition"},
                                       {"name", "not_a_registered_condition"}}}}})};
    unknownCondition.evidenceEventSequences = proposal.evidenceEventSequences;
    expect(EvolutionManager::validate(unknownCondition, service.content(), service.state()).code ==
               "unknown_condition_reference",
           "overlay recipes cannot invoke conditions absent from the active content registry");

    const Json duplicateRoot = Json{
        {"type", "sequence"}, {"id", "root"},
        {"children", Json::array({
            Json{{"type", "condition"}, {"id", "duplicate"}, {"name", "has_inputs"}},
            Json{{"type", "condition"}, {"id", "duplicate"}, {"name", "has_output_capacity"}}})}};
    EvolutionProposal duplicateNode{
        "proposal.recipe.duplicate_node", "declarative_capability", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "duplicate_node_recipe"},
                         {"available_to", Json::array({"mira"})},
                         {"root", duplicateRoot}}})};
    duplicateNode.evidenceEventSequences = proposal.evidenceEventSequences;
    expect(EvolutionManager::validate(duplicateNode, service.content(), service.state()).code ==
               "invalid_behavior_recipe",
           "overlay recipes reject duplicate behavior node identities");
}

void evolution_sandbox_isolated_and_versioned() {
    auto base = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    EvolutionProposal proposal{
        "overlay.archetype.sandbox", "declarative_capability", "llm", base.content().contentVersion,
        base.state().revision,
        Json::array({Json{{"operation", "create_entity_archetype"}, {"id", "market_stall"},
                         {"kind", "facility"}, {"default_metadata", Json{{"capacity", 12}}}}})};
    proposal.evidenceEventSequences = {base.state().events.back().sequence};
    const auto sandbox = EvolutionManager::sandbox(proposal, base, 60);
    expect(sandbox.passed && sandbox.overlayVersion.find("overlay.archetype.sandbox") != std::string::npos,
           "an evolution proposal runs in an isolated versioned sandbox");
    expect(sandbox.baseRevision == base.state().revision,
           "sandbox records the source world revision");
    expect(sandbox.baseContentVersion == base.content().contentVersion,
           "sandbox records the source content version");
    expect(!base.content().entityArchetypes.empty() &&
               std::none_of(base.content().entityArchetypes.begin(), base.content().entityArchetypes.end(),
                            [](const auto& item) { return item.id == "market_stall"; }),
           "sandbox content changes do not mutate the source world");
    expect(sandbox.metrics.at("events_added").is_number(), "sandbox returns measurable event differences");

    EvolutionProposal approved = proposal;
    approved.status = EvolutionProposalStatus::Validated;
    expect(EvolutionManager::approve(approved, "developer", sandbox.passed).valid,
           "an independently approved sandbox result can activate");
    const auto sourceVersion = base.content().contentVersion;
    expect(base.activateOverlay(approved, sandbox).accepted,
           "an approved sandbox result activates through the world service");
    expect(base.content().contentVersion == sandbox.overlayVersion &&
               base.state().activeOverlays.size() == 1,
           "activation records the exact overlay version on the world instance");
    const auto overlaySnapshot = base.snapshotJson();
    auto restoredOverlay = WorldService::fromSnapshot(overlaySnapshot);
    expect(restoredOverlay.state().activeOverlays.size() == 1 &&
               restoredOverlay.content().contentVersion == sandbox.overlayVersion &&
               std::any_of(restoredOverlay.content().entityArchetypes.begin(),
                           restoredOverlay.content().entityArchetypes.end(),
                           [](const auto& item) { return item.id == "market_stall"; }),
           "an activated overlay and its operations survive snapshot restore");
    expect(restoredOverlay.rollbackOverlay(approved.id).accepted &&
               restoredOverlay.state().activeOverlays.empty() &&
               restoredOverlay.content().contentVersion == sourceVersion,
           "a restored overlay can be rolled back without an in-memory backup");
    expect(base.rollbackOverlay(approved.id).accepted,
           "an activated overlay can be rolled back");
    expect(base.content().contentVersion == sourceVersion && base.state().activeOverlays.empty(),
           "rollback restores the content view without erasing world history");
    expect(std::any_of(base.state().events.begin(), base.state().events.end(), [](const auto& event) {
               return event.type == "evolution.overlay_rolled_back";
           }),
           "overlay rollback is audited as a world event");
    auto overlayContent = base.content();
    const auto activated = EvolutionManager::applyOverlay(overlayContent, approved);
    expect(activated.valid && overlayContent.contentVersion == sandbox.overlayVersion &&
               std::any_of(overlayContent.entityArchetypes.begin(), overlayContent.entityArchetypes.end(),
                                          [](const auto& item) { return item.id == "market_stall"; }),
           "approved overlay operations can be applied to a versioned content view");

    auto atomicOverlayContent = base.content();
    const auto atomicOverlayVersion = atomicOverlayContent.contentVersion;
    EvolutionProposal invalidBatch = approved;
    invalidBatch.id = "overlay.partial.failure";
    invalidBatch.operations = Json::array({
        Json{{"operation", "create_entity_archetype"}, {"id", "temporary_stall"}, {"kind", "facility"}},
        Json{{"operation", "unsupported_after_valid"}, {"id", "invalid_tail"}}});
    const auto rejectedBatch = EvolutionManager::applyOverlay(atomicOverlayContent, invalidBatch);
    expect(!rejectedBatch.valid && atomicOverlayContent.contentVersion == atomicOverlayVersion &&
               std::none_of(atomicOverlayContent.entityArchetypes.begin(), atomicOverlayContent.entityArchetypes.end(),
                            [](const auto& item) { return item.id == "temporary_stall"; }),
           "a multi-operation overlay is committed only when every operation succeeds");

    const auto path = (std::filesystem::temp_directory_path() / "tide_marks_evolution.jsonl").string();
    std::filesystem::remove(path);
    EvolutionProposalStore::append(path, approved);
    const auto loaded = EvolutionProposalStore::load(path);
    expect(loaded.size() == 1 && loaded.front().id == approved.id,
           "evolution proposals persist as auditable JSONL records");
    std::filesystem::remove(path);
}

void innovation_reflection_lifecycle_is_governed() {
    const auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto evidence = service.state().events.back().sequence;
    const Json innovationSpec{
        {"hypothesis", "A private repair rehearsal can reduce repeated ferry failures."},
        {"novelty_basis", "new_combination"},
        {"operation_ids", Json::array({"private_repair_rehearsal"})},
        {"expected_benefits", Json::array({"lower repeated failure cost"})},
        {"risks", Json::array({"consumes time without changing shared infrastructure"})},
        {"test_plan", Json{{"horizon_minutes", 120}, {"metrics", Json::array({"events_added", "resource_total"})},
                             {"baseline", "Mira uses the existing ferry repair routine."},
                             {"scenario", "Mira rehearses a private repair behavior."},
                             {"expected_metrics", Json{{"events_added", 2.0}, {"resource_total", 100.0}}},
                             {"constraints", Json::array({"do not reduce record integrity"})},
                             {"resource_budget", Json{{"resource_total", 5.0}}},
                             {"success_criteria", "The rehearsal completes within the horizon."},
                             {"failure_criteria", "The rehearsal exceeds its resource budget."}}}};
    EvolutionProposal level1{
        "innovation.reflection.private", "innovation", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "private_repair_rehearsal"},
                         {"available_to", Json::array({"mira"})},
                         {"root", Json{{"type", "meta_action"}, {"id", "root"},
                                       {"name", "relieve_need"}}}}})};
    level1.evidenceEventSequences = {evidence};
    level1.evolutionLevel = "level1";
    level1.targetNpcId = "mira";
    level1.innovation = innovationSpec;
    expect(EvolutionManager::validate(level1, service.content(), service.state()).valid,
           "daily reflection can produce a private Level 1 innovation hypothesis");
    auto sharedPrivate = level1;
    sharedPrivate.id = "innovation.reflection.leak";
    sharedPrivate.operations.at(0)["available_to"] = Json::array({"mira", "lorn"});
    expect(EvolutionManager::validate(sharedPrivate, service.content(), service.state()).code ==
               "innovation_target_mismatch",
           "Level 1 innovation cannot silently propagate a private recipe to peers");

    EvolutionProposal level2{
        "innovation.world.shared", "innovation", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "create_entity_archetype"}, {"id", "innovation_workshop"},
                         {"kind", "facility"}, {"default_metadata", Json{{"purpose", "experimentation"}}}}})};
    level2.evidenceEventSequences = {evidence};
    level2.innovation = Json{
        {"hypothesis", "A shared workshop can make successful behavior experiments reusable."},
        {"novelty_basis", "new_archetype"},
        {"operation_ids", Json::array({"innovation_workshop"})},
        {"expected_benefits", Json::array({"increase reusable experiments"})},
        {"risks", Json::array({"uses scarce construction capacity"})},
        {"test_plan", Json{{"horizon_minutes", 240}, {"metrics", Json::array({"npc_count", "events_added"})},
                             {"baseline", "NPCs use separate authored routines."},
                             {"scenario", "NPCs use a shared workshop recipe."},
                             {"expected_metrics", Json{{"npc_count", 3.0}, {"events_added", 4.0}}},
                             {"constraints", Json::array({"do not reduce information reliability"})},
                             {"resource_budget", Json{{"resource_total", 12.0}}},
                             {"success_criteria", "Shared work increases measured coordination."},
                             {"failure_criteria", "Coordination falls below the baseline."}}}};
    expect(EvolutionManager::validate(level2, service.content(), service.state()).valid,
           "Level 2 can promote a bounded innovation hypothesis into shared declarative content");
    const auto roundTrip = EvolutionManager::fromJson(EvolutionManager::toJson(level2));
    expect(roundTrip.kind == "innovation" && roundTrip.innovation == level2.innovation &&
               roundTrip.innovationStatus == "hypothesized",
           "innovation metadata and lifecycle persist through proposal storage JSON");
    auto lifecycle = level2;
    expect(EvolutionManager::transitionInnovation(lifecycle, "refining").valid &&
               EvolutionManager::transitionInnovation(lifecycle, "evaluated").valid &&
               EvolutionManager::transitionInnovation(lifecycle, "accepted").valid,
           "innovation hypotheses follow the explicit reflection-to-acceptance lifecycle");
    expect(!EvolutionManager::transitionInnovation(lifecycle, "refining").valid,
           "accepted hypotheses cannot be silently reopened by a later reflection");
}

void evolved_behavior_recipe_is_bound_to_declared_npcs() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    EvolutionProposal proposal{
        "overlay.recipe.bound", "declarative_capability", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "observe_records"},
                         {"available_to", Json::array({"eve"})}, {"plan_id", "observe_records"},
                         {"duration_minutes", 10}, {"interruptible", true},
                         {"root", Json{{"type", "meta_action"}, {"id", "observe"},
                                       {"name", "relieve_need"}, {"args", Json{{"action", "share_record"}}}}}}})};
    proposal.evidenceEventSequences = {service.state().events.back().sequence};
    const auto validation = EvolutionManager::validate(proposal, service.content(), service.state());
    expect(validation.valid, "an evolved recipe requires and validates an explicit actor propagation scope");
    const auto sandbox = EvolutionManager::sandbox(proposal, service);
    proposal.status = EvolutionProposalStatus::Validated;
    expect(sandbox.passed && EvolutionManager::approve(proposal, "developer", true).valid &&
               service.activateOverlay(proposal, sandbox).accepted,
           "an approved behavior recipe overlay activates through the normal governance path");
    const auto evePlans = service.decisionContext("eve").planCatalog;
    const auto miraPlans = service.decisionContext("mira").planCatalog;
    expect(std::any_of(evePlans.begin(), evePlans.end(), [](const auto& plan) {
               return plan.value("action_id", "") == "observe_records";
           }) &&
               std::none_of(miraPlans.begin(), miraPlans.end(), [](const auto& plan) {
                   return plan.value("action_id", "") == "observe_records";
               }),
           "an evolved behavior is visible only to the NPCs declared by the overlay");
}

void innovation_adoption_binds_process_and_measures_effects() {
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.enabled = false;
    content.evolutionPolicy.enabled = false;
    auto service = WorldService::fromContent(content, std::make_unique<MockProvider>());
    const auto evidence = service.state().events.back().sequence;
    EvolutionProposal proposal{
        "innovation.adoption.process", "innovation", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "private_reflection"},
                         {"plan_id", "private_reflection"}, {"duration_minutes", 5},
                         {"interruptible", true}, {"interrupt_salience_threshold", 1.0},
                         {"reservation_mode", "input_resources"}, {"reservations", Json{{"grain", 5}}},
                         {"priority", 1},
                         {"preemption_policy", "none"}, {"parameters", Json::object()},
                         {"available_to", Json::array({"mira"})},
                         {"root", Json{{"type", "meta_action"}, {"id", "reflect"},
                                       {"name", "relieve_need"}, {"args", Json{{"action", "rest"}}}}}}})};
    proposal.evolutionLevel = "level1";
    proposal.targetNpcId = "mira";
    proposal.evidenceEventSequences = {evidence};
    proposal.innovation = Json{
        {"hypothesis", "A short private reflection rehearsal can reduce a known deficit."},
        {"novelty_basis", "new_combination"},
        {"operation_ids", Json::array({"private_reflection"})},
        {"expected_benefits", Json::array({"measured need relief"})},
        {"risks", Json::array({"consumes five minutes"})},
        {"test_plan", Json{{"horizon_minutes", 10}, {"metrics", Json::array({"advance_minutes"})},
                             {"baseline", "Mira rests using an authored routine."},
                             {"scenario", "Mira executes the private reflection recipe."},
                             {"expected_metrics", Json{{"advance_minutes", 5.0}}},
                             {"constraints", Json::array({"do not change shared content"})},
                             {"resource_budget", Json{{"minutes", 5.0}}},
                             {"success_criteria", "The process completes within the declared horizon."},
                             {"failure_criteria", "The process does not complete."}}}};
    expect(EvolutionManager::validate(proposal, service.content(), service.state()).valid,
           "an adoption test starts from a valid private innovation proposal");
    const auto sandbox = EvolutionManager::sandbox(proposal, service);
    proposal.innovationStatus = "accepted";
    proposal.status = EvolutionProposalStatus::Validated;
    const auto approval = EvolutionManager::approve(proposal, "developer", true);
    const auto activation = approval.valid ? service.activateOverlay(proposal, sandbox)
                                           : CommandResult{};
    expect(sandbox.passed && approval.valid && activation.accepted,
           "an adoption recipe must pass sandbox and independent approval before activation");
    auto snapshot = service.snapshotJson();
    snapshot["npcs"]["mira"]["innovation_hypotheses"] =
        Json::array({EvolutionManager::toJson(proposal)});
    auto blockedSnapshot = snapshot;
    blockedSnapshot["resources"]["grain"]["amount"] = 0;
    blockedSnapshot["resources"]["grain"]["reserved"] = 0;
    auto blocked = WorldService::fromSnapshot(blockedSnapshot);
    const auto blockedStart = blocked.startInnovationAdoption("mira", proposal.id);
    expect(blockedStart.accepted && blockedStart.code == "resource_blocked" &&
               blocked.state().npcs.at("mira").innovationAdoptions.front().status ==
                   InnovationAdoptionStatus::ResourceBlocked &&
               blocked.state().npcs.at("mira").activity.status == ActivityStatus::Proposed,
           "an adoption with unavailable reservations remains resource_blocked without creating a process");
    auto restored = WorldService::fromSnapshot(snapshot);
    const auto started = restored.startInnovationAdoption("mira", proposal.id);
    expect(started.accepted && started.code == "executing" &&
               restored.state().npcs.at("mira").activity.status == ActivityStatus::Active &&
               restored.state().npcs.at("mira").activity.process.has_value(),
           "accepted innovation adoption creates a real NpcActivity ProcessExecution");
    const auto& runningAdoption = restored.state().npcs.at("mira").innovationAdoptions.front();
    expect(runningAdoption.status == InnovationAdoptionStatus::Executing &&
               !runningAdoption.processId.empty() && runningAdoption.processId ==
                   restored.state().npcs.at("mira").activity.process->id,
           "adoption and process facts share a durable process identity");
    restored.advanceMinutes(5);
    const auto& finished = restored.state().npcs.at("mira").innovationAdoptions.front();
    expect(finished.status == InnovationAdoptionStatus::Completed &&
               finished.observedMetrics.value("advance_minutes", 0) == 5 &&
               finished.metricEvaluation.value("passed", false),
           "process completion records Runtime effect metrics and evaluates the declared threshold");
    const auto roundTrip = WorldService::fromSnapshot(restored.snapshotJson());
    const auto& persisted = roundTrip.state().npcs.at("mira").innovationAdoptions.front();
    expect(persisted.status == InnovationAdoptionStatus::Completed &&
               persisted.observedMetrics == finished.observedMetrics &&
               persisted.metricEvaluation == finished.metricEvaluation,
           "adoption process identity and measured outcomes survive snapshot restore");
}

void llm_schedule_overlay_is_versioned_and_content_driven() {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto baseVersion = service.content().contentVersion;
    EvolutionProposal proposal{
        "overlay.schedule.lorn", "strategy", "llm", baseVersion, service.state().revision,
        Json::array({Json{{"operation", "overlay_schedule"}, {"id", "lorn.night.watch"},
                         {"npc_id", "lorn"},
                         {"slots", Json::array({Json{{"start_minute", 0}, {"end_minute", 360},
                                                       {"location", "lorn_house"}},
                                                  Json{{"start_minute", 360}, {"end_minute", 1080},
                                                       {"location", "ferry_dock"}},
                                                  Json{{"start_minute", 1080}, {"end_minute", 0},
                                                       {"location", "lorn_house"}}})}}})};
    proposal.evolutionLevel = "level1";
    proposal.targetNpcId = "lorn";
    proposal.evidenceEventSequences = {service.state().events.back().sequence};
    expect(EvolutionManager::validate(proposal, service.content(), service.state()).valid,
           "Level 1 schedule evolution validates only authored locations and bounded time slots");
    const auto sandbox = EvolutionManager::sandbox(proposal, service);
    proposal.status = EvolutionProposalStatus::Validated;
    expect(sandbox.passed && EvolutionManager::approve(proposal, "world_designer", true).valid &&
               service.activateOverlay(proposal, sandbox).accepted,
           "a schedule proposal follows the same sandbox and independent approval chain");
    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
    const auto lorn = std::find_if(restored.content().npcs.begin(), restored.content().npcs.end(),
                                   [](const auto& npc) { return npc.definition.id == "lorn"; });
    expect(lorn != restored.content().npcs.end() && lorn->schedule.size() == 3 &&
               restored.content().contentVersion.find("overlay.schedule.lorn") != std::string::npos,
           "the schedule overlay is persisted through the active overlay stack and snapshot restore");
}

void overlay_stack_restores_and_rolls_back_in_order() {
    auto base = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
    const auto rootVersion = base.content().contentVersion;
    EvolutionProposal first{
        "overlay.stack.first", "declarative_capability", "llm", rootVersion, base.state().revision,
        Json::array({Json{{"operation", "create_entity_archetype"}, {"id", "market_stall"},
                         {"kind", "facility"}}})};
    first.evidenceEventSequences = {base.state().events.back().sequence};
    const auto firstSandbox = EvolutionManager::sandbox(first, base);
    first.status = EvolutionProposalStatus::Validated;
    expect(firstSandbox.passed && EvolutionManager::approve(first, "developer", true).valid &&
               base.activateOverlay(first, firstSandbox).accepted,
           "the first versioned overlay activates");
    EvolutionProposal second{
        "overlay.stack.second", "declarative_capability", "llm", base.content().contentVersion,
        base.state().revision,
        Json::array({Json{{"operation", "create_entity_archetype"}, {"id", "market_awning"},
                         {"kind", "facility"}}})};
    second.evidenceEventSequences = {base.state().events.back().sequence};
    const auto secondSandbox = EvolutionManager::sandbox(second, base);
    second.status = EvolutionProposalStatus::Validated;
    expect(secondSandbox.passed && EvolutionManager::approve(second, "developer", true).valid &&
               base.activateOverlay(second, secondSandbox).accepted && base.state().activeOverlays.size() == 2,
           "a proposal binds to the exact active content view and can be layered");
    auto restored = WorldService::fromSnapshot(base.snapshotJson());
    expect(restored.state().activeOverlays.size() == 2 && restored.content().contentVersion == secondSandbox.overlayVersion,
           "the full overlay chain restores from a snapshot");
    expect(restored.rollbackOverlay(first.id).code == "overlay_not_top",
           "an older overlay cannot be removed while newer overlays depend on it");
    expect(restored.rollbackOverlay(second.id).accepted && restored.rollbackOverlay(first.id).accepted &&
               restored.state().activeOverlays.empty() && restored.content().contentVersion == rootVersion,
           "stacked overlays roll back in order after restore");
}

void llm_proposes_level_one_and_level_two_evolution() {
    auto provider = std::make_unique<EvolutionProposalProvider>();
    auto* providerRaw = provider.get();
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::move(provider));
    const auto before = service.snapshotJson();

    const auto level1 = EvolutionManager::propose(service, "level1", "test_strategy_reflection");
    expect(level1.accepted && level1.proposal.evolutionLevel == "level1" &&
               level1.proposal.source == "llm:evolution-test-llm" &&
               level1.proposal.kind == "strategy",
           "Level 1 evolution is proposed by the configured LLM and normalized with runtime context");
    expect(providerRaw->observedLevel == "level1" &&
               providerRaw->observedContext.value("base_world_revision", 0ULL) == service.state().revision,
           "the Level 1 provider receives the current world revision and evidence context");
    expect(providerRaw->observedContext.value("evolution_system_prompt", "").find("target subject") != std::string::npos,
           "Level 1 receives the world-authored subject-scoped role prompt");
    const auto& miraContext = providerRaw->observedContext.at("npcs").at("mira");
    expect(miraContext.contains("memories") && miraContext.contains("knowledge") &&
               miraContext.contains("experiences") && miraContext.contains("relationships") &&
               miraContext.contains("stimuli") && miraContext.contains("activity"),
           "evolution context includes bounded cognitive, social, external-drive, and activity evidence");

    const auto level2 = EvolutionManager::propose(service, "level2", "test_world_foundation");
    expect(level2.accepted && level2.proposal.evolutionLevel == "level2" &&
               level2.proposal.operations.at(0).value("operation", "") == "create_entity_archetype",
           "Level 2 evolution is also an LLM proposal over declarative world content");
    expect(providerRaw->observedContext.value("evolution_system_prompt", "").find("shared world substrate") != std::string::npos,
           "Level 2 receives a distinct world-substrate role prompt");
    expect(service.snapshotJson() == before,
           "asking the LLM for evolution proposals never mutates the source world");

    const auto sandbox = EvolutionManager::sandbox(level2.proposal, service);
    expect(sandbox.passed && sandbox.overlayVersion.find(level2.proposal.id) != std::string::npos,
           "an LLM proposal still requires isolated sandbox execution before activation");
}

void level_one_evolution_is_scoped_to_target_npc() {
    auto provider = std::make_unique<EvolutionProposalProvider>();
    auto* providerRaw = provider.get();
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::move(provider));
    const auto sourceSequence = service.state().events.back().sequence;
    expect(service.recordExperience("mira", "private", "peer_observation", "withhold", "kept private",
                                    sourceSequence, 0.8, true, "mira").accepted,
           "the fixture can create a committed peer-private experience for projection testing");
    const auto result = EvolutionManager::propose(service, "level1", "targeted_reflection", "lorn");
    expect(result.accepted && result.proposal.targetNpcId == "lorn",
           "Level 1 proposals retain the NPC target used to build their context");
    expect(providerRaw->observedContext.value("target_npc", "") == "lorn" &&
               providerRaw->observedContext.at("npcs").at("lorn").contains("knowledge") &&
               providerRaw->observedContext.at("npcs").at("lorn").contains("private_traits") &&
               !providerRaw->observedContext.at("npcs").at("mira").contains("knowledge") &&
               !providerRaw->observedContext.at("npcs").at("mira").contains("private_traits") &&
               !providerRaw->observedContext.at("npcs").at("mira").contains("experiences"),
           "Level 1 exposes private cognitive evidence and traits only for the target NPC");
    expect(std::none_of(providerRaw->observedContext.at("evidence_events").begin(),
                        providerRaw->observedContext.at("evidence_events").end(), [](const auto& event) {
                            return event.value("type", "") == "experience.recorded" &&
                                   event.value("actor", "") == "mira";
                        }),
           "Level 1 evidence history filters a peer's private cognitive event");

    const auto roundTrip = EvolutionManager::fromJson(EvolutionManager::toJson(result.proposal));
    expect(roundTrip.targetNpcId == "lorn" && roundTrip.evolutionLevel == "level1",
           "Level 1 target scope survives proposal JSON persistence");

    auto mismatched = result.proposal;
    mismatched.operations.at(0)["npc_id"] = "mira";
    expect(EvolutionManager::validate(mismatched, service.content(), service.state()).code ==
               "level_target_mismatch",
           "Level 1 validation rejects an operation aimed at a different NPC");

    auto level2Target = result.proposal;
    level2Target.evolutionLevel = "level2";
    expect(EvolutionManager::validate(level2Target, service.content(), service.state()).code ==
               "level_target_mismatch",
           "Level 2 validation rejects an individual NPC target");

    auto missingTarget = result.proposal;
    missingTarget.targetNpcId.clear();
    expect(EvolutionManager::validate(missingTarget, service.content(), service.state()).code ==
               "missing_target_npc",
           "Level 1 validation rejects a proposal without a target NPC");
}

void automatic_evolution_uses_async_llm_and_audits_sandbox() {
    auto provider = std::make_unique<EvolutionProposalProvider>();
    auto* providerRaw = provider.get();
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.evolutionPolicy.enabled = true;
    content.evolutionPolicy.level1IntervalDays = 1;
    content.evolutionPolicy.level2IntervalDays = 0;
    content.evolutionPolicy.maxProposalAgeMinutes = 180;
    content.npcDeliberation.enabled = false;
    auto service = WorldService::fromContent(content, std::move(provider));
    const auto toBoundary = kMinutesPerDay - (service.state().now.minute % kMinutesPerDay);
    service.advanceMinutes(toBoundary);
    service.waitForAsyncTurns(1000);
    expect(providerRaw->observedReflectionTargets.size() == 3 &&
               std::set<std::string>(providerRaw->observedReflectionTargets.begin(), providerRaw->observedReflectionTargets.end()) ==
                   std::set<std::string>{"eve", "lorn", "mira"},
           "a configured day boundary enqueues one independent daily reflection for every active NPC");
    expect(std::any_of(service.state().events.begin(), service.state().events.end(),
                       [](const auto& event) { return event.type == "reflection.committed"; }),
           "an asynchronous LLM ReflectionResult is validated, committed, and audited");
    expect(std::none_of(service.state().events.begin(), service.state().events.end(),
                        [](const auto& event) { return event.type == "evolution.overlay_activated"; }),
           "automatic evolution never self-approves or activates an overlay");

    auto level2Provider = std::make_unique<EvolutionProposalProvider>();
    auto* level2ProviderRaw = level2Provider.get();
    content.evolutionPolicy.level1IntervalDays = 0;
    content.evolutionPolicy.level2IntervalDays = 1;
    auto level2Service = WorldService::fromContent(content, std::move(level2Provider));
    level2Service.advanceMinutes(kMinutesPerDay - (level2Service.state().now.minute % kMinutesPerDay));
    level2Service.waitForAsyncTurns(1000);
    expect(level2ProviderRaw->observedLevel == "level2" &&
               std::any_of(level2Service.state().events.begin(), level2Service.state().events.end(),
                           [](const auto& event) { return event.type == "evolution.proposal_ready" &&
                                                          event.target == "level2"; }),
           "the same configured queue also drives Level 2 world-foundation proposals through the LLM");
}

void daily_reflection_routes_updates_and_keeps_incremental_cursor() {
    auto provider = std::make_unique<ReflectionProvider>();
    auto* providerRaw = provider.get();
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.evolutionPolicy.enabled = true;
    content.evolutionPolicy.level1IntervalDays = 1;
    content.evolutionPolicy.level2IntervalDays = 0;
    content.evolutionPolicy.maxProposalAgeMinutes = 180;
    content.npcDeliberation.enabled = false;
    auto service = WorldService::fromContent(content, std::move(provider));
    service.advanceMinutes(kMinutesPerDay - (service.state().now.minute % kMinutesPerDay));
    service.waitForAsyncTurns(1000);
    const auto& mira = service.state().npcs.at("mira");
    expect(mira.reflectionCursor.lastReflectionDay == 2 && mira.reflectionCursor.lastEventSequence > 0,
           "daily reflection commits an NPC cursor at the completed event boundary");
    expect(!mira.knowledge.empty() && mira.strategy == "reflective" &&
               std::find(mira.unresolvedIssues.begin(), mira.unresolvedIssues.end(), "verify the harbor signal") !=
                   mira.unresolvedIssues.end(),
           "reflection routes sourced belief and strategy updates while retaining unresolved issues");
    expect(service.commitDailyReflection("mira", ProviderResponse{}).code == "reflection_already_processed",
           "replaying a same-day reflection is idempotently rejected before provider data can mutate facts");
    const auto firstContextCount = providerRaw->reflectionContexts.size();
    expect(firstContextCount == 3, "one reflection result is requested for each NPC");

    service.advanceMinutes(kMinutesPerDay);
    service.waitForAsyncTurns(1000);
    expect(providerRaw->reflectionContexts.size() == 6,
           "each NPC receives one new reflection on the next day");
    const auto& secondContext = providerRaw->reflectionContexts.at(3);
    expect(secondContext.at("reflection_cursor").at("last_event_sequence").get<std::uint64_t>() > 0 &&
               !secondContext.at("unresolved_issues").empty(),
           "the next reflection receives the persisted cursor and unresolved issue set");
    expect(std::count_if(service.state().events.begin(), service.state().events.end(),
                         [](const auto& event) { return event.type == "reflection.committed"; }) >= 6,
           "reflection commits remain auditable across day boundaries");
    const auto worldContext = WorldEvolutionSystem::proposalContext(service, "level2", "world_reflection", Json::object());
    expect(std::all_of(worldContext.at("evidence_events").begin(), worldContext.at("evidence_events").end(),
                       [](const auto& event) {
                           return event.value("type", "") != "innovation.hypothesis_created" ||
                                  !event.at("data").contains("proposal");
                       }),
           "Level 2 receives committed hypothesis evidence without a private Level 1 proposal payload");
}

void intelligence_profile_configures_decision_budgets() {
    const auto profile = loadIntelligenceProfile(defaultIntelligenceProfilePath());
    const auto& decision = profile.purposes.at("decision");
    const auto& policies = decision.policies;
    expect(profile.id == "village-qwen35-4b-v2", "village runs from an explicit intelligence profile");
    expect(profile.providers.at("local_qwen").type == "llama_cpp" &&
               decision.providerId == "local_qwen",
           "the decision purpose references the reusable local llama.cpp provider");
    expect(!policies.at("player_dialogue").enableThinking &&
               policies.at("player_dialogue").maxTokens == 512,
           "player dialogue uses the fast answer policy after utterance understanding");
    expect(policies.at("utterance_understanding").enableThinking &&
               policies.at("utterance_understanding").reasoningBudgetTokens == 128 &&
               policies.at("utterance_understanding").maxTokens == 512,
           "utterance understanding has its own bounded reasoning policy");
    expect(policies.at("autonomous_reappraisal").enableThinking &&
               policies.at("autonomous_reappraisal").reasoningBudgetTokens == 128 &&
               policies.at("autonomous_reappraisal").maxTokens == 1024,
           "autonomous reappraisal uses its bounded thinking policy");
    expect(policies.size() == 3 && policies.contains("player_dialogue") &&
               policies.contains("utterance_understanding") && policies.contains("autonomous_reappraisal"),
           "decision owns exactly the three required channel policies");
    expect(profile.purposes.contains("decision") &&
               profile.purposes.contains("npc_evolution") &&
               profile.purposes.contains("world_evolution") &&
               profile.purposes.at("npc_evolution").providerId == "local_qwen" &&
               profile.purposes.at("world_evolution").providerId == "local_qwen",
           "the profile exposes independent purpose bindings for decisions and both evolution levels");
    expect(profile.providers.at("local_qwen").apiKeyEnv.empty() &&
               profile.purposes.at("decision").providerId == profile.purposes.at("world_evolution").providerId &&
               profile.purposes.at("npc_evolution").policies.at("proposal").reasoningBudgetTokens == 256 &&
               profile.purposes.at("world_evolution").policies.at("proposal").reasoningBudgetTokens == 512,
           "provider credentials are stored on the reusable provider and evolution has bounded purpose policies");
    if (profile.purposes.contains("reflection")) {
        expect(profile.purposes.at("reflection").policies.at("proposal").reasoningBudgetTokens == 256,
               "the current profile gives daily reflection its own bounded reasoning policy");
    }
    const auto provider = makeProviderFromProfile(profile, "mock");
    expect(provider->name() == "mock", "the profile provider can be overridden for offline tests");
}

void intelligence_profile_decouples_models_from_api_keys() {
    const auto path = std::filesystem::temp_directory_path() /
                      "tide_marks-purpose-endpoint-profile.json";
    const Json profile{
        {"kind", "intelligence_profile"},
        {"schema_version", 2},
        {"id", "purpose-endpoint-test"},
        {"providers", Json{{"shared", Json{{"type", "openai_compatible"},
                                             {"base_url", "https://shared.example/v1"},
                                             {"model", "shared-model"},
                                             {"api_key_env", "SIM_WORLD_SHARED_KEY"},
                                             {"timeout_ms", 120000}, {"temperature", 0.2}}},
                            {"evolution", Json{{"type", "openai_compatible"},
                                                {"base_url", "https://evolution.example/v1"},
                                                {"model", "evolution-model"},
                                                {"api_key_env", "SIM_WORLD_EVOLUTION_KEY"},
                                                {"timeout_ms", 120000}, {"temperature", 0.3}}}}},
        {"purposes", Json{{"decision", Json{{"provider", "shared"},
                                             {"model", "decision-model"},
                                             {"api_key_env", "SIM_WORLD_DECISION_KEY"},
                                             {"policies", Json{{"player_dialogue", Json{{"thinking", false}, {"max_tokens", 64}}},
                                                                  {"utterance_understanding", Json{{"thinking", false}, {"max_tokens", 64}}},
                                                                  {"autonomous_reappraisal", Json{{"thinking", false}, {"max_tokens", 64}}}}}}},
                           {"npc_evolution", Json{{"provider", "evolution"},
                                                   {"api_key_env", nullptr},
                                                   {"policy", Json{{"thinking", false}, {"max_tokens", 64}}}}},
                           {"world_evolution", Json{{"provider", "shared"},
                                                     {"base_url", "https://other.example/v1"},
                                                     {"policy", Json{{"thinking", false}, {"max_tokens", 64}}}}}}}};
    {
        std::ofstream output(path);
        output << profile.dump(2) << '\n';
    }
    const auto loaded = loadIntelligenceProfile(path.string());
    expect(loaded.purposes.at("decision").providerId == "shared" &&
               loaded.purposes.at("decision").model == "decision-model" &&
               loaded.purposes.at("decision").apiKeyEnv == "SIM_WORLD_DECISION_KEY",
           "a purpose can reference a provider and override its model and API-key environment variable independently");
    expect(loaded.purposes.at("npc_evolution").providerId == "evolution" &&
               loaded.purposes.at("npc_evolution").apiKeyConfigured &&
               loaded.purposes.at("npc_evolution").apiKeyEnv.empty(),
           "an explicit null API-key override disables the referenced provider credential");
    expect(loaded.purposes.at("world_evolution").providerId == "shared" &&
               loaded.purposes.at("world_evolution").baseUrl == "https://other.example/v1",
           "a purpose can reuse a provider while overriding only its endpoint");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void intelligence_profile_rejects_legacy_layout() {
    const auto path = std::filesystem::temp_directory_path() /
                      "tide_marks-legacy-intelligence-profile.json";
    Json legacy = Json::object();
    legacy["kind"] = "intelligence_profile";
    legacy["schema_version"] = 2;
    legacy["id"] = "legacy-layout";
    legacy["providers"] = Json{{"local", Json{{"type", "mock"}}}};
    legacy["purposes"] = Json::object();
    legacy["purposes"]["decision"] = Json::object();
    legacy["purposes"]["decision"]["provider"] = "local";
    legacy["purposes"]["decision"]["policies"] = Json{
        {"player_dialogue", Json{{"thinking", false}, {"max_tokens", 64}}},
        {"utterance_understanding", Json{{"thinking", false}, {"max_tokens", 64}}},
        {"autonomous_reappraisal", Json{{"thinking", false}, {"max_tokens", 64}}}};
    for (const auto purpose : {"npc_evolution", "world_evolution"}) {
        legacy["purposes"][purpose] = Json{{"provider", "local"},
                                             {"policy", Json{{"thinking", false}, {"max_tokens", 64}}}};
    }
    legacy["decision_policy"] = Json{{"player_dialogue", Json{{"thinking", false}, {"max_tokens", 64}}}};
    {
        std::ofstream output(path);
        output << legacy.dump(2) << '\n';
    }
    bool rejected = false;
    try {
        (void)loadIntelligenceProfile(path.string());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "schema 2 rejects the legacy top-level decision_policy field");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

int main() {
    const auto run = [](const char* name, auto&& test) {
        const auto started = std::chrono::steady_clock::now();
        std::cerr << "RUN " << name << '\n';
        test();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "DONE " << name << " (" << elapsed << "s)\n";
    };
    run("village_has_three_independent_npcs", village_has_three_independent_npcs);
    run("goal_ranking_aggregates_all_satisfied_needs", goal_ranking_aggregates_all_satisfied_needs);
    run("drive_confidence_is_applied_once", drive_confidence_is_applied_once);
    run("private_traits_change_stable_action_order", private_traits_change_stable_action_order);
    run("authored_utility_profile_is_bounded_and_auditable",
        authored_utility_profile_is_bounded_and_auditable);
    run("offline_progression_changes_village", offline_progression_changes_village);
    run("needs_are_data_driven_and_replayable", needs_are_data_driven_and_replayable);
    run("accepted_intent_runs_without_reasking_llm", accepted_intent_runs_without_reasking_llm);
    run("autonomous_deliberation_is_triggered_not_per_tick", autonomous_deliberation_is_triggered_not_per_tick);
    run("async_turn_queue_does_not_commit_expired_results", async_turn_queue_does_not_commit_expired_results);
    run("async_turn_queue_commits_results_with_current_context",
        async_turn_queue_commits_results_with_current_context);
    run("independent_autonomous_turns_survive_unrelated_commits",
        independent_autonomous_turns_survive_unrelated_commits);
    run("async_llm_action_validation_retries_without_blocking_world_tick",
        async_llm_action_validation_retries_without_blocking_world_tick);
    run("async_turn_queue_coalesces_pending_requests_per_npc_and_channel",
        async_turn_queue_coalesces_pending_requests_per_npc_and_channel);
    run("async_turn_queue_rejects_decisions_past_game_deadline",
        async_turn_queue_rejects_decisions_past_game_deadline);
    run("time_step_partitioning_preserves_deterministic_world_state",
        time_step_partitioning_preserves_deterministic_world_state);
    run("high_salience_stimulus_interrupts_and_releases_reservations", high_salience_stimulus_interrupts_and_releases_reservations);
    run("reservation_policy_is_authored_per_process", reservation_policy_is_authored_per_process);
    run("resource_preemption_is_declared_and_atomic", resource_preemption_is_declared_and_atomic);
    run("exchange_contracts_are_durable_and_atomic", exchange_contracts_are_durable_and_atomic);
    run("behavior_tree_supports_sequence_timeout_and_interrupt", behavior_tree_supports_sequence_timeout_and_interrupt);
    run("provider_and_action_validation_are_safe", provider_and_action_validation_are_safe);
    run("decision_context_projects_location_relevant_resources",
        decision_context_projects_location_relevant_resources);
    run("decision_context_projects_only_startable_behavior_plans",
        decision_context_projects_only_startable_behavior_plans);
    run("decision_context_obeys_authored_visibility_budgets",
        decision_context_obeys_authored_visibility_budgets);
    run("action_validator_rejects_malformed_nested_schema_without_throwing",
        action_validator_rejects_malformed_nested_schema_without_throwing);
    run("action_validator_enforces_unique_array_parameters",
        action_validator_enforces_unique_array_parameters);
    run("action_validator_enforces_recursive_schema_constraints",
        action_validator_enforces_recursive_schema_constraints);
    run("llm_action_validation_returns_bounded_repair_feedback",
        llm_action_validation_returns_bounded_repair_feedback);
    run("llm_expected_relief_validation_returns_bounded_repair_feedback",
        llm_expected_relief_validation_returns_bounded_repair_feedback);
    run("meta_actions_are_loaded_from_content", meta_actions_are_loaded_from_content);
    run("needs_cover_domains_and_explicit_execution_paths", needs_cover_domains_and_explicit_execution_paths);
    run("bounded_drive_expectation_adapts_from_outcomes", bounded_drive_expectation_adapts_from_outcomes);
    run("external_stimuli_and_structures_are_observable_and_persistent", external_stimuli_and_structures_are_observable_and_persistent);
    run("promise_policy_comes_from_content", promise_policy_comes_from_content);
    run("promises_cover_accept_renegotiate_refuse_and_break", promises_cover_accept_renegotiate_refuse_and_break);
    run("snapshot_and_replay_are_deterministic", snapshot_and_replay_are_deterministic);
    run("persistence_adapter_receives_committed_representations",
        persistence_adapter_receives_committed_representations);
    run("json_file_persistence_creates_run_directories", json_file_persistence_creates_run_directories);
    run("durable_checkpoint_policy_is_boundary_driven_and_opt_in",
        durable_checkpoint_policy_is_boundary_driven_and_opt_in);
    run("sqlite_persistence_roundtrips_versioned_artifacts",
        sqlite_persistence_roundtrips_versioned_artifacts);
    run("sqlite_instance_imports_content_and_restores_without_source_root",
        sqlite_instance_imports_content_and_restores_without_source_root);
    run("event_history_has_causal_identity", event_history_has_causal_identity);
    run("physics_events_are_deterministic_and_drained", physics_events_are_deterministic_and_drained);
    run("event_bus_is_ordered_and_filterable", event_bus_is_ordered_and_filterable);
    run("world_service_publishes_committed_events", world_service_publishes_committed_events);
    run("global_and_local_planners_are_deterministic", global_and_local_planners_are_deterministic);
    run("npc_state_machine_handles_interrupts", npc_state_machine_handles_interrupts);
    run("entity_registry_accepts_feature_components_without_core_changes", entity_registry_accepts_feature_components_without_core_changes);
    run("entity_projection_tracks_committed_npc_movement", entity_projection_tracks_committed_npc_movement);
    run("world_entity_lifecycle_is_transactional_and_persistent", world_entity_lifecycle_is_transactional_and_persistent);
    run("resource_transfer_is_atomic_and_persistent", resource_transfer_is_atomic_and_persistent);
    run("movement_operator_obeys_authored_topology", movement_operator_obeys_authored_topology);
    run("behavior_plan_resolves_dynamic_targets_through_runtime", behavior_plan_resolves_dynamic_targets_through_runtime);
    run("conflict_and_security_operators_change_facts_then_stimuli", conflict_and_security_operators_change_facts_then_stimuli);
    run("knowledge_and_experience_keep_provenance", knowledge_and_experience_keep_provenance);
    run("successful_actions_automatically_create_experience", successful_actions_automatically_create_experience);
    run("extended_meta_actions_execute_from_content_recipes", extended_meta_actions_execute_from_content_recipes);
    run("exchange_meta_actions_execute_from_scheduled_activity",
        exchange_meta_actions_execute_from_scheduled_activity);
    run("runtime_loads_a_world_without_village_identifiers", runtime_loads_a_world_without_village_identifiers);
    run("social_groups_trade_and_relationships_are_shared_facts", social_groups_trade_and_relationships_are_shared_facts);
    run("port_slice_is_a_replaceable_social_instance", port_slice_is_a_replaceable_social_instance);
    run("glass_orchard_is_a_second_replaceable_world_instance",
        glass_orchard_is_a_second_replaceable_world_instance);
    run("offline_scheduler_baseline_is_measurable", offline_scheduler_baseline_is_measurable);
    run("world_intelligence_detects_anomalies_and_runs_counterfactuals",
        world_intelligence_detects_anomalies_and_runs_counterfactuals);
    run("npc_activity_slot_is_single_owner", npc_activity_slot_is_single_owner);
    run("evolution_proposals_are_governed", evolution_proposals_are_governed);
    run("evolution_sandbox_isolated_and_versioned", evolution_sandbox_isolated_and_versioned);
    run("evolved_behavior_recipe_is_bound_to_declared_npcs",
        evolved_behavior_recipe_is_bound_to_declared_npcs);
    run("innovation_adoption_binds_process_and_measures_effects",
        innovation_adoption_binds_process_and_measures_effects);
    run("llm_schedule_overlay_is_versioned_and_content_driven",
        llm_schedule_overlay_is_versioned_and_content_driven);
    run("overlay_stack_restores_and_rolls_back_in_order", overlay_stack_restores_and_rolls_back_in_order);
    run("llm_proposes_level_one_and_level_two_evolution",
        llm_proposes_level_one_and_level_two_evolution);
    run("level_one_evolution_is_scoped_to_target_npc",
        level_one_evolution_is_scoped_to_target_npc);
    run("automatic_evolution_uses_async_llm_and_audits_sandbox",
        automatic_evolution_uses_async_llm_and_audits_sandbox);
    run("daily_reflection_routes_updates_and_keeps_incremental_cursor",
        daily_reflection_routes_updates_and_keeps_incremental_cursor);
    run("intelligence_profile_configures_decision_budgets", intelligence_profile_configures_decision_budgets);
    run("intelligence_profile_decouples_models_from_api_keys",
        intelligence_profile_decouples_models_from_api_keys);
    run("intelligence_profile_rejects_legacy_layout", intelligence_profile_rejects_legacy_layout);
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Tide Marks core tests passed\n";
    return EXIT_SUCCESS;
}
