#include "world/world_state_json.hpp"

#include <stdexcept>

namespace tide_marks {
namespace {

template <typename T>
T jsonValue(const Json& value, const char* key, T fallback) {
    if (!value.contains(key) || value.at(key).is_null()) return fallback;
    return value.at(key).get<T>();
}

Json memoryJson(const Memory& memory) {
    return Json{{"kind", memory.kind}, {"text", memory.text}, {"minute", memory.at.minute},
                {"confidence", memory.confidence}};
}

Memory memoryFromJson(const Json& json) {
    return Memory{jsonValue<std::string>(json, "kind", "fact"), jsonValue<std::string>(json, "text", ""),
                  GameTime{jsonValue<std::int64_t>(json, "minute", 0)},
                  jsonValue<double>(json, "confidence", 1.0)};
}

Json npcDefinitionJson(const NpcDefinition& definition) {
    return Json{{"id", definition.id}, {"name", definition.name}, {"role", definition.role},
                {"home", definition.home}, {"workplace", definition.workplace},
                {"public_goal", definition.publicGoal}, {"hidden_goal", definition.hiddenGoal},
                {"boundary", definition.boundary}, {"voice", definition.voice}};
}

Json needStateJson(const NeedState& state) {
    return Json{{"deficit", state.deficit}, {"last_updated", state.lastUpdated.minute},
                {"last_relief_revision", state.lastReliefRevision}};
}

NeedState needStateFromJson(const Json& json) {
    return NeedState{jsonValue<double>(json, "deficit", 0.0),
                     GameTime{jsonValue<std::int64_t>(json, "last_updated", 0)},
                     jsonValue<std::uint64_t>(json, "last_relief_revision", 0)};
}

NpcDefinition npcDefinitionFromJson(const Json& json) {
    return NpcDefinition{jsonValue<std::string>(json, "id", ""), jsonValue<std::string>(json, "name", ""),
                         jsonValue<std::string>(json, "role", ""), jsonValue<std::string>(json, "home", ""),
                         jsonValue<std::string>(json, "workplace", ""),
                         jsonValue<std::string>(json, "public_goal", ""),
                         jsonValue<std::string>(json, "hidden_goal", ""),
                         jsonValue<std::string>(json, "boundary", ""), jsonValue<std::string>(json, "voice", "")};
}

WorldEvent worldEventFromJson(const Json& json) {
    return WorldEvent{jsonValue<int>(json, "schema_version", 1), jsonValue<std::uint64_t>(json, "sequence", 0),
                      GameTime{jsonValue<std::int64_t>(json, "minute", 0)}, jsonValue<std::string>(json, "type", "unknown"),
                      jsonValue<std::string>(json, "actor", ""), jsonValue<std::string>(json, "target", ""),
                      jsonValue<std::string>(json, "detail", ""), jsonValue<std::uint64_t>(json, "world_revision", 0),
                      json.value("data", Json::object())};
}

}  // namespace

Json worldEventToJson(const WorldEvent& event) {
    return Json{{"schema_version", event.schemaVersion}, {"sequence", event.sequence},
                {"minute", event.at.minute}, {"type", event.type}, {"actor", event.actor},
                {"target", event.target}, {"detail", event.detail},
                {"world_revision", event.worldRevision}, {"data", event.data}};
}

Json worldStateToJson(const WorldState& state) {
    Json entities = Json::object();
    for (const auto& [id, entity] : state.entities) {
        entities[id] = Json{{"id", entity.id}, {"kind", entity.kind},
                            {"lifecycle", toString(entity.lifecycle)}, {"metadata", entity.metadata}};
    }
    Json links = Json::object();
    for (const auto& [id, link] : state.links) {
        links[id] = Json{{"traversable", link.traversable}, {"locked", link.locked}};
    }
    Json npcs = Json::object();
    for (const auto& [id, npc] : state.npcs) {
        Json memories = Json::array();
        for (const auto& memory : npc.memories) memories.push_back(memoryJson(memory));
        Json peerTrust = Json::object();
        for (const auto& [peer, trust] : npc.peerTrust) peerTrust[peer] = trust;
        Json needs = Json::object();
        for (const auto& [needId, need] : npc.needs) needs[needId] = needStateJson(need);
        npcs[id] = Json{{"definition", npcDefinitionJson(npc.definition)}, {"location", npc.location},
                        {"schedule_label", npc.scheduleLabel}, {"goal_stage", npc.goalStage},
                        {"needs", needs},
                        {"relation", {{"trust", npc.playerRelation.trust}, {"respect", npc.playerRelation.respect},
                                       {"fear", npc.playerRelation.fear}, {"owed", npc.playerRelation.owed}}},
                        {"memories", memories}, {"open_promises", npc.openPromises},
                        {"state_version", npc.stateVersion}, {"mode", npc.mode}, {"strategy", npc.strategy},
                        {"peer_trust", peerTrust}};
    }
    Json promises = Json::object();
    for (const auto& [id, promise] : state.promises) {
        promises[id] = Json{{"id", promise.id}, {"from", promise.from}, {"to", promise.to},
                            {"description", promise.description}, {"due", promise.due.minute},
                            {"status", toString(promise.status)}, {"version", promise.version}};
    }
    Json events = Json::array();
    for (const auto& event : state.events) events.push_back(worldEventToJson(event));
    Json resources = Json::object();
    for (const auto& [id, value] : state.resources) resources[id] = Json{{"amount", value.amount}, {"reserved", value.reserved}};
    return Json{{"save_schema_version", 5}, {"simulation_version", "adaptive-runtime-p0.5"},
                {"event_sequence", state.events.size()}, {"world_id", state.worldId},
                {"content_version", state.contentVersion}, {"now", state.now.minute},
                {"revision", state.revision},
                {"resources", resources},
                {"entities", entities}, {"links", links}, {"npcs", npcs}, {"promises", promises}, {"events", events},
                {"processed_turn_ids", state.processedTurnIds}};
}

WorldState deserializeWorldState(const Json& json) {
    WorldState state;
    state.worldId = jsonValue<std::string>(json, "world_id", "world");
    state.contentVersion = jsonValue<std::string>(json, "content_version", "adaptive-runtime-p0.4");
    state.now = GameTime{jsonValue<std::int64_t>(json, "now", 0)};
    state.revision = jsonValue<std::uint64_t>(json, "revision", 0);
    const auto resources = json.value("resources", Json::object());
    if (!resources.is_object()) throw std::runtime_error("snapshot resources must be an object");
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        state.resources[it.key()] =
            ResourceState{jsonValue<int>(it.value(), "amount", 0), jsonValue<int>(it.value(), "reserved", 0)};
    }
    const auto entities = json.value("entities", Json::object());
    for (auto it = entities.begin(); it != entities.end(); ++it) {
        const auto value = it.value();
        state.entities[it.key()] = WorldEntityState{
            jsonValue<std::string>(value, "id", it.key()), jsonValue<std::string>(value, "kind", "world_object"),
            entityLifecycleFromString(jsonValue<std::string>(value, "lifecycle", "active")),
            value.value("metadata", Json::object())};
    }
    const auto links = json.value("links", Json::object());
    for (auto it = links.begin(); it != links.end(); ++it) {
        state.links[it.key()] = RuntimeLinkState{jsonValue<bool>(it.value(), "traversable", true),
                                                  jsonValue<bool>(it.value(), "locked", false)};
    }
    const auto npcs = json.value("npcs", Json::object());
    for (auto it = npcs.begin(); it != npcs.end(); ++it) {
        const auto value = it.value();
        NpcState npc;
        npc.definition = npcDefinitionFromJson(value.value("definition", Json::object()));
        npc.location = jsonValue<std::string>(value, "location", "");
        npc.scheduleLabel = jsonValue<std::string>(value, "schedule_label", "");
        npc.goalStage = jsonValue<int>(value, "goal_stage", 0);
        const auto needs = value.value("needs", Json::object());
        for (auto need = needs.begin(); need != needs.end(); ++need) {
            npc.needs[need.key()] = needStateFromJson(need.value());
        }
        const auto relation = value.value("relation", Json::object());
        npc.playerRelation = RelationshipAxes{jsonValue<double>(relation, "trust", 0.35),
                                              jsonValue<double>(relation, "respect", 0.35),
                                              jsonValue<double>(relation, "fear", 0.0),
                                              jsonValue<double>(relation, "owed", 0.0)};
        for (const auto& memory : value.value("memories", Json::array())) npc.memories.push_back(memoryFromJson(memory));
        npc.openPromises = value.value("open_promises", std::vector<std::string>{});
        npc.stateVersion = jsonValue<std::uint64_t>(value, "state_version", 0);
        npc.mode = jsonValue<std::string>(value, "mode", "working");
        npc.strategy = jsonValue<std::string>(value, "strategy", "routine");
        const auto peerTrust = value.value("peer_trust", Json::object());
        for (auto peer = peerTrust.begin(); peer != peerTrust.end(); ++peer) {
            npc.peerTrust[peer.key()] = peer.value().get<double>();
        }
        state.npcs[it.key()] = std::move(npc);
    }
    const auto promises = json.value("promises", Json::object());
    for (auto it = promises.begin(); it != promises.end(); ++it) {
        const auto value = it.value();
        state.promises[it.key()] = Promise{
            jsonValue<std::string>(value, "id", it.key()), jsonValue<std::string>(value, "from", ""),
            jsonValue<std::string>(value, "to", ""), jsonValue<std::string>(value, "description", ""),
            GameTime{jsonValue<std::int64_t>(value, "due", 0)},
            promiseStatusFromString(jsonValue<std::string>(value, "status", "proposed")),
            jsonValue<int>(value, "version", 0)};
    }
    for (const auto& event : json.value("events", Json::array())) state.events.push_back(worldEventFromJson(event));
    for (const auto& turn : json.value("processed_turn_ids", std::vector<std::string>{})) {
        state.processedTurnIds.insert(turn);
    }
    return state;
}

}  // namespace tide_marks
