#include "tide_marks.hpp"

#if defined(TIDE_MARKS_HAS_BULLET)
#include "physics/bullet_physics.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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

void village_has_three_independent_npcs() {
    const auto service = WorldService::createVillage();
    expect(service.state().npcs.size() == 3, "village has exactly three important NPCs");
    expect(service.state().npcs.at("mira").definition.hiddenGoal != service.state().npcs.at("lorn").definition.hiddenGoal,
           "NPC hidden goals are distinct");
    expect(service.state().npcs.at("mira").location == "granary", "Mira starts at the granary");
    const auto ranking = GoalSelector::rank(service.state().npcs.at("mira"), service.state().village, service.state().now);
    expect(!ranking.empty() && ranking.front().score >= ranking.back().score, "GoalSelector ranks authored goals");
}

void offline_progression_changes_village() {
    auto service = WorldService::createVillage();
    const auto before = summarize(service.state());
    service.advanceMinutes(2 * 1440);
    const auto after = summarize(service.state());
    expect(before != after, "two days change the deterministic world");
    int changed = 0;
    for (const auto& [id, npc] : service.state().npcs) {
        if (npc.goalStage > 0 || npc.stateVersion > 0 || npc.resource != 0) ++changed;
    }
    expect(changed >= 2, "at least two NPCs changed while player was away");
    expect(service.state().events.size() >= 9, "life events are recorded");
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
    auto service = WorldService::createVillage(std::make_unique<MockProvider>());
    const auto turn = service.converse("mira", "Can you spare a ration?", "dialogue-1");
    expect(turn.accepted, "mock provider returns a valid candidate action");
    expect(service.state().village.grain == 95, "validated ration action changes grain atomically");
    const auto duplicate = service.converse("mira", "again", "dialogue-1");
    expect(!duplicate.accepted && duplicate.error == "duplicate_turn", "duplicate turn is idempotent");
    const auto unknown = service.submit(Command{"bad-command", "unknown", "mira", {}, {}, {}});
    expect(!unknown.accepted && unknown.code == "unknown_command", "unknown command is rejected");
    const auto stale = service.submit(Command{"stale", "advance", "mira", {}, {}, "1", 1, 1});
    expect(!stale.accepted && stale.code == "state_conflict", "stale world revisions are rejected atomically");
}

void promises_cover_accept_renegotiate_refuse_and_break() {
    auto service = WorldService::createVillage();
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
    service.advanceMinutes(3 * 1440);
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
    auto service = WorldService::createVillage();
    service.converse("eve", "Tell me about the record", "replay-dialogue");
    service.advanceMinutes(2 * 1440);
    const auto path = std::filesystem::temp_directory_path() / "tide_marks_test_replay.jsonl";
    service.writeReplay(path.string());
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

}  // namespace

int main() {
    village_has_three_independent_npcs();
    offline_progression_changes_village();
    behavior_tree_supports_sequence_timeout_and_interrupt();
    provider_and_action_validation_are_safe();
    promises_cover_accept_renegotiate_refuse_and_break();
    snapshot_and_replay_are_deterministic();
    physics_events_are_deterministic_and_drained();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Tide Marks core tests passed\n";
    return EXIT_SUCCESS;
}
