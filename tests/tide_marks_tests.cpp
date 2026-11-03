#include "tide_marks.hpp"
#include "npc/drive_system.hpp"
#include "world/meta_actions.hpp"
#include "evolution/evolution.hpp"

#if defined(TIDE_MARKS_HAS_BULLET)
#include "physics/bullet_physics.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

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

void village_has_three_independent_npcs() {
    const auto service = WorldService::load();
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
}

void offline_progression_changes_village() {
    auto service = WorldService::load();
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
    auto service = WorldService::load();
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

void autonomous_deliberation_is_triggered_not_per_tick() {
    int providerCalls = 0;
    auto content = ContentPackLoader::load(ContentPackLoader::defaultRoot());
    content.npcDeliberation.minStimulusSalience = 0.0;
    content.npcDeliberation.minDrivePressure = 0.0;
    content.npcDeliberation.cooldownMinutes = 360;
    auto service = WorldService::fromContent(content, std::make_unique<CountingProvider>(providerCalls));
    service.advanceMinutes(60);
    expect(providerCalls == 3, "a high-pressure hourly boundary triggers one autonomous deliberation per eligible NPC");
    service.advanceMinutes(60);
    expect(providerCalls == 3, "ordinary tick progression does not repeatedly call the model");
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
    service.advanceMinutes(30);
    expect(service.state().resources.at("grain").amount == 95,
           "validated ration action changes grain atomically");
    const auto duplicate = service.converse("mira", "again", "dialogue-1");
    expect(!duplicate.accepted && duplicate.error == "duplicate_turn", "duplicate turn is idempotent");
    const auto unknown = service.submit(Command{"bad-command", "unknown", "mira", {}, {}, {}});
    expect(!unknown.accepted && unknown.code == "unknown_command", "unknown command is rejected");
    const auto stale = service.submit(Command{"stale", "advance", "mira", {}, {}, "1", 1, 1});
    expect(!stale.accepted && stale.code == "state_conflict", "stale world revisions are rejected atomically");
}

void meta_action_registry_distinguishes_design_from_execution() {
    expect(MetaActionRegistry::contains("transfer_resource"),
           "reservation remains in the design registry");
    expect(!MetaActionRegistry::isImplemented("transfer_resource"),
           "a design-only meta action is not executable content");
    expect(MetaActionRegistry::isImplemented("consume_resource"),
           "resource consumption has a runtime executor");
    expect(MetaActionRegistry::isImplemented("relieve_need"),
           "need relief has an explicit runtime executor");
    expect(MetaActionRegistry::isImplemented("set_link_state"),
           "authored structural transitions have a runtime executor");
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
    auto service = WorldService::load();
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
    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
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
    auto service = WorldService::fromContent(content);
    const double before = service.state().npcs.at("mira").playerRelation.trust;
    const auto made = service.createPromise("player", "mira", "bring a ledger", 1, "policy-create");
    expect(made.accepted, "policy test creates a promise");
    expect(service.decidePromise(made.message, PromiseStatus::Accepted, "mira", "policy-accept").accepted,
           "policy test accepts the promise");
    const double after = service.state().npcs.at("mira").playerRelation.trust;
    expect(after > before + 0.49 && after < before + 0.51,
           "relationship change follows the authored promise policy");
    const auto dueBefore = service.state().promises.at(made.message).due.minute;
    expect(service.decidePromise(made.message, PromiseStatus::Renegotiated, "player", "policy-renegotiate").accepted,
           "policy test renegotiates the promise");
    expect(service.state().promises.at(made.message).due.minute == dueBefore + 60,
           "renegotiation extension follows the authored policy");
}

void promises_cover_accept_renegotiate_refuse_and_break() {
    auto service = WorldService::load();
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
    auto service = WorldService::load();
    service.converse("eve", "Tell me about the record", "replay-dialogue");
    service.advanceMinutes(2 * kMinutesPerDay);
    const auto path = std::filesystem::temp_directory_path() / "tide_marks_test_replay.jsonl";
    service.writeReplay(path.string());
    const auto snapshot = service.snapshotJson();
    expect(snapshot.at("save_schema_version").get<int>() == 10, "snapshot declares its schema version");
    expect(snapshot.at("instance_id") == "tide_marks.village.instance",
           "snapshot records the concrete game instance identity");
    expect(snapshot.at("event_sequence").get<std::size_t>() == service.state().events.size(),
           "snapshot records its event-log cutoff");
    const auto& savedNpc = snapshot.at("npcs").at("mira");
    expect(savedNpc.contains("definition_id") && !savedNpc.contains("definition"),
           "snapshot stores a content reference instead of a definition copy");
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
    expect(summarize(service.state()) == summarize(replayed.state()), "replay restores identical final state");
    std::filesystem::remove(path);
    const auto restored = WorldService::fromSnapshot(service.snapshotJson());
    expect(summarize(service.state()) == summarize(restored.state()), "snapshot restores identical state");
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
    auto service = WorldService::load();
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

    auto submitted = WorldService::load();
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
    auto service = WorldService::load();
    service.advanceMinutes(6 * kMinutesPerHour);
    const auto* location = service.entities().get<LocationComponent>("npc:mira");
    expect(location != nullptr && location->locationId == service.state().npcs.at("mira").location,
           "NPC component projection matches committed world state");
}

void world_entity_lifecycle_is_transactional_and_persistent() {
    auto service = WorldService::load();
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

void runtime_loads_a_world_without_village_identifiers() {
    const std::filesystem::path fixtureRoot = std::filesystem::path(TIDE_MARKS_TEST_DATA_DIR) / "minimal-pack";
    auto service = WorldService::load(fixtureRoot.string());
    expect(service.state().worldId == "fixture_world", "world identity comes from the content pack");
    expect(service.state().instanceId == "fixture.minimal.instance", "replacement instance identity is preserved");
    expect(service.state().now == GameTime::fromDayHour(4, 9), "world clock comes from the content pack");
    expect(service.state().npcs.contains("sable") && !service.state().npcs.contains("mira"),
           "world loads arbitrary authored NPCs without village identifiers");
    expect(service.state().npcs.at("sable").definitionId == "sable",
           "runtime NPC state stores the authored definition id");
    expect(service.content().instance && service.content().instance->id == "fixture.minimal.instance",
           "replacement world is launched through its own game instance");
    const auto fixtureSnapshot = service.snapshotJson();
    expect(fixtureSnapshot.at("npcs").at("sable").at("definition_id") == "sable" &&
               !fixtureSnapshot.at("npcs").at("sable").contains("definition"),
           "replacement world snapshot contains only its content reference");
    expect(service.state().npcs.at("sable").needs.contains("safety"),
           "arbitrary authored NPC needs load from JSON content");
    const auto initialResources = service.state().resources;
    expect(initialResources.contains("energy") && initialResources.contains("coolant") &&
               !initialResources.contains("grain"),
           "arbitrary world resources load from JSON without village-specific fields");
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

}  // namespace

void npc_activity_slot_is_single_owner() {
    auto service = WorldService::load();
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
    auto completed = WorldService::fromSnapshot(service.snapshotJson());
    completed.advanceMinutes(30);
    const auto& after = completed.state().npcs.at("mira").activity;
    expect(after.status == ActivityStatus::Proposed && !after.goal && !after.process,
           "completion clears the single activity slot");
    expect(completed.state().npcs.at("mira").mode != "executing",
           "FSM leaves executing mode after completion");
}

void evolution_proposals_are_governed() {
    const auto service = WorldService::load();
    EvolutionProposal proposal{
        "proposal.recipe.1", "declarative_capability", "llm", service.content().contentVersion,
        service.state().revision,
        Json::array({Json{{"operation", "register_behavior_recipe"}, {"id", "assist_neighbor"},
                         {"meta_actions", Json::array({"relieve_need"})}}})};
    const auto valid = EvolutionManager::validate(proposal, service.content(), service.state().revision);
    expect(valid.valid, "declarative evolution proposal passes structural validation");
    const auto approved = EvolutionManager::approve(proposal, "developer", true);
    expect(approved.valid && proposal.status == EvolutionProposalStatus::Approved,
           "only an approved sandbox result activates a proposal");
    EvolutionProposal forbidden = proposal;
    forbidden.status = EvolutionProposalStatus::Proposed;
    forbidden.operations = Json::array({Json{{"operation", "inject_code"}, {"id", "bad"}}});
    expect(!EvolutionManager::validate(forbidden, service.content(), service.state().revision).valid,
           "Runtime Core mutation is rejected before approval");
    expect(!EvolutionManager::approve(forbidden, "llm", true).valid,
           "LLM cannot approve its own evolution proposal");
}

int main() {
    village_has_three_independent_npcs();
    offline_progression_changes_village();
    needs_are_data_driven_and_replayable();
    accepted_intent_runs_without_reasking_llm();
    autonomous_deliberation_is_triggered_not_per_tick();
    high_salience_stimulus_interrupts_and_releases_reservations();
    behavior_tree_supports_sequence_timeout_and_interrupt();
    provider_and_action_validation_are_safe();
    meta_action_registry_distinguishes_design_from_execution();
    needs_cover_domains_and_explicit_execution_paths();
    bounded_drive_expectation_adapts_from_outcomes();
    external_stimuli_and_structures_are_observable_and_persistent();
    promise_policy_comes_from_content();
    promises_cover_accept_renegotiate_refuse_and_break();
    snapshot_and_replay_are_deterministic();
    physics_events_are_deterministic_and_drained();
    event_bus_is_ordered_and_filterable();
    world_service_publishes_committed_events();
    global_and_local_planners_are_deterministic();
    npc_state_machine_handles_interrupts();
    entity_registry_accepts_feature_components_without_core_changes();
    entity_projection_tracks_committed_npc_movement();
    world_entity_lifecycle_is_transactional_and_persistent();
    runtime_loads_a_world_without_village_identifiers();
    npc_activity_slot_is_single_owner();
    evolution_proposals_are_governed();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Tide Marks core tests passed\n";
    return EXIT_SUCCESS;
}
