#include "content/content_pack.hpp"

#include <fstream>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <set>

namespace tide_marks {
namespace {

using Json = nlohmann::json;

Json readJson(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read content file: " + path);
    Json value;
    input >> value;
    return value;
}

std::string requiredString(const Json& value, const char* key, const std::string& path) {
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::runtime_error("missing string '" + std::string(key) + "' in " + path);
    }
    return value.at(key).get<std::string>();
}

int clockMinute(const std::string& value) {
    const auto separator = value.find(':');
    if (separator == std::string::npos) throw std::runtime_error("invalid schedule time: " + value);
    const int hour = std::stoi(value.substr(0, separator));
    const int minute = std::stoi(value.substr(separator + 1));
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) throw std::runtime_error("invalid schedule time: " + value);
    return hour * 60 + minute;
}

std::vector<ScheduleSlot> parseSchedule(const Json& value, const std::string& path) {
    std::vector<ScheduleSlot> result;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto separator = it.key().find('-');
        if (separator == std::string::npos || !it.value().is_string()) {
            throw std::runtime_error("invalid schedule entry in " + path);
        }
        result.push_back(ScheduleSlot{clockMinute(it.key().substr(0, separator)),
                                      clockMinute(it.key().substr(separator + 1)), it.value().get<std::string>()});
    }
    return result;
}

std::vector<ActionCandidate> parseActions(const Json& beats, const std::string& npcId) {
    std::vector<ActionCandidate> result;
    for (const auto& beat : beats.value("beats", Json::array())) {
        if (beat.value("npc", "") != npcId) continue;
        for (const auto& action : beat.value("allowed_actions", Json::array())) {
            const std::string id = action.get<std::string>();
            result.push_back(ActionCandidate{id, beat.value("id", "content"), id});
        }
    }
    return result;
}

std::vector<std::string> stringArray(const Json& value, const char* key, const std::string& path) {
    std::vector<std::string> result;
    const auto items = value.value(key, Json::array());
    if (!items.is_array()) throw std::runtime_error("expected array '" + std::string(key) + "' in " + path);
    for (const auto& item : items) {
        if (!item.is_string()) throw std::runtime_error("expected string in '" + std::string(key) + "' in " + path);
        result.push_back(item.get<std::string>());
    }
    return result;
}

SpatialContent parseSpatial(const Json& value, const std::string& path) {
    const auto bounds = value.value("bounds", Json::object());
    SpatialContent result;
    result.id = requiredString(value, "id", path);
    result.kind = value.value("kind", "place");
    result.spatial = SpatialComponent{requiredString(value, "display_name", path),
                                     bounds.value("min_x", -10.0), bounds.value("max_x", 10.0),
                                     bounds.value("min_z", -10.0), bounds.value("max_z", 10.0),
                                     stringArray(value, "tags", path)};
    return result;
}

LinkContent parseLink(const Json& value, const std::string& path) {
    LinkContent result;
    result.id = requiredString(value, "id", path);
    result.kind = value.value("kind", "link");
    result.link = LinkComponent{stringArray(value, "endpoints", path), value.value("traversable", true),
                                value.value("locked", false), value.value("required_item", ""),
                                stringArray(value, "tags", path)};
    if (result.link.endpoints.size() < 2) throw std::runtime_error("link needs at least two endpoints in " + path);
    return result;
}

NpcContent parseNpc(const Json& value, const Json& beats, const std::string& path) {
    NpcContent result;
    result.definition = NpcDefinition{requiredString(value, "id", path), requiredString(value, "name", path),
                                      requiredString(value, "role", path), requiredString(value, "home", path),
                                      requiredString(value, "workplace", path), requiredString(value, "public_goal", path),
                                      requiredString(value, "hidden_goal", path), requiredString(value, "boundary", path),
                                      value.value("voice", "")};
    const auto initial = value.value("initial", Json::object());
    result.initialLocation = initial.value("location", result.definition.workplace);
    result.initialScheduleLabel = initial.value("schedule_label", result.initialLocation);
    result.initialGoalStage = initial.value("goal_stage", 0);
    result.initialResource = initial.value("resource", 0);
    result.initialPressure = initial.value("pressure", 0.2);
    result.initialStrategy = initial.value("strategy", "routine");
    const auto peerTrust = initial.value("peer_trust", Json::object());
    for (auto it = peerTrust.begin(); it != peerTrust.end(); ++it) {
        result.initialPeerTrust[it.key()] = it.value().get<double>();
    }
    result.schedule = parseSchedule(value.value("schedule", Json::object()), path);
    result.actions = parseActions(beats, result.definition.id);
    return result;
}

}  // namespace

std::string ContentPackLoader::defaultRoot() {
    if (const char* configured = std::getenv("TIDE_MARKS_DATA_DIR"); configured != nullptr && *configured != '\0') {
        return configured;
    }
#ifdef TIDE_MARKS_DATA_DIR
    return TIDE_MARKS_DATA_DIR;
#else
    return "data";
#endif
}

WorldContentPack ContentPackLoader::load(const std::string& rootPath) {
    const auto manifestPath = rootPath + "/content-pack.json";
    const auto manifest = readJson(manifestPath);
    WorldContentPack pack;
    pack.id = requiredString(manifest, "id", manifestPath);
    pack.version = requiredString(manifest, "version", manifestPath);
    const auto worldPath = rootPath + "/" + requiredString(manifest, "world", manifestPath);
    const auto world = readJson(worldPath);
    pack.contentVersion = world.value("content_version", pack.version);
    pack.worldId = world.value("id", pack.id);
    const auto initialTime = world.value("initial_time", Json::object());
    pack.startTime = GameTime::fromDayHour(initialTime.value("day", 1), initialTime.value("hour", 6));
    const auto resources = world.value("resources", Json::object());
    pack.initialVillage.grain = resources.value("grain", 100);
    pack.initialVillage.boatIntegrity = resources.value("boat_integrity", 72);
    pack.initialVillage.recordsIntegrity = resources.value("records_integrity", 80);
    pack.initialVillage.lanternLit = resources.value("lantern_lit", true);
    pack.initialVillage.reputation = resources.value("reputation", 0);
    pack.initialVillage.cooperation = resources.value("cooperation", 0);
    if (!world.contains("entities") || !world.at("entities").is_array()) {
        throw std::runtime_error("world content must define an 'entities' array: " + worldPath);
    }
    if (!world.contains("links") || !world.at("links").is_array()) {
        throw std::runtime_error("world content must define a 'links' array: " + worldPath);
    }
    for (const auto& entity : world.at("entities")) pack.spatialEntities.push_back(parseSpatial(entity, worldPath));
    for (const auto& link : world.at("links")) pack.links.push_back(parseLink(link, worldPath));
    const auto archetypes = world.value("entity_archetypes", Json::array());
    if (!archetypes.is_array()) throw std::runtime_error("entity_archetypes must be an array: " + worldPath);
    for (const auto& archetype : archetypes) {
        const auto defaultMetadata = archetype.value("metadata", Json::object());
        if (!defaultMetadata.is_object()) throw std::runtime_error("entity archetype metadata must be an object: " + worldPath);
        pack.entityArchetypes.push_back(EntityArchetypeContent{requiredString(archetype, "id", worldPath),
                                                                requiredString(archetype, "kind", worldPath),
                                                                defaultMetadata});
    }
    std::set<std::string> archetypeIds;
    for (const auto& archetype : pack.entityArchetypes) {
        if (!archetypeIds.insert(archetype.id).second) throw std::runtime_error("duplicate entity archetype id: " + archetype.id);
    }
    std::set<std::string> spatialIds;
    for (const auto& entity : pack.spatialEntities) {
        if (!spatialIds.insert(entity.id).second) throw std::runtime_error("duplicate spatial entity id: " + entity.id);
    }
    for (const auto& link : pack.links) {
        for (const auto& endpoint : link.link.endpoints) {
            if (!spatialIds.contains(endpoint)) {
                throw std::runtime_error("link endpoint does not reference an authored spatial entity: " + endpoint);
            }
        }
    }
    const auto dialoguePath = rootPath + "/" + requiredString(manifest, "dialogue", manifestPath);
    const auto beats = readJson(dialoguePath);
    for (const auto& npcPathValue : manifest.at("npcs")) {
        const auto relativePath = npcPathValue.get<std::string>();
        const auto path = rootPath + "/" + relativePath;
        auto npc = parseNpc(readJson(path), beats, path);
        if (npc.actions.empty()) throw std::runtime_error("NPC has no authored actions: " + npc.definition.id);
        pack.npcs.push_back(std::move(npc));
    }
    std::set<std::string> npcIds;
    for (const auto& npc : pack.npcs) {
        if (!npcIds.insert(npc.definition.id).second) throw std::runtime_error("duplicate NPC id: " + npc.definition.id);
        if (!spatialIds.contains(npc.initialLocation)) {
            throw std::runtime_error("NPC initial location is not an authored spatial entity: " + npc.initialLocation);
        }
        for (const auto& slot : npc.schedule) {
            if (!spatialIds.contains(slot.location)) {
                throw std::runtime_error("NPC schedule location is not an authored spatial entity: " + slot.location);
            }
        }
    }
    return pack;
}

}  // namespace tide_marks
