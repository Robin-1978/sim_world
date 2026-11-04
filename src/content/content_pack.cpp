#include "content/content_pack.hpp"
#include "world/meta_actions.hpp"

#include <algorithm>
#include <cmath>
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

BehaviorNodeContent parseBehaviorNode(const Json& value, const std::string& path,
                                      const std::string& parentId, int index) {
    if (!value.is_object()) throw std::runtime_error("behavior node must be an object in " + path);
    BehaviorNodeContent result;
    result.type = requiredString(value, "type", path);
    result.id = value.value("id", parentId + "." + std::to_string(index));
    if (result.id.empty()) throw std::runtime_error("behavior node id must not be empty in " + path);
    if (result.type == "condition" || result.type == "meta_action") {
        result.name = requiredString(value, "name", path);
    } else if (result.type != "sequence" && result.type != "selector" &&
               result.type != "interruptible" && result.type != "timeout") {
        throw std::runtime_error("unsupported behavior node type '" + result.type + "' in " + path);
    }
    result.args = value.value("args", Json::object());
    if (!result.args.is_object()) throw std::runtime_error("behavior node args must be an object in " + path);
    if (result.type == "timeout") {
        if (!value.contains("max_ticks") || !value.at("max_ticks").is_number_integer() ||
            value.at("max_ticks").get<int>() <= 0) {
            throw std::runtime_error("timeout behavior node requires positive max_ticks in " + path);
        }
        result.maxTicks = value.at("max_ticks").get<int>();
    }
    if (result.type == "sequence" || result.type == "selector" ||
        result.type == "interruptible" || result.type == "timeout") {
        if (!value.contains("children") || !value.at("children").is_array() || value.at("children").empty()) {
            throw std::runtime_error("composite behavior node requires children in " + path);
        }
        for (std::size_t childIndex = 0; childIndex < value.at("children").size(); ++childIndex) {
            result.children.push_back(parseBehaviorNode(value.at("children").at(childIndex), path,
                                                        result.id, static_cast<int>(childIndex)));
        }
        if ((result.type == "interruptible" || result.type == "timeout") && result.children.size() != 1) {
            throw std::runtime_error(result.type + " requires exactly one child in " + path);
        }
    }
    return result;
}

void parseBehaviorPack(WorldContentPack& pack, const Json& document, const std::string& path) {
    if (document.value("kind", "") != "behavior_pack" || document.value("schema_version", 0) != 2) {
        throw std::runtime_error("unsupported behavior pack schema: " + path);
    }
    const auto metaActions = document.value("meta_actions", Json::array());
    if (!metaActions.is_array()) throw std::runtime_error("behavior pack meta_actions must be an array: " + path);
    for (const auto& item : metaActions) {
        MetaActionDefinition definition;
        definition.id = requiredString(item, "id", path);
        definition.category = requiredString(item, "category", path);
        definition.description = item.value("description", definition.id);
        definition.operation = requiredString(item, "operator", path);
        definition.defaultArgs = item.value("args", Json::object());
        if (!MetaActionRegistry::isExecutableOperator(definition.operation)) {
            throw std::runtime_error("meta action '" + definition.id + "' uses an unsupported operator '" +
                                     definition.operation + "' in " + path);
        }
        if (!definition.defaultArgs.is_object()) {
            throw std::runtime_error("meta action default args must be an object: " + definition.id);
        }
        if (!pack.metaActions.emplace(definition.id, std::move(definition)).second) {
            throw std::runtime_error("duplicate meta action: " + item.value("id", std::string{}));
        }
    }
    const auto conditions = document.value("conditions", Json::array());
    if (!conditions.is_array()) throw std::runtime_error("behavior pack conditions must be an array: " + path);
    for (const auto& item : conditions) {
        ConditionDefinition definition;
        definition.id = requiredString(item, "id", path);
        definition.operation = requiredString(item, "operator", path);
        if (!MetaActionRegistry::isExecutableConditionOperator(definition.operation)) {
            throw std::runtime_error("condition uses unsupported operator: " + definition.operation);
        }
        definition.args = item.value("args", Json::object());
        if (!definition.args.is_object()) {
            throw std::runtime_error("condition default args must be an object: " + definition.id);
        }
        if (!pack.conditions.emplace(definition.id, std::move(definition)).second) {
            throw std::runtime_error("duplicate condition definition: " + item.value("id", std::string{}));
        }
    }
    const auto recipes = document.value("recipes", Json::array());
    if (!recipes.is_array()) throw std::runtime_error("behavior pack recipes must be an array: " + path);
    std::set<std::string> ids;
    for (const auto& item : recipes) {
        const auto id = requiredString(item, "id", path);
        if (!ids.insert(id).second) throw std::runtime_error("duplicate behavior recipe: " + id);
        if (!item.contains("root")) throw std::runtime_error("behavior recipe requires root: " + id);
        BehaviorRecipeContent recipe;
        recipe.id = id;
        recipe.planId = item.value("plan_id", id);
        recipe.durationMinutes = item.value("duration_minutes", 0);
        recipe.interruptible = item.value("interruptible", true);
        recipe.interruptSalienceThreshold = item.value("interrupt_salience_threshold", 1.0);
        recipe.reservationMode = item.value("reservation_mode", "none");
        recipe.priority = item.value("priority", 0);
        recipe.preemptionPolicy = item.value("preemption_policy", "none");
        const auto allowedActors = item.value("allowed_actors", Json::array());
        if (!allowedActors.is_array()) {
            throw std::runtime_error("behavior recipe allowed_actors must be an array: " + id);
        }
        for (const auto& actor : allowedActors) {
            if (!actor.is_string() || actor.get<std::string>().empty()) {
                throw std::runtime_error("behavior recipe allowed_actors entries must be non-empty strings: " + id);
            }
            recipe.allowedActors.push_back(actor.get<std::string>());
        }
        recipe.parameterSchema = item.value("parameters", Json::object());
        if (!recipe.parameterSchema.is_object()) {
            throw std::runtime_error("behavior recipe parameters must be an object: " + id);
        }
        if (recipe.reservationMode != "none" && recipe.reservationMode != "input_resources") {
            throw std::runtime_error("unsupported reservation_mode in behavior recipe: " + id);
        }
        if (recipe.preemptionPolicy != "none" && recipe.preemptionPolicy != "lower_priority") {
            throw std::runtime_error("unsupported preemption_policy in behavior recipe: " + id);
        }
        if (recipe.durationMinutes < 0 || recipe.interruptSalienceThreshold < 0.0 ||
            recipe.interruptSalienceThreshold > 1.0 || recipe.priority < 0) {
            throw std::runtime_error("invalid behavior recipe timing, priority or interrupt threshold: " + id);
        }
        recipe.root = parseBehaviorNode(item.at("root"), path, id, 0);
        pack.behaviorRecipes.push_back(std::move(recipe));
    }
}

Json mergedArgs(const Json& defaults, const Json& supplied) {
    Json result = defaults.is_object() ? defaults : Json::object();
    if (supplied.is_object()) {
        for (auto it = supplied.begin(); it != supplied.end(); ++it) result[it.key()] = it.value();
    }
    return result;
}

const BehaviorRecipeContent* findRecipe(const WorldContentPack& pack, const std::string& id) {
    const auto found = std::find_if(pack.behaviorRecipes.begin(), pack.behaviorRecipes.end(),
                                    [&id](const auto& recipe) { return recipe.id == id; });
    return found == pack.behaviorRecipes.end() ? nullptr : &*found;
}

void collectBehaviorReservations(const BehaviorNodeContent& node,
                                 const WorldContentPack& pack,
                                 std::map<std::string, int>& reservations) {
    if (node.type == "meta_action") {
        const auto definition = pack.metaActions.find(node.name);
        if (definition != pack.metaActions.end()) {
            const auto args = mergedArgs(definition->second.defaultArgs, node.args);
            if (definition->second.operation == "resource.consume" ||
                definition->second.operation == "resource.reserve") {
                const auto resource = args.value("resource", "");
                const int amount = args.value("amount", 0);
                if (!resource.empty() && amount > 0) reservations[resource] += amount;
            } else if (definition->second.operation == "production.execute") {
                const auto inputs = args.value("inputs", Json::object());
                if (inputs.is_object()) {
                    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                        if (it.value().is_number_integer() && it.value().get<int>() > 0) {
                            reservations[it.key()] += it.value().get<int>();
                        }
                    }
                }
            }
        }
    }
    for (const auto& child : node.children) collectBehaviorReservations(child, pack, reservations);
}

void validateBehaviorNodeReferences(const BehaviorNodeContent& node, const WorldContentPack& pack,
                                    const std::string& path) {
    if (node.type == "condition") {
        if (!pack.conditions.contains(node.name)) {
            throw std::runtime_error("behavior recipe references an undeclared condition: " + node.name);
        }
    } else if (node.type == "meta_action") {
        const auto definition = pack.metaActions.find(node.name);
        if (definition == pack.metaActions.end()) {
            throw std::runtime_error("behavior recipe references an undeclared meta action: " + node.name);
        }
        const auto args = mergedArgs(definition->second.defaultArgs, node.args);
        if (definition->second.operation.starts_with("resource.")) {
            const auto resource = args.value("resource", "");
            if (resource.empty() || !pack.resources.contains(resource)) {
                throw std::runtime_error("behavior recipe references an unknown resource in " + path);
            }
            if (!args.contains("amount") || !args.at("amount").is_number_integer() ||
                args.at("amount").get<int>() <= 0) {
                throw std::runtime_error("behavior resource action requires a positive amount in " + path);
            }
        } else if (definition->second.operation == "production.execute") {
            const auto inputs = args.value("inputs", Json::object());
            const auto outputs = args.value("outputs", Json::object());
            if (!inputs.is_object() || !outputs.is_object() || (inputs.empty() && outputs.empty())) {
                throw std::runtime_error("production action needs non-empty inputs or outputs in " + path);
            }
            for (const auto* collection : {&inputs, &outputs}) {
                for (auto it = collection->begin(); it != collection->end(); ++it) {
                    if (!pack.resources.contains(it.key()) || !it.value().is_number_integer() ||
                        it.value().get<int>() <= 0) {
                        throw std::runtime_error("production action references an invalid resource in " + path);
                    }
                }
            }
        } else if (definition->second.operation == "link.set_state") {
            const auto link = args.value("link", "");
            if (std::none_of(pack.links.begin(), pack.links.end(),
                             [&link](const auto& item) { return item.id == link; })) {
                throw std::runtime_error("behavior action references an unknown link: " + link);
            }
        } else if (definition->second.operation == "exchange.create") {
            const auto offered = args.value("offered", Json::object());
            const auto requested = args.value("requested", Json::object());
            if (!offered.is_object() || !requested.is_object() || offered.empty() || requested.empty()) {
                throw std::runtime_error("exchange.create needs non-empty terms on both sides in " + path);
            }
            for (const auto* terms : {&offered, &requested}) {
                for (auto term = terms->begin(); term != terms->end(); ++term) {
                    if (!pack.resources.contains(term.key()) || !term.value().is_number_integer() ||
                        term.value().get<int>() <= 0) {
                        throw std::runtime_error("exchange.create references invalid terms in " + path);
                    }
                }
            }
            if (args.value("counterparty", "").empty() || !args.contains("due_minutes") ||
                !args.at("due_minutes").is_number_integer() || args.at("due_minutes").get<int>() <= 0) {
                throw std::runtime_error("exchange.create needs a counterparty and positive due_minutes in " + path);
            }
        } else if (definition->second.operation == "exchange.transition" ||
                   definition->second.operation == "exchange.settle") {
            if (args.value("contract_id", "").empty()) {
                throw std::runtime_error("exchange transition needs contract_id in " + path);
            }
            if (definition->second.operation == "exchange.transition") {
                const auto status = args.value("status", "");
                if (status != "accepted" && status != "rejected" && status != "cancelled") {
                    throw std::runtime_error("exchange.transition has an invalid status in " + path);
                }
            }
        }
    }
    for (const auto& child : node.children) validateBehaviorNodeReferences(child, pack, path);
}

std::vector<ActionCandidate> parseActions(const Json& beats, const std::string& npcId,
                                          const WorldContentPack& pack) {
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
            const auto* recipe = findRecipe(pack, id);
            if (recipe == nullptr) {
                throw std::runtime_error("dialogue action has no behavior recipe: " + id);
            }
            const auto definition = behaviorSet.value(id, Json::object());
            if (!definition.is_object()) throw std::runtime_error("dialogue behavior must be an object: " + id);
            ActionCandidate candidate{id, recipe->planId, beat.value("id", "content"), recipe->durationMinutes,
                                      recipe->interruptible, recipe->interruptSalienceThreshold,
                                      recipe->reservationMode, recipe->id, {}, recipe->priority,
                                      recipe->preemptionPolicy};
            candidate.parameterSchema = recipe->parameterSchema;
            const auto utility = definition.value("utility_profile", Json::object());
            if (!utility.is_object()) throw std::runtime_error("utility_profile must be an object: " + id);
            const auto bounded = [&utility, &id](const char* key, double minimum, double maximum) {
                const double value = utility.value(key, 0.0);
                if (!std::isfinite(value) || value < minimum || value > maximum) {
                    throw std::runtime_error("utility_profile value is out of range: " + id + "/" + key);
                }
                return value;
            };
            candidate.resourceCost = bounded("resource_cost", 0.0, 1.0);
            candidate.risk = bounded("risk", 0.0, 1.0);
            candidate.relationshipEffect = bounded("relationship_effect", -1.0, 1.0);
            candidate.futureValue = bounded("future_value", -1.0, 1.0);
            candidate.constraintPenalty = bounded("constraint_penalty", 0.0, 1.0);
            collectBehaviorReservations(recipe->root, pack, candidate.reservations);
            result.push_back(std::move(candidate));
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
                                    const WorldContentPack& pack, const std::string& path) {
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
        // Domains are semantic content labels, not a Runtime enum.  The
        // village uses four human-motivation domains, while another world
        // may author ecological, institutional, spiritual, or entirely new
        // domains.  DriveSystem only consumes the numeric state contract.
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
            bool explicitRelief = false;
            for (const auto& metaAction : metaActions) {
                const auto definition = pack.metaActions.find(metaAction);
                if (definition == pack.metaActions.end()) {
                    throw std::runtime_error("need satisfier references an unavailable meta action: " + metaAction);
                }
                explicitRelief = explicitRelief || definition->second.operation == "need.relieve";
            }
            if (!explicitRelief) {
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

RelationshipAxes parseRelationshipAxes(const Json& value, const std::string& path) {
    if (!value.is_object()) throw std::runtime_error("relationship axes must be an object in " + path);
    const auto axis = [&value, &path](const char* key, double fallback) {
        if (!value.contains(key)) return fallback;
        if (!value.at(key).is_number() || value.at(key).get<double>() < 0.0 || value.at(key).get<double>() > 1.0) {
            throw std::runtime_error(std::string("relationship axis must be between 0 and 1: ") + key + " in " + path);
        }
        return value.at(key).get<double>();
    };
    return RelationshipAxes{axis("trust", 0.35), axis("respect", 0.35), axis("fear", 0.0), axis("owed", 0.0)};
}

SocialGroupContent parseSocialGroup(const Json& value, const std::string& path) {
    SocialGroupContent result;
    result.id = requiredString(value, "id", path);
    result.kind = requiredString(value, "kind", path);
    result.name = requiredString(value, "name", path);
    const auto metrics = value.value("metrics", Json::object());
    if (!metrics.is_object()) throw std::runtime_error("social group metrics must be an object in " + path);
    for (auto metric = metrics.begin(); metric != metrics.end(); ++metric) {
        if (!metric.value().is_number()) {
            throw std::runtime_error("social group metric must be numeric in " + path + ": " + metric.key());
        }
        result.metrics[metric.key()] = metric.value().get<double>();
    }
    const auto members = value.value("initial_members", Json::object());
    if (!members.is_object()) throw std::runtime_error("social group initial_members must be an object in " + path);
    for (auto member = members.begin(); member != members.end(); ++member) {
        if (!member.value().is_string()) {
            throw std::runtime_error("social group member role must be a string in " + path);
        }
        result.initialMembers[member.key()] = member.value().get<std::string>();
    }
    const auto relationships = value.value("initial_relationships", Json::object());
    if (!relationships.is_object()) {
        throw std::runtime_error("social group initial_relationships must be an object in " + path);
    }
    for (auto relationship = relationships.begin(); relationship != relationships.end(); ++relationship) {
        result.initialRelationships[relationship.key()] = parseRelationshipAxes(relationship.value(), path);
    }
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

NpcContent parseNpc(const Json& value, const Json& beats, const WorldContentPack& pack,
                    const std::string& path) {
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
    result.simulationTier = value.value("simulation_tier", "near");
    if (result.simulationTier != "near" && result.simulationTier != "far") {
        throw std::runtime_error("NPC simulation_tier must be near or far in " + path);
    }
    const auto peerTrust = initial.value("peer_trust", Json::object());
    for (auto it = peerTrust.begin(); it != peerTrust.end(); ++it) {
        result.initialPeerTrust[it.key()] = it.value().get<double>();
    }
    result.schedule = parseSchedule(value.value("schedule", Json::object()), path);
    result.actions = parseActions(beats, result.definition.id, pack);
    result.needs = parseNeeds(value, result.actions, pack, path);
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
    result.decisionChannel = value.value("decision_channel", "");
    if (!result.decisionChannel.empty() && result.actor == "world") {
        throw std::runtime_error("world activities cannot request an NPC decision channel in " + path);
    }
    if (!value.contains("effects") || !value.at("effects").is_array()) {
        throw std::runtime_error("activity needs an effects array in " + path);
    }
    for (const auto& effectValue : value.at("effects")) {
        EffectContent effect;
        effect.operation = requiredString(effectValue, "operation", path);
        if (effect.operation == "meta_action") {
            effect.metaActionId = requiredString(effectValue, "meta_action", path);
            effect.args = effectValue.value("args", Json::object());
            if (!effect.args.is_object()) {
                throw std::runtime_error("activity meta_action args must be an object in " + path);
            }
            result.effects.push_back(std::move(effect));
            continue;
        }
        if (!MetaActionRegistry::isExecutableOperator(effect.operation)) {
            throw std::runtime_error("unknown or unimplemented activity operator in " + path + ": " +
                                     effect.operation);
        }
        effect.resource = effectValue.value("resource", "");
        effect.amount = effectValue.value("amount", 0);
        effect.maximum = effectValue.value("maximum", 100);
        effect.effectAction = effectValue.value("action", "");
        effect.targets = effectValue.value("targets", std::vector<std::string>{});
        if ((effect.operation == "resource.consume" || effect.operation == "resource.produce" ||
             effect.operation == "resource.reserve" || effect.operation == "resource.release") &&
            effect.amount <= 0) {
            throw std::runtime_error("resource meta action needs a positive amount in " + path);
        }
        if (effect.operation == "need.relieve") {
            if (effect.effectAction.empty()) {
                throw std::runtime_error("relieve_need requires the committed action id in 'action' in " + path);
            }
            if (!effect.resource.empty() || effect.amount != 0 || !effect.targets.empty()) {
                throw std::runtime_error("relieve_need cannot write resources or arbitrary targets in " + path);
            }
        } else if (effect.operation == "link.set_state") {
            effect.linkId = requiredString(effectValue, "link", path);
            effect.traversable = effectValue.value("traversable", true);
            effect.locked = effectValue.value("locked", false);
            if (!effect.resource.empty() || effect.amount != 0 || !effect.targets.empty()) {
                throw std::runtime_error("set_link_state cannot write resources or NPC targets in " + path);
            }
        } else if (effect.resource.empty()) {
            throw std::runtime_error("resource meta action requires 'resource' in " + path);
        }
        if (effect.operation == "resource.consume" && !effect.targets.empty()) {
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
    if (manifest.contains("behaviors")) {
        const auto behaviorPath = rootPath + "/" + requiredString(manifest, "behaviors", manifestPath);
        parseBehaviorPack(pack, readJson(behaviorPath), behaviorPath);
    }
    const auto worldPath = rootPath + "/" + requiredString(manifest, "world", manifestPath);
    const auto world = readJson(worldPath);
    if (world.value("kind", "") != "world") {
        throw std::runtime_error("world content must declare kind=world: " + worldPath);
    }
    pack.contentVersion = requiredString(world, "content_version", worldPath);
    pack.worldId = requiredString(world, "id", worldPath);
    pack.sceneAsset = world.value("scene_asset", "");
    pack.cameraMode = world.value("camera", "slanted_top_down");
    if (pack.cameraMode.empty()) {
        throw std::runtime_error("world camera mode must not be empty: " + worldPath);
    }
    if (pack.cameraMode != "slanted_top_down" && pack.cameraMode != "top_down") {
        throw std::runtime_error("unsupported camera mode '" + pack.cameraMode + "' in " + worldPath);
    }
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
    if (!deliberation.contains("max_decision_age_minutes") ||
        !deliberation.at("max_decision_age_minutes").is_number_integer() ||
        deliberation.at("max_decision_age_minutes").get<int>() <= 0) {
        throw std::runtime_error("npc_deliberation.max_decision_age_minutes must be positive in " + worldPath);
    }
    pack.npcDeliberation.maxDecisionAgeMinutes = deliberation.at("max_decision_age_minutes").get<int>();
    if (!deliberation.contains("coalesce_pending") || !deliberation.at("coalesce_pending").is_boolean()) {
        throw std::runtime_error("npc_deliberation.coalesce_pending must be boolean in " + worldPath);
    }
    pack.npcDeliberation.coalescePending = deliberation.at("coalesce_pending").get<bool>();
    if (pack.npcDeliberation.minStimulusSalience < 0.0 || pack.npcDeliberation.minStimulusSalience > 1.0 ||
        pack.npcDeliberation.minDrivePressure < 0.0 || pack.npcDeliberation.minDrivePressure > 1.0 ||
        pack.npcDeliberation.minInterruptSalience < 0.0 || pack.npcDeliberation.minInterruptSalience > 1.0) {
        throw std::runtime_error("npc_deliberation values are out of range in " + worldPath);
    }
    const auto simulationPolicy = world.value("simulation_policy", Json::object());
    if (!simulationPolicy.is_object()) throw std::runtime_error("simulation_policy must be an object in " + worldPath);
    if (simulationPolicy.contains("near_tick_minutes")) {
        const auto& value = simulationPolicy.at("near_tick_minutes");
        if (!value.is_number_integer() || value.get<int>() <= 0 || value.get<int>() > 1440) {
            throw std::runtime_error("simulation_policy.near_tick_minutes must be between 1 and 1440");
        }
        pack.simulationPolicy.nearTickMinutes = value.get<int>();
    }
    if (simulationPolicy.contains("far_tick_minutes")) {
        const auto& value = simulationPolicy.at("far_tick_minutes");
        if (!value.is_number_integer() || value.get<int>() <= 0 || value.get<int>() > 1440) {
            throw std::runtime_error("simulation_policy.far_tick_minutes must be between 1 and 1440");
        }
        pack.simulationPolicy.farTickMinutes = value.get<int>();
    }
    if (pack.simulationPolicy.farTickMinutes < pack.simulationPolicy.nearTickMinutes) {
        throw std::runtime_error("simulation_policy.far_tick_minutes must not be smaller than near_tick_minutes");
    }
    const auto evolutionPolicy = world.value("evolution_policy", Json::object());
    if (!evolutionPolicy.is_object()) throw std::runtime_error("evolution_policy must be an object in " + worldPath);
    if (evolutionPolicy.contains("enabled")) {
        if (!evolutionPolicy.at("enabled").is_boolean()) {
            throw std::runtime_error("evolution_policy.enabled must be boolean in " + worldPath);
        }
        pack.evolutionPolicy.enabled = evolutionPolicy.at("enabled").get<bool>();
    }
    const auto readEvolutionDays = [&evolutionPolicy, &worldPath](const char* key) {
        if (!evolutionPolicy.contains(key)) return 0;
        const auto& value = evolutionPolicy.at(key);
        if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 3650) {
            throw std::runtime_error(std::string("evolution_policy.") + key +
                                     " must be between 0 and 3650 in " + worldPath);
        }
        return value.get<int>();
    };
    pack.evolutionPolicy.level1IntervalDays = readEvolutionDays("level1_interval_days");
    pack.evolutionPolicy.level2IntervalDays = readEvolutionDays("level2_interval_days");
    if (evolutionPolicy.contains("max_proposal_age_minutes")) {
        const auto& value = evolutionPolicy.at("max_proposal_age_minutes");
        if (!value.is_number_integer() || value.get<int>() <= 0 || value.get<int>() > 525600) {
            throw std::runtime_error("evolution_policy.max_proposal_age_minutes must be between 1 and 525600 in " + worldPath);
        }
        pack.evolutionPolicy.maxProposalAgeMinutes = value.get<int>();
    }
    if (evolutionPolicy.contains("coalesce_pending")) {
        if (!evolutionPolicy.at("coalesce_pending").is_boolean()) {
            throw std::runtime_error("evolution_policy.coalesce_pending must be boolean in " + worldPath);
        }
        pack.evolutionPolicy.coalescePending = evolutionPolicy.at("coalesce_pending").get<bool>();
    }
    if (!pack.evolutionPolicy.enabled) {
        pack.evolutionPolicy.level1IntervalDays = 0;
        pack.evolutionPolicy.level2IntervalDays = 0;
    }
    if (!world.contains("conversation_policy") || !world.at("conversation_policy").is_object()) {
        throw std::runtime_error("world must define a conversation_policy: " + worldPath);
    }
    const auto& conversation = world.at("conversation_policy");
    if (!conversation.contains("default_world_action_allowed") ||
        !conversation.at("default_world_action_allowed").is_boolean()) {
        throw std::runtime_error("conversation_policy.default_world_action_allowed must be boolean in " + worldPath);
    }
    pack.conversationPolicy.defaultWorldActionAllowed =
        conversation.at("default_world_action_allowed").get<bool>();
    if (!conversation.contains("speech_acts") || !conversation.at("speech_acts").is_array()) {
        throw std::runtime_error("conversation_policy.speech_acts must be an array in " + worldPath);
    }
    std::set<std::string> speechActIds;
    for (const auto& item : conversation.at("speech_acts")) {
        SpeechActRule rule;
        const auto id = requiredString(item, "id", worldPath);
        if (!speechActIds.insert(id).second) throw std::runtime_error("duplicate speech act: " + id);
        rule.act = speechActFromString(id);
        if (rule.act == SpeechAct::Unknown) throw std::runtime_error("unsupported speech act: " + id);
        if (!item.contains("world_action_allowed") || !item.at("world_action_allowed").is_boolean()) {
            throw std::runtime_error("speech act world_action_allowed must be boolean: " + id);
        }
        rule.worldActionAllowed = item.at("world_action_allowed").get<bool>();
        rule.description = requiredString(item, "description", worldPath);
        pack.conversationPolicy.rules.push_back(std::move(rule));
    }
    if (!world.contains("decision_channels") || !world.at("decision_channels").is_array()) {
        throw std::runtime_error("world must define decision_channels: " + worldPath);
    }
    for (const auto& item : world.at("decision_channels")) {
        DecisionChannelPolicy channel;
        channel.id = requiredString(item, "id", worldPath);
        if (pack.decisionChannels.contains(channel.id)) {
            throw std::runtime_error("duplicate decision channel: " + channel.id);
        }
        if (!item.contains("conversation") || !item.at("conversation").is_boolean() ||
            !item.contains("classify_speech_act") || !item.at("classify_speech_act").is_boolean() ||
            !item.contains("default_world_action_allowed") ||
            !item.at("default_world_action_allowed").is_boolean()) {
            throw std::runtime_error("decision channel requires boolean conversation/classify_speech_act/default_world_action_allowed: " + channel.id);
        }
        channel.conversation = item.at("conversation").get<bool>();
        channel.classifySpeechAct = item.at("classify_speech_act").get<bool>();
        channel.defaultWorldActionAllowed = item.at("default_world_action_allowed").get<bool>();
        channel.allowInterveningWorldChanges = item.value("allow_intervening_world_changes", false);
        channel.defaultSpeechAct = item.value("default_speech_act", channel.defaultSpeechAct);
        channel.eventType = item.value("event_type", channel.eventType);
        channel.memoryKind = item.value("memory_kind", channel.memoryKind);
        channel.eventTarget = item.value("event_target", channel.eventTarget);
        channel.providerPolicy = requiredString(item, "provider_policy", worldPath);
        pack.decisionChannels.emplace(channel.id, std::move(channel));
    }
    pack.conversationChannelId = requiredString(world, "conversation_channel", worldPath);
    pack.autonomousChannelId = requiredString(world, "autonomous_channel", worldPath);
    const auto conversationChannel = pack.decisionChannels.find(pack.conversationChannelId);
    const auto autonomousChannel = pack.decisionChannels.find(pack.autonomousChannelId);
    if (conversationChannel == pack.decisionChannels.end() || !conversationChannel->second.conversation) {
        throw std::runtime_error("conversation_channel must reference a conversation decision channel: " + worldPath);
    }
    if (autonomousChannel == pack.decisionChannels.end() || autonomousChannel->second.conversation) {
        throw std::runtime_error("autonomous_channel must reference a non-conversation decision channel: " + worldPath);
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
    const auto socialGroups = world.value("social_groups", Json::array());
    if (!socialGroups.is_array()) throw std::runtime_error("social_groups must be an array: " + worldPath);
    std::set<std::string> socialGroupIds;
    for (const auto& group : socialGroups) {
        auto parsed = parseSocialGroup(group, worldPath);
        if (!socialGroupIds.insert(parsed.id).second) {
            throw std::runtime_error("duplicate social group id: " + parsed.id);
        }
        pack.socialGroups.push_back(std::move(parsed));
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
    const auto anomalyRules = world.value("anomaly_rules", Json::array());
    if (!anomalyRules.is_array()) throw std::runtime_error("anomaly_rules must be an array: " + worldPath);
    std::set<std::string> anomalyRuleIds;
    for (const auto& item : anomalyRules) {
        AnomalyRuleContent rule;
        rule.id = requiredString(item, "id", worldPath);
        if (!anomalyRuleIds.insert(rule.id).second) {
            throw std::runtime_error("duplicate anomaly rule: " + rule.id);
        }
        rule.operation = requiredString(item, "operation", worldPath);
        static const std::set<std::string> operations{
            "resource.below", "resource.above", "need.above", "group_metric.below",
            "group_metric.above", "promise.overdue"};
        if (!operations.contains(rule.operation)) {
            throw std::runtime_error("unsupported anomaly operation: " + rule.operation);
        }
        rule.subject = item.value("subject", "");
        rule.metric = item.value("metric", "");
        if (rule.operation != "promise.overdue" && rule.subject.empty()) {
            throw std::runtime_error("anomaly rule needs a subject: " + rule.id);
        }
        if (!item.contains("threshold") || !item.at("threshold").is_number()) {
            throw std::runtime_error("anomaly rule needs a numeric threshold: " + rule.id);
        }
        rule.threshold = item.at("threshold").get<double>();
        if (!std::isfinite(rule.threshold)) {
            throw std::runtime_error("anomaly rule threshold must be finite: " + rule.id);
        }
        rule.severity = item.value("severity", "warning");
        if (rule.severity != "info" && rule.severity != "warning" && rule.severity != "critical") {
            throw std::runtime_error("unsupported anomaly severity: " + rule.severity);
        }
        rule.description = item.value("description", rule.id);
        pack.anomalyRules.push_back(std::move(rule));
    }
    const auto dialoguePath = rootPath + "/" + requiredString(manifest, "dialogue", manifestPath);
    const auto beats = readJson(dialoguePath);
    for (const auto& recipe : pack.behaviorRecipes) {
        validateBehaviorNodeReferences(recipe.root, pack, manifestPath);
    }
    for (const auto& npcPathValue : manifest.at("npcs")) {
        const auto relativePath = npcPathValue.get<std::string>();
        const auto path = rootPath + "/" + relativePath;
        auto npc = parseNpc(readJson(path), beats, pack, path);
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
            if (effect.operation == "meta_action") {
                if (activity.actor == "world" || activity.actor.empty()) {
                    throw std::runtime_error("meta_action activity effects require an NPC actor: " + activity.id);
                }
                if (!pack.metaActions.contains(effect.metaActionId)) {
                    throw std::runtime_error("activity references an unknown meta action: " + effect.metaActionId);
                }
                continue;
            }
            if (effect.operation != "need.relieve" && effect.operation != "link.set_state" &&
                !pack.resources.contains(effect.resource)) {
                throw std::runtime_error("activity effect references an unknown resource: " + effect.resource);
            }
            if (effect.operation == "link.set_state") {
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
            if (effect.operation == "need.relieve") {
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
    for (const auto& rule : pack.anomalyRules) {
        if (rule.operation.starts_with("resource.") && !pack.resources.contains(rule.subject)) {
            throw std::runtime_error("anomaly rule references an unknown resource: " + rule.id);
        }
        if (rule.operation.starts_with("need.")) {
            if (!npcIds.contains(rule.subject)) {
                throw std::runtime_error("anomaly rule references an unknown NPC: " + rule.id);
            }
            const auto npc = std::find_if(pack.npcs.begin(), pack.npcs.end(), [&rule](const auto& item) {
                return item.definition.id == rule.subject;
            });
            const bool knownNeed = npc != pack.npcs.end() && std::any_of(
                npc->needs.begin(), npc->needs.end(), [&rule](const auto& need) { return need.id == rule.metric; });
            if (!knownNeed) throw std::runtime_error("anomaly rule references an unknown need: " + rule.id);
        }
        if (rule.operation.starts_with("group_metric.")) {
            const auto group = std::find_if(pack.socialGroups.begin(), pack.socialGroups.end(),
                                            [&rule](const auto& item) { return item.id == rule.subject; });
            if (group == pack.socialGroups.end() || !group->metrics.contains(rule.metric)) {
                throw std::runtime_error("anomaly rule references an unknown group metric: " + rule.id);
            }
        }
    }
    for (const auto& group : pack.socialGroups) {
        for (const auto& [npcId, role] : group.initialMembers) {
            (void)role;
            if (!npcIds.contains(npcId)) {
                throw std::runtime_error("social group references an unknown NPC: " + group.id + "/" + npcId);
            }
        }
        for (const auto& [target, relation] : group.initialRelationships) {
            (void)relation;
            if (!socialGroupIds.contains(target) && !npcIds.contains(target)) {
                throw std::runtime_error("social group relationship references an unknown target: " +
                                         group.id + "/" + target);
            }
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
        const auto inventories = instanceJson.value("inventories", Json::object());
        if (!inventories.is_object()) {
            throw std::runtime_error("game instance inventories must be an object: " + instancePath);
        }
        for (auto actor = inventories.begin(); actor != inventories.end(); ++actor) {
            if (actor.key() != "player" && !npcIds.contains(actor.key())) {
                throw std::runtime_error("instance inventory references an unknown actor: " + actor.key());
            }
            if (!actor.value().is_object()) {
                throw std::runtime_error("instance inventory entry must be an object: " + actor.key());
            }
            for (auto resource = actor.value().begin(); resource != actor.value().end(); ++resource) {
                if (!pack.resources.contains(resource.key())) {
                    throw std::runtime_error("instance inventory references an unknown resource: " + resource.key());
                }
                if (!resource.value().is_number_integer() || resource.value().get<int>() < 0) {
                    throw std::runtime_error("instance inventory amount must be a non-negative integer: " +
                                             actor.key() + "/" + resource.key());
                }
                instance.inventoryInitial[actor.key()][resource.key()] = resource.value().get<int>();
            }
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
