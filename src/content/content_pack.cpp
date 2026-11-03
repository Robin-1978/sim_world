#include "content/content_pack.hpp"
#include "world/meta_actions.hpp"

#include <algorithm>
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
    return hour * static_cast<int>(kMinutesPerHour) + minute;
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

std::vector<std::string> stringArray(const Json& value, const char* key, const std::string& path);

std::vector<ActionCandidate> parseActions(const Json& beats, const std::string& npcId) {
    std::vector<ActionCandidate> result;
    for (const auto& beat : beats.value("beats", Json::array())) {
        if (beat.value("npc", "") != npcId) continue;
        const auto behaviorSet = beat.value("behaviors", Json::object());
        if (!behaviorSet.is_object()) throw std::runtime_error("dialogue beat behaviors must be an object");
        std::set<std::string> allowedIds;
        for (const auto& action : beat.value("allowed_actions", Json::array())) {
            if (!action.is_string()) throw std::runtime_error("allowed_actions entries must be strings");
            const std::string id = action.get<std::string>();
            allowedIds.insert(id);
            const auto definition = behaviorSet.value(id, Json::object());
            if (!definition.is_object()) throw std::runtime_error("dialogue behavior must be an object: " + id);
            const int duration = definition.value("duration_minutes", 0);
            const double interruptThreshold = definition.value("interrupt_salience_threshold", 1.0);
            if ((!definition.empty() && (!definition.contains("duration_minutes") ||
                                         !definition.at("duration_minutes").is_number_integer())) ||
                duration < 0 || interruptThreshold < 0.0 || interruptThreshold > 1.0) {
                throw std::runtime_error("invalid behavior definition in dialogue content");
            }
            result.push_back(ActionCandidate{id, beat.value("id", "content"), id, duration,
                                             definition.value("interruptible", true), interruptThreshold,
                                             stringArray(definition, "keywords", "dialogue content")});
        }
        for (auto it = behaviorSet.begin(); it != behaviorSet.end(); ++it) {
            if (!allowedIds.contains(it.key())) {
                throw std::runtime_error("dialogue behavior is not an allowed action: " + it.key());
            }
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
        need.domain = requiredString(item, "domain", path);
        if (need.domain != "physiological" && need.domain != "psychological" &&
            need.domain != "social" && need.domain != "cognitive") {
            throw std::runtime_error("need domain must be physiological, psychological, social or cognitive in " + path);
        }
        need.dimension = item.value("dimension", "");
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
            if (!it.value().is_object()) throw std::runtime_error("need satisfier must be an object in " + path);
            const double relief = boundedNumber(it.value(), "relief", path, 0.0, 1.0);
            if (!actionIds.contains(it.key())) {
                throw std::runtime_error("need satisfier references an unauthored action: " + it.key());
            }
            const auto metaActions = stringArray(it.value(), "meta_actions", path);
            if (metaActions.empty()) throw std::runtime_error("need satisfier must reference meta_actions in " + path);
            for (const auto& metaAction : metaActions) {
                if (!MetaActionRegistry::contains(metaAction) || !MetaActionRegistry::isImplemented(metaAction)) {
                    throw std::runtime_error("need satisfier references an unavailable meta action: " + metaAction);
                }
            }
            if (std::find(metaActions.begin(), metaActions.end(), "relieve_need") == metaActions.end()) {
                throw std::runtime_error("need satisfier must commit explicit relieve_need in " + path);
            }
            need.satisfiers[it.key()] = NeedSatisfier{relief, metaActions};
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
        effect.resource = effectValue.value("resource", "");
        effect.amount = effectValue.value("amount", 0);
        effect.maximum = effectValue.value("maximum", 100);
        effect.effectAction = effectValue.value("action", "");
        effect.targets = effectValue.value("targets", std::vector<std::string>{});
        if ((effect.operation == "consume_resource" || effect.operation == "produce_resource" ||
             effect.operation == "reserve_resource" || effect.operation == "release_resource") &&
            effect.amount <= 0) {
            throw std::runtime_error("resource meta action needs a positive amount in " + path);
        }
        if (effect.operation == "relieve_need") {
            if (effect.effectAction.empty()) {
                throw std::runtime_error("relieve_need requires the committed action id in 'action' in " + path);
            }
            if (!effect.resource.empty() || effect.amount != 0 || !effect.targets.empty()) {
                throw std::runtime_error("relieve_need cannot write resources or arbitrary targets in " + path);
            }
        } else if (effect.operation == "set_link_state") {
            effect.linkId = requiredString(effectValue, "link", path);
            effect.traversable = effectValue.value("traversable", true);
            effect.locked = effectValue.value("locked", false);
            if (!effect.resource.empty() || effect.amount != 0 || !effect.targets.empty()) {
                throw std::runtime_error("set_link_state cannot write resources or NPC targets in " + path);
            }
        } else if (effect.resource.empty()) {
            throw std::runtime_error("resource meta action requires 'resource' in " + path);
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
    if (manifest.value("kind", "") != "world_content_pack" || manifest.value("schema_version", 0) != 1) {
        throw std::runtime_error("unsupported content-pack manifest schema: " + manifestPath);
    }
    WorldContentPack pack;
    pack.id = requiredString(manifest, "id", manifestPath);
    pack.version = requiredString(manifest, "version", manifestPath);
    const auto worldPath = rootPath + "/" + requiredString(manifest, "world", manifestPath);
    const auto world = readJson(worldPath);
    if (world.value("kind", "") != "world") {
        throw std::runtime_error("world content must declare kind=world: " + worldPath);
    }
    pack.contentVersion = requiredString(world, "content_version", worldPath);
    pack.worldId = requiredString(world, "id", worldPath);
    const auto initialTime = world.value("initial_time", Json::object());
    pack.startTime = GameTime::fromDayHour(initialTime.value("day", 1), initialTime.value("hour", 6));
    if (!world.contains("drive_adaptation") || !world.at("drive_adaptation").is_object()) {
        throw std::runtime_error("world must define a drive_adaptation policy: " + worldPath);
    }
    const auto& adaptation = world.at("drive_adaptation");
    pack.driveAdaptation.enabled = adaptation.value("enabled", true);
    pack.driveAdaptation.expectationLearningRate = boundedNumber(adaptation, "expectation_learning_rate", worldPath, 0.0, 1.0);
    pack.driveAdaptation.maxExpectationStep = boundedNumber(adaptation, "max_expectation_step", worldPath, 0.0, 1.0);
    if (!adaptation.contains("cooldown_minutes") || !adaptation.at("cooldown_minutes").is_number_integer() ||
        adaptation.at("cooldown_minutes").get<int>() < 0) {
        throw std::runtime_error("drive_adaptation.cooldown_minutes must be a non-negative integer in " + worldPath);
    }
    pack.driveAdaptation.cooldownMinutes = adaptation.at("cooldown_minutes").get<int>();
    if (!world.contains("stimulus_policy") || !world.at("stimulus_policy").is_object()) {
        throw std::runtime_error("world must define a stimulus_policy: " + worldPath);
    }
    const auto& stimulusPolicy = world.at("stimulus_policy");
    if (!stimulusPolicy.contains("max_active_per_npc") ||
        !stimulusPolicy.at("max_active_per_npc").is_number_integer() ||
        stimulusPolicy.at("max_active_per_npc").get<int>() <= 0) {
        throw std::runtime_error("stimulus_policy.max_active_per_npc must be positive in " + worldPath);
    }
    pack.stimulusPolicy.maxActivePerNpc = static_cast<std::size_t>(stimulusPolicy.at("max_active_per_npc").get<int>());
    pack.stimulusPolicy.noveltyDecay = boundedNumber(stimulusPolicy, "novelty_decay", worldPath, 0.0, 1.0);
    pack.stimulusPolicy.habituationStep = boundedNumber(stimulusPolicy, "habituation_step", worldPath, 0.0, 1.0);
    pack.stimulusPolicy.minSensitivity = boundedNumber(stimulusPolicy, "min_sensitivity", worldPath, 0.0, 10.0);
    pack.stimulusPolicy.maxSensitivity = boundedNumber(stimulusPolicy, "max_sensitivity", worldPath, 0.0, 10.0);
    if (pack.stimulusPolicy.minSensitivity > pack.stimulusPolicy.maxSensitivity) {
        throw std::runtime_error("stimulus sensitivity bounds are reversed in " + worldPath);
    }
    if (!stimulusPolicy.contains("repeat_window_minutes") ||
        !stimulusPolicy.at("repeat_window_minutes").is_number_integer() ||
        stimulusPolicy.at("repeat_window_minutes").get<int>() < 0) {
        throw std::runtime_error("stimulus_policy.repeat_window_minutes must be non-negative in " + worldPath);
    }
    pack.stimulusPolicy.repeatWindowMinutes = stimulusPolicy.at("repeat_window_minutes").get<int>();
    const auto deliberation = world.value("npc_deliberation", Json::object());
    if (!deliberation.is_object()) {
        throw std::runtime_error("npc_deliberation must be an object in " + worldPath);
    }
    if (!deliberation.contains("enabled") || !deliberation.at("enabled").is_boolean()) {
        throw std::runtime_error("npc_deliberation.enabled must be a boolean in " + worldPath);
    }
    pack.npcDeliberation.enabled = deliberation.at("enabled").get<bool>();
    pack.npcDeliberation.minStimulusSalience =
        boundedNumber(deliberation, "min_stimulus_salience", worldPath, 0.0, 1.0);
    pack.npcDeliberation.minDrivePressure = boundedNumber(deliberation, "min_drive_pressure", worldPath, 0.0, 1.0);
    pack.npcDeliberation.minInterruptSalience =
        boundedNumber(deliberation, "min_interrupt_salience", worldPath, 0.0, 1.0);
    if (!deliberation.contains("cooldown_minutes") || !deliberation.at("cooldown_minutes").is_number_integer() ||
        deliberation.at("cooldown_minutes").get<int>() < 0) {
        throw std::runtime_error("npc_deliberation.cooldown_minutes must be non-negative in " + worldPath);
    }
    pack.npcDeliberation.cooldownMinutes = deliberation.at("cooldown_minutes").get<int>();
    if (pack.npcDeliberation.minStimulusSalience < 0.0 || pack.npcDeliberation.minStimulusSalience > 1.0 ||
        pack.npcDeliberation.minDrivePressure < 0.0 || pack.npcDeliberation.minDrivePressure > 1.0 ||
        pack.npcDeliberation.minInterruptSalience < 0.0 || pack.npcDeliberation.minInterruptSalience > 1.0) {
        throw std::runtime_error("npc_deliberation values are out of range in " + worldPath);
    }
    const auto promisePolicy = world.value("promise_policy", Json::object());
    if (!promisePolicy.is_object()) {
        throw std::runtime_error("promise_policy must be an object in " + worldPath);
    }
    if (promisePolicy.contains("renegotiation_extension_minutes")) {
        const auto& extension = promisePolicy.at("renegotiation_extension_minutes");
        if (!extension.is_number_integer() || extension.get<int>() <= 0) {
            throw std::runtime_error("promise_policy.renegotiation_extension_minutes must be a positive integer");
        }
        pack.promisePolicy.renegotiationExtensionMinutes = extension.get<int>();
    }
    const auto parseRelationshipDelta = [&worldPath](const Json& value) {
        if (!value.is_object()) throw std::runtime_error("relationship delta must be an object in " + worldPath);
        const auto axis = [&value, &worldPath](const char* key) {
            if (!value.contains(key)) return 0.0;
            if (!value.at(key).is_number()) {
                throw std::runtime_error(std::string("relationship delta '") + key + "' must be numeric in " + worldPath);
            }
            const double result = value.at(key).get<double>();
            if (result < -1.0 || result > 1.0) {
                throw std::runtime_error(std::string("relationship delta '") + key + "' is out of range in " + worldPath);
            }
            return result;
        };
        RelationshipDelta delta;
        delta.trust = axis("trust");
        delta.respect = axis("respect");
        delta.fear = axis("fear");
        delta.owed = axis("owed");
        return delta;
    };
    if (promisePolicy.contains("broken")) {
        const auto& broken = promisePolicy.at("broken");
        if (!broken.is_object()) throw std::runtime_error("promise_policy.broken must be an object");
        if (broken.contains("promiser")) pack.promisePolicy.brokenPromiser = parseRelationshipDelta(broken.at("promiser"));
        if (broken.contains("promisee")) pack.promisePolicy.brokenPromisee = parseRelationshipDelta(broken.at("promisee"));
    }
    if (promisePolicy.contains("refused")) {
        const auto& refused = promisePolicy.at("refused");
        if (!refused.is_object() || !refused.contains("actor")) {
            throw std::runtime_error("promise_policy.refused.actor must be defined");
        }
        pack.promisePolicy.refusedActor = parseRelationshipDelta(refused.at("actor"));
    }
    if (promisePolicy.contains("accepted")) {
        const auto& accepted = promisePolicy.at("accepted");
        if (!accepted.is_object() || !accepted.contains("actor")) {
            throw std::runtime_error("promise_policy.accepted.actor must be defined");
        }
        pack.promisePolicy.acceptedActor = parseRelationshipDelta(accepted.at("actor"));
    }
    if (!world.contains("stimulus_rules") || !world.at("stimulus_rules").is_array()) {
        throw std::runtime_error("world must define a stimulus_rules array: " + worldPath);
    }
    std::set<std::string> stimulusRuleIds;
    for (const auto& item : world.at("stimulus_rules")) {
        StimulusRule rule;
        rule.id = requiredString(item, "id", worldPath);
        if (!stimulusRuleIds.insert(rule.id).second) throw std::runtime_error("duplicate stimulus rule: " + rule.id);
        rule.eventPrefix = requiredString(item, "event_prefix", worldPath);
        rule.category = requiredString(item, "category", worldPath);
        rule.visibility = requiredString(item, "visibility", worldPath);
        if (rule.visibility != "world" && rule.visibility != "participants" &&
            rule.visibility != "actor" && rule.visibility != "target") {
            throw std::runtime_error("unsupported stimulus visibility in " + worldPath + ": " + rule.visibility);
        }
        rule.relevance = boundedNumber(item, "relevance", worldPath, 0.0, 1.0);
        rule.intensity = boundedNumber(item, "intensity", worldPath, 0.0, 1.0);
        rule.urgency = boundedNumber(item, "urgency", worldPath, 0.0, 1.0);
        rule.valence = boundedNumber(item, "valence", worldPath, -1.0, 1.0);
        rule.sourceConfidence = boundedNumber(item, "source_confidence", worldPath, 0.0, 1.0);
        if (!item.contains("ttl_minutes") || !item.at("ttl_minutes").is_number_integer() ||
            item.at("ttl_minutes").get<int>() <= 0) {
            throw std::runtime_error("stimulus rule ttl_minutes must be positive in " + worldPath);
        }
        rule.ttlMinutes = item.at("ttl_minutes").get<int>();
        pack.stimulusRules.push_back(std::move(rule));
    }
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
    const auto strategyRules = world.value("strategy_rules", Json::array());
    if (!strategyRules.is_array()) throw std::runtime_error("strategy_rules must be an array: " + worldPath);
    std::set<std::string> strategyRuleIds;
    for (const auto& item : strategyRules) {
        StrategyRuleContent rule;
        rule.id = requiredString(item, "id", worldPath);
        if (!strategyRuleIds.insert(rule.id).second) throw std::runtime_error("duplicate strategy rule: " + rule.id);
        rule.npcId = requiredString(item, "npc", worldPath);
        rule.peerId = requiredString(item, "peer", worldPath);
        rule.minPeerTrust = boundedNumber(item, "min_peer_trust", worldPath, 0.0, 1.0);
        rule.strategy = requiredString(item, "strategy", worldPath);
        rule.goalStage = item.value("goal_stage", 0);
        rule.memory = requiredString(item, "memory", worldPath);
        rule.eventType = requiredString(item, "event_type", worldPath);
        pack.strategyRules.push_back(std::move(rule));
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
            if (effect.operation != "relieve_need" && effect.operation != "set_link_state" &&
                !pack.resources.contains(effect.resource)) {
                throw std::runtime_error("activity effect references an unknown resource: " + effect.resource);
            }
            if (effect.operation == "set_link_state") {
                const auto link = std::find_if(pack.links.begin(), pack.links.end(),
                                               [&effect](const auto& item) { return item.id == effect.linkId; });
                if (link == pack.links.end()) {
                    throw std::runtime_error("activity effect references an unknown link: " + effect.linkId);
                }
            }
            for (const auto& target : effect.targets) {
                if (!npcIds.contains(target)) throw std::runtime_error("activity effect targets an unknown NPC: " + target);
            }
        }
    }
    std::map<std::string, std::set<std::string>> reliefActionsByActor;
    for (const auto& activity : pack.activities) {
        for (const auto& effect : activity.effects) {
            if (effect.operation == "relieve_need") {
                reliefActionsByActor[activity.actor].insert(effect.effectAction);
            }
        }
    }
    for (const auto& npc : pack.npcs) {
        if (npc.needs.empty()) throw std::runtime_error("NPC must define at least one need: " + npc.definition.id);
        for (const auto& need : npc.needs) {
            if (need.satisfiers.empty()) {
                throw std::runtime_error("need must define an executable satisfier: " + npc.definition.id +
                                         "/" + need.id);
            }
            for (const auto& [actionId, satisfier] : need.satisfiers) {
                (void)satisfier;
                if (!reliefActionsByActor[npc.definition.id].contains(actionId)) {
                    throw std::runtime_error("need satisfier has no explicit relieve_need executor: " +
                                             npc.definition.id + "/" + need.id + "/" + actionId);
                }
            }
        }
    }
    for (const auto& rule : pack.strategyRules) {
        if (!npcIds.contains(rule.npcId) || !npcIds.contains(rule.peerId)) {
            throw std::runtime_error("strategy rule references an unknown NPC: " + rule.id);
        }
    }
    if (manifest.contains("instance")) {
        const auto instancePath = rootPath + "/" + requiredString(manifest, "instance", manifestPath);
        const auto instanceJson = readJson(instancePath);
        if (instanceJson.value("kind", "") != "game_instance") {
            throw std::runtime_error("instance file must declare kind=game_instance: " + instancePath);
        }
        if (instanceJson.value("schema_version", 0) != 1) {
            throw std::runtime_error("unsupported game instance schema_version in " + instancePath);
        }
        GameInstanceContent instance;
        instance.id = requiredString(instanceJson, "id", instancePath);
        instance.displayName = instanceJson.value("display_name", instance.id);
        instance.contentPackId = requiredString(instanceJson, "content_pack", instancePath);
        instance.contentVersion = requiredString(instanceJson, "content_version", instancePath);
        if (instance.contentPackId != pack.id) {
            throw std::runtime_error("instance references content pack '" + instance.contentPackId +
                                     "' but loaded pack is '" + pack.id + "'");
        }
        if (instance.contentVersion != pack.contentVersion) {
            throw std::runtime_error("instance requires content version '" + instance.contentVersion +
                                     "' but pack is '" + pack.contentVersion + "'");
        }
        instance.worldId = instanceJson.value("world_id", pack.worldId);
        if (instance.worldId.empty()) throw std::runtime_error("game instance world_id must not be empty: " + instancePath);
        if (instanceJson.contains("initial_time")) {
            const auto& time = instanceJson.at("initial_time");
            if (!time.is_object() || !time.contains("day") || !time.contains("hour") || !time.contains("minute")) {
                throw std::runtime_error("game instance initial_time must contain day/hour/minute: " + instancePath);
            }
            instance.startTime = GameTime::fromDayHour(time.value("day", 1), time.value("hour", 6),
                                                       time.value("minute", 0));
        }
        const auto instanceResources = instanceJson.value("resources", Json::object());
        for (auto it = instanceResources.begin(); it != instanceResources.end(); ++it) {
            const auto definition = pack.resources.find(it.key());
            if (definition == pack.resources.end()) {
                throw std::runtime_error("instance references an unknown resource: " + it.key());
            }
            if (!it.value().is_number_integer()) {
                throw std::runtime_error("instance resource initial must be an integer: " + it.key());
            }
            const int amount = it.value().get<int>();
            if (amount < definition->second.minimum || amount > definition->second.maximum) {
                throw std::runtime_error("instance resource initial is out of range: " + it.key());
            }
            instance.resourceInitial[it.key()] = amount;
        }
        const auto instanceNpcs = instanceJson.value("npcs", Json::object());
        for (auto it = instanceNpcs.begin(); it != instanceNpcs.end(); ++it) {
            if (!npcIds.contains(it.key())) {
                throw std::runtime_error("instance references an unknown NPC: " + it.key());
            }
            if (!it.value().is_object()) throw std::runtime_error("instance NPC entry must be an object: " + it.key());
            NpcInstanceContent npc;
            npc.location = it.value().value("location", "");
            if (!npc.location.empty() && !spatialIds.contains(npc.location)) {
                throw std::runtime_error("instance NPC location is not authored: " + npc.location);
            }
            npc.scheduleLabel = it.value().value("schedule_label", npc.location);
            npc.goalStage = it.value().value("goal_stage", 0);
            npc.strategy = it.value().value("strategy", "");
            const auto peerTrust = it.value().value("peer_trust", Json::object());
            for (auto peer = peerTrust.begin(); peer != peerTrust.end(); ++peer) {
                if (!npcIds.contains(peer.key())) {
                    throw std::runtime_error("instance peer_trust references an unknown NPC: " + peer.key());
                }
                npc.peerTrust[peer.key()] = peer.value().get<double>();
            }
            instance.npcs[it.key()] = std::move(npc);
        }
        pack.instance = std::move(instance);
    }
    return pack;
}

}  // namespace tide_marks
