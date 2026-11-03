#include "content/content_pack.hpp"
#include "world/meta_actions.hpp"

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

double boundedNumber(const Json& value, const char* key, const std::string& path,
                     double minimum, double maximum) {
    if (!value.contains(key) || !value.at(key).is_number()) {
        throw std::runtime_error("missing numeric '" + std::string(key) + "' in " + path);
    }
    const double result = value.at(key).get<double>();
    if (result < minimum || result > maximum) {
        throw std::runtime_error("numeric '" + std::string(key) + "' is out of range in " + path);
    }
    return result;
}

std::vector<NeedContent> parseNeeds(const Json& value, const std::vector<ActionCandidate>& actions,
                                    const std::string& path) {
    if (!value.contains("needs") || !value.at("needs").is_array()) {
        throw std::runtime_error("NPC must define a 'needs' array in " + path);
    }
    std::set<std::string> actionIds;
    for (const auto& action : actions) actionIds.insert(action.actionId);
    std::set<std::string> needIds;
    std::vector<NeedContent> result;
    for (const auto& item : value.at("needs")) {
        if (!item.is_object()) throw std::runtime_error("invalid need in " + path);
        NeedContent need;
        need.id = requiredString(item, "id", path);
        need.taxonomy = requiredString(item, "taxonomy", path);
        if (!needIds.insert(need.id).second) throw std::runtime_error("duplicate need id: " + need.id);
        need.initialDeficit = boundedNumber(item, "initial_deficit", path, 0.0, 1.0);
        need.decayPerHour = boundedNumber(item, "decay_per_hour", path, 0.0, 1.0);
        need.criticalThreshold = boundedNumber(item, "critical_threshold", path, 0.0, 1.0);
        need.weight = boundedNumber(item, "weight", path, 0.0, 100.0);
        need.priority = boundedNumber(item, "priority", path, 0.0, 100.0);
        need.sourceKind = item.value("source_kind", "authored");
        need.sourceId = item.value("source_id", need.id);
        need.sourceVersion = item.value("source_version", "1");
        if (!item.contains("satisfiers") || !item.at("satisfiers").is_object()) {
            throw std::runtime_error("need must define an object 'satisfiers' in " + path);
        }
        for (auto it = item.at("satisfiers").begin(); it != item.at("satisfiers").end(); ++it) {
            if (!it.value().is_number()) throw std::runtime_error("need satisfier must be numeric in " + path);
            const double relief = it.value().get<double>();
            if (relief < 0.0 || relief > 1.0) throw std::runtime_error("need satisfier is out of range in " + path);
            if (!actionIds.contains(it.key())) {
                throw std::runtime_error("need satisfier references an unauthored action: " + it.key());
            }
            need.satisfiers[it.key()] = relief;
        }
        result.push_back(std::move(need));
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
    result.initialStrategy = initial.value("strategy", "routine");
    const auto peerTrust = initial.value("peer_trust", Json::object());
    for (auto it = peerTrust.begin(); it != peerTrust.end(); ++it) {
        result.initialPeerTrust[it.key()] = it.value().get<double>();
    }
    result.schedule = parseSchedule(value.value("schedule", Json::object()), path);
    result.actions = parseActions(beats, result.definition.id);
    result.needs = parseNeeds(value, result.actions, path);
    return result;
}

ActivityContent parseActivity(const Json& value, const std::string& path) {
    ActivityContent result;
    result.id = requiredString(value, "id", path);
    result.trigger = requiredString(value, "trigger", path);
    if (result.trigger != "hour" && result.trigger != "day") {
        throw std::runtime_error("activity trigger must be hour or day in " + path);
    }
    result.hour = value.value("hour", -1);
    if (result.trigger == "hour" && (result.hour < 0 || result.hour > 23)) {
        throw std::runtime_error("hour activity needs an authoring hour in " + path);
    }
    if (result.trigger == "day" && result.hour != -1) {
        throw std::runtime_error("day activity cannot set hour in " + path);
    }
    result.actor = value.value("actor", "world");
    result.target = value.value("target", "");
    result.conditionResource = value.value("condition_resource", "");
    result.conditionBelow = value.value("condition_below", 0);
    result.eventType = requiredString(value, "event_type", path);
    result.detail = value.value("detail", result.id);
    if (!value.contains("effects") || !value.at("effects").is_array()) {
        throw std::runtime_error("activity needs an effects array in " + path);
    }
    for (const auto& effectValue : value.at("effects")) {
        EffectContent effect;
        effect.operation = requiredString(effectValue, "operation", path);
        if (!MetaActionRegistry::contains(effect.operation)) {
            throw std::runtime_error("unknown activity effect operation in " + path + ": " + effect.operation);
        }
        if (!MetaActionRegistry::isImplemented(effect.operation)) {
            throw std::runtime_error("activity effect uses an unimplemented meta action in " + path + ": " +
                                     effect.operation);
        }
        effect.resource = requiredString(effectValue, "resource", path);
        effect.amount = effectValue.value("amount", 0);
        effect.maximum = effectValue.value("maximum", 100);
        effect.effectAction = effectValue.value("action", "");
        effect.targets = effectValue.value("targets", std::vector<std::string>{});
        if ((effect.operation == "consume_resource" || effect.operation == "produce_resource") &&
            effect.amount <= 0) {
            throw std::runtime_error("resource meta action needs a positive amount in " + path);
        }
        if (effect.operation == "consume_resource" && !effect.targets.empty()) {
            throw std::runtime_error("public resource consume does not accept targets in " + path);
        }
        result.effects.push_back(std::move(effect));
    }
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
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        if (!it.value().is_object()) {
            throw std::runtime_error("world resource must define its properties: " + it.key() + " in " + worldPath);
        }
        ResourceContent resource;
        resource.id = it.key();
        resource.unit = requiredString(it.value(), "unit", worldPath);
        resource.kind = requiredString(it.value(), "kind", worldPath);
        resource.owner = it.value().value("owner", "world");
        resource.location = it.value().value("location", "");
        resource.initial = it.value().value("initial", 0);
        resource.minimum = it.value().value("minimum", 0);
        resource.maximum = it.value().value("maximum", 100);
        resource.divisible = it.value().value("divisible", true);
        if (resource.initial < resource.minimum || resource.initial > resource.maximum ||
            resource.minimum < 0 || resource.maximum < resource.minimum) {
            throw std::runtime_error("resource initial value or capacity is out of range: " + it.key());
        }
        pack.resources[it.key()] = std::move(resource);
    }
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
    const auto activities = world.value("activities", Json::array());
    if (!activities.is_array()) throw std::runtime_error("activities must be an array: " + worldPath);
    std::set<std::string> activityIds;
    for (const auto& activity : activities) {
        auto parsed = parseActivity(activity, worldPath);
        if (!activityIds.insert(parsed.id).second) throw std::runtime_error("duplicate activity id: " + parsed.id);
        pack.activities.push_back(std::move(parsed));
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
    for (const auto& activity : pack.activities) {
        if (!activity.conditionResource.empty() && !pack.resources.contains(activity.conditionResource)) {
            throw std::runtime_error("activity condition references an unknown resource: " + activity.conditionResource);
        }
        if (!activity.actor.empty() && activity.actor != "world" && !npcIds.contains(activity.actor)) {
            throw std::runtime_error("activity actor is not an authored NPC: " + activity.actor);
        }
        for (const auto& effect : activity.effects) {
            if (!pack.resources.contains(effect.resource)) {
                throw std::runtime_error("activity effect references an unknown resource: " + effect.resource);
            }
            for (const auto& target : effect.targets) {
                if (!npcIds.contains(target)) throw std::runtime_error("activity effect targets an unknown NPC: " + target);
            }
        }
    }
    return pack;
}

}  // namespace tide_marks
