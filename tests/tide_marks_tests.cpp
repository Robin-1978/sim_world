#include "tide_marks.hpp"

#if defined(TIDE_MARKS_HAS_BULLET)
#include "physics/bullet_physics.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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

void village_has_three_independent_npcs() {
    const auto service = WorldService::load();
    expect(service.state().npcs.size() == 3, "village has exactly three important NPCs");
    expect(service.state().npcs.at("mira").definition.hiddenGoal != service.state().npcs.at("lorn").definition.hiddenGoal,
           "NPC hidden goals are distinct");
    expect(service.state().npcs.at("mira").location == "granary", "Mira starts at the granary");
    const auto ranking = GoalSelector::rank(service.state().npcs.at("mira"), service.state().village, service.state().now);
    expect(!ranking.empty() && ranking.front().score >= ranking.back().score, "GoalSelector ranks authored goals");
}

void offline_progression_changes_village() {
    auto service = WorldService::load();
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
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::make_unique<MockProvider>());
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
    auto service = WorldService::load();
    service.converse("eve", "Tell me about the record", "replay-dialogue");
    service.advanceMinutes(2 * 1440);
    const auto path = std::filesystem::temp_directory_path() / "tide_marks_test_replay.jsonl";
    service.writeReplay(path.string());
    const auto snapshot = service.snapshotJson();
    expect(snapshot.at("save_schema_version").get<int>() == 2, "snapshot declares its schema version");
    expect(snapshot.at("event_sequence").get<std::size_t>() == service.state().events.size(),
           "snapshot records its event-log cutoff");
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
    std::int64_t observedMinute = -1;
    const auto token = service.eventBus().subscribe("life.day", [&service, &delivered, &observedMinute](const WorldEvent&) {
        ++delivered;
        observedMinute = service.state().now.minute;
    });
    (void)token;
    service.advanceMinutes(2 * 1440);
    expect(delivered == 2, "WorldService publishes each committed day event");
    expect(observedMinute == service.state().now.minute,
           "events are published only after the complete transaction is committed");
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
    service.advanceMinutes(6 * 60);
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
    expect(service.state().now == GameTime::fromDayHour(4, 9), "world clock comes from the content pack");
    expect(service.state().npcs.contains("sable") && !service.state().npcs.contains("mira"),
           "world loads arbitrary authored NPCs without village identifiers");
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

int main() {
    village_has_three_independent_npcs();
    offline_progression_changes_village();
    behavior_tree_supports_sequence_timeout_and_interrupt();
    provider_and_action_validation_are_safe();
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
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Tide Marks core tests passed\n";
    return EXIT_SUCCESS;
}
