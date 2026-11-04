#include "provider/decision_context_projector.hpp"

#include "npc/drive_system.hpp"
#include "npc/goal_selector.hpp"
#include "world/ids.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string_view>

namespace tide_marks {

namespace {

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::ostringstream output;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) output << separator;
        output << values[i];
    }
    return output.str();
}

std::string joinRecent(const std::vector<std::string>& values, std::size_t limit,
                       std::string_view separator) {
    if (values.empty()) return {};
    const auto first = values.size() > limit ? values.size() - limit : 0;
    std::vector<std::string> recent;
    recent.reserve(values.size() - first);
    for (std::size_t index = first; index < values.size(); ++index) recent.push_back(values[index]);
    return join(recent, separator);
}

Json quantityMapJson(const std::map<std::string, int>& quantities) {
    Json result = Json::object();
    for (const auto& [resource, amount] : quantities) result[resource] = amount;
    return result;
}

Json behaviorNodeProjection(const BehaviorNodeContent& node, int depth = 0) {
    Json result{{"type", node.type}, {"id", node.id}};
    if (!node.name.empty()) result["name"] = node.name;
    if (!node.args.empty()) result["args"] = node.args;
    if (node.maxTicks > 0) result["max_ticks"] = node.maxTicks;
    if (depth < 4 && !node.children.empty()) {
        result["children"] = Json::array();
        const std::size_t limit = std::min<std::size_t>(node.children.size(), 8);
        for (std::size_t index = 0; index < limit; ++index) {
            result["children"].push_back(behaviorNodeProjection(node.children[index], depth + 1));
        }
        if (node.children.size() > limit) result["children_truncated"] = true;
    }
    return result;
}

const NpcContent* authoredNpc(const WorldContentPack& content, const std::string& npcId) {
    const auto found = std::find_if(content.npcs.begin(), content.npcs.end(),
                                    [&npcId](const auto& item) { return item.definition.id == npcId; });
    return found == content.npcs.end() ? nullptr : &*found;
}

bool activeNpc(const WorldState& state, const std::string& npcId) {
    if (!state.npcs.contains(npcId)) return false;
    const auto entity = state.entities.find(npcEntityId(npcId));
    return entity == state.entities.end() || entity->second.lifecycle == EntityLifecycle::Active;
}

}  // namespace

DecisionContext DecisionContextProjector::build(const WorldState& state,
                                                const WorldContentPack& content,
                                                const std::string& npcId,
                                                std::uint64_t worldRevision,
                                                std::string promptVersion) {
    DecisionContext context;
    if (!activeNpc(state, npcId)) return context;
    const auto npcFound = state.npcs.find(npcId);
    const auto* authoredContent = authoredNpc(content, npcId);
    if (npcFound == state.npcs.end() || authoredContent == nullptr) return context;

    const auto& npc = npcFound->second;
    const auto& authoredDefinition = authoredContent->definition;
    context.npcId = npcId;
    context.npcName = authoredDefinition.name;
    context.contentVersion = content.contentVersion;
    context.promptVersion = std::move(promptVersion);
    context.now = state.now;
    context.worldRevision = worldRevision;
    context.npcStateVersion = npc.stateVersion;
    context.runtimeMetadata = Json{
        {"world_id", state.worldId},
        {"instance_id", state.instanceId},
        {"content_version", content.contentVersion},
        {"simulation_version", kSimulationVersion},
        {"prompt_version", context.promptVersion},
        {"world_revision", worldRevision},
        {"npc_state_version", npc.stateVersion}};

    const auto ownerIsNpc = [&state](const std::string& owner) {
        if (state.npcs.contains(owner)) return true;
        return owner.starts_with("npc:") && state.npcs.contains(owner.substr(4));
    };
    const auto resourceVisible = [&npc, &npcId, &ownerIsNpc](const ResourceContent& definition) {
        const bool locationVisible = definition.location.empty() || definition.location == npc.location;
        const bool ownerVisible = !ownerIsNpc(definition.owner) || definition.owner == npcId ||
                                  definition.owner == npcEntityId(npcId);
        return locationVisible && ownerVisible;
    };
    const auto resourceValue = [&state](const std::string& resource) {
        const auto found = state.resources.find(resource);
        return found == state.resources.end() ? 0 : found->second.amount;
    };
    std::ostringstream resources;
    for (const auto& [id, resourceDefinition] : content.resources) {
        if (!resourceVisible(resourceDefinition)) continue;
        resources << id << '=' << resourceValue(id) << ' ' << resourceDefinition.unit << ';';
    }
    context.perception = "At " + npc.location + ", it is " + state.now.toString() +
                         ". Public resources: " + resources.str();
    for (const auto& stimulus : npc.stimuli) {
        if (!stimulus.active || state.now.minute >= stimulus.expiresAt.minute) continue;
        context.stimuli.push_back(stimulus);
    }
    std::sort(context.stimuli.begin(), context.stimuli.end(), [](const auto& left, const auto& right) {
        if (left.effectiveSalience == right.effectiveSalience) return left.id < right.id;
        return left.effectiveSalience > right.effectiveSalience;
    });
    if (context.stimuli.size() > content.decisionContextPolicy.maxStimuli) {
        context.stimuli.resize(content.decisionContextPolicy.maxStimuli);
    }
    std::ostringstream stimuli;
    for (const auto& stimulus : context.stimuli) {
        stimuli << stimulus.category << " from " << stimulus.sourceEvent << " by " << stimulus.actor
                << " salience=" << stimulus.effectiveSalience << " valence=" << stimulus.valence
                << " sequence=" << stimulus.sourceSequence << ';';
    }
    if (stimuli.str().empty()) stimuli << "none beyond the current public state;";
    context.externalStimuli = stimuli.str();

    for (const auto& [resourceId, resource] : state.resources) {
        const auto definition = content.resources.find(resourceId);
        if (definition == content.resources.end() || !resourceVisible(definition->second)) continue;
        if (resource.amount - resource.reserved > 0) continue;
        context.constraints.push_back(StructuralConstraintView{
            "resource:" + resourceId + ":unavailable", "resource", resourceId,
            "world_resource_rule", "No unreserved units are available.", true});
    }
    for (const auto& link : content.links) {
        const auto runtime = state.links.find(link.id);
        if (runtime == state.links.end() || (runtime->second.traversable && !runtime->second.locked)) continue;
        context.constraints.push_back(StructuralConstraintView{
            linkEntityId(link.id) + ":blocked", "topology", join(link.link.endpoints, ","),
            "authored_link_state", runtime->second.locked ? "The link is locked." : "The link is closed.", true});
    }
    context.constraints.push_back(StructuralConstraintView{
        "actions:authored_vocabulary", "capability", npcId, "content_action_whitelist",
        "Only the supplied authored action ids may be proposed.", true});
    std::ostringstream constraints;
    for (const auto& constraint : context.constraints) {
        constraints << constraint.id << " [" << (constraint.hard ? "hard" : "soft") << "] "
                    << constraint.reason << ';';
    }
    context.structuralConstraints = constraints.str();

    std::vector<std::string> memories;
    for (const auto& memory : npc.memories) memories.push_back(memory.text);
    context.memories = joinRecent(memories, content.decisionContextPolicy.maxMemories, " | ");
    std::vector<std::string> knowledge;
    for (const auto& fact : npc.knowledge) {
        knowledge.push_back(fact.subject + " " + fact.predicate + " " + fact.value +
                           " [confidence=" + std::to_string(fact.confidence) + ", scope=" + fact.scope + "]");
    }
    context.knowledge = joinRecent(knowledge, content.decisionContextPolicy.maxKnowledge, " | ");
    std::vector<std::string> experiences;
    for (const auto& experience : npc.experiences) {
        experiences.push_back(experience.domain + ": when " + experience.trigger + ", " +
                              experience.action + " -> " + experience.outcome +
                              " [confidence=" + std::to_string(experience.confidence) + "]");
    }
    context.experiences = joinRecent(experiences, content.decisionContextPolicy.maxExperiences, " | ");
    context.character = authoredDefinition.name + ": " + authoredDefinition.role + "; public goal=" +
                        authoredDefinition.publicGoal;
    context.privateCharacter = "hidden goal=" + authoredDefinition.hiddenGoal +
                               "; boundary=" + authoredDefinition.boundary +
                               "; strategy=" + npc.strategy;
    for (const auto& [needId, need] : npc.needs) {
        context.character += "; need(" + needId + ")=" + std::to_string(need.deficit);
    }
    std::size_t relationshipCount = 0;
    for (const auto& [peer, relation] : npc.relationships) {
        if (relationshipCount++ >= content.decisionContextPolicy.maxRelationships) break;
        context.character += "; relationship(" + peer + ")=trust:" + std::to_string(relation.trust) +
                             ",respect:" + std::to_string(relation.respect) +
                             ",fear:" + std::to_string(relation.fear) +
                             ",owed:" + std::to_string(relation.owed);
    }
    if (npc.activity.goal) {
        context.currentActivity = Json{{"status", toString(npc.activity.status)},
                                       {"goal_id", npc.activity.goal->id},
                                       {"intent", npc.activity.goal->intent},
                                       {"action_id", npc.activity.goal->target},
                                       {"reason", npc.activity.goal->reason},
                                       {"deadline", npc.activity.goal->deadline.toString()}};
    }
    if (npc.activity.process) {
        context.currentActivity["process_id"] = npc.activity.process->id;
        context.currentActivity["recipe_id"] = npc.activity.process->recipeId;
        context.currentActivity["plan_id"] = npc.activity.process->planId;
        context.currentActivity["progress"] = npc.activity.process->progress;
        context.currentActivity["behavior_status"] = npc.activity.process->behaviorStatus;
        context.currentActivity["completes_at"] = npc.activity.process->completesAt.toString();
    }
    for (const auto& [groupId, group] : state.socialGroups) {
        const auto membership = group.members.find(npcId);
        if (membership == group.members.end()) continue;
        context.character += "; membership(" + groupId + ")=" + membership->second;
        for (const auto& [metric, value] : group.metrics) {
            context.character += "; group_metric(" + groupId + ":" + metric + ")=" + std::to_string(value);
        }
    }

    std::vector<NeedContent> needs = authoredContent->needs;
    context.candidates = authoredContent->actions;
    // Approved evolved recipes carry explicit actor propagation. An empty
    // allow-list is never interpreted as "all NPCs" by this projection.
    for (const auto& recipe : content.behaviorRecipes) {
        if (recipe.allowedActors.empty() ||
            std::find(recipe.allowedActors.begin(), recipe.allowedActors.end(), npcId) == recipe.allowedActors.end()) {
            continue;
        }
        if (std::any_of(context.candidates.begin(), context.candidates.end(), [&recipe](const auto& candidate) {
                return candidate.actionId == recipe.id || candidate.recipeId == recipe.id;
            })) {
            continue;
        }
        ActionCandidate candidate;
        candidate.actionId = recipe.id;
        candidate.planId = recipe.planId;
        candidate.label = recipe.planId.empty() ? recipe.id : recipe.planId;
        candidate.durationMinutes = recipe.durationMinutes;
        candidate.interruptible = recipe.interruptible;
        candidate.interruptSalienceThreshold = recipe.interruptSalienceThreshold;
        candidate.reservationMode = recipe.reservationMode;
        candidate.recipeId = recipe.id;
        candidate.reservations = recipe.reservations;
        candidate.priority = recipe.priority;
        candidate.preemptionPolicy = recipe.preemptionPolicy;
        candidate.parameterSchema = recipe.parameterSchema;
        context.candidates.push_back(std::move(candidate));
    }
    for (const auto& needDefinition : needs) {
        const auto need = npc.needs.find(needDefinition.id);
        if (need == npc.needs.end()) continue;
        const double currentDrive = DriveSystem::drive(needDefinition, need->second);
        context.internalDrives += (context.internalDrives.empty() ? "" : "; ") + needDefinition.id +
                                  "[" + needDefinition.domain + "] deficit=" +
                                  std::to_string(need->second.deficit) + " drive=" + std::to_string(currentDrive);
        context.character += "; drive(" + needDefinition.id + ")=" + std::to_string(currentDrive);
        context.drives.push_back(DriveView{needDefinition.id, needDefinition.domain, needDefinition.dimension,
                                           need->second.deficit, currentDrive, need->second.sensitivity,
                                           need->second.evidenceConfidence,
                                           need->second.deficit >= needDefinition.criticalThreshold,
                                           need->second.expectedRelief});
    }

    const auto ranked = GoalSelector::rank(npc, needs, context.candidates);
    std::vector<ActionCandidate> ordered;
    for (const auto& choice : ranked) {
        const auto candidate = std::find_if(context.candidates.begin(), context.candidates.end(),
                                            [&choice](const auto& item) { return item.actionId == choice.actionId; });
        if (candidate == context.candidates.end()) continue;
        auto rankedCandidate = *candidate;
        rankedCandidate.utilityScore = choice.score;
        for (const auto& [needId, contribution] : choice.needContributions) {
            rankedCandidate.utilityContributions[needId] = contribution;
        }
        for (const auto& [term, contribution] : choice.utilityTerms) {
            rankedCandidate.utilityContributions[term] = contribution;
        }
        ordered.push_back(std::move(rankedCandidate));
    }
    for (const auto& candidate : context.candidates) {
        if (std::none_of(ordered.begin(), ordered.end(), [&candidate](const auto& item) {
                return item.actionId == candidate.actionId;
            })) {
            ordered.push_back(candidate);
        }
    }
    context.candidates = std::move(ordered);

    context.worldSnapshot = Json{{"time", state.now.toString()},
                                 {"observer_location", npc.location},
                                 {"public_resources", Json::object()},
                                 {"visible_entities", Json::array()},
                                 {"visible_links", Json::array()},
                                 {"active_stimuli", Json::array()}};
    for (const auto& [resourceId, resource] : state.resources) {
        const auto definition = content.resources.find(resourceId);
        if (definition == content.resources.end() || !resourceVisible(definition->second)) continue;
        const auto& authoredResource = definition->second;
        context.worldSnapshot["public_resources"][resourceId] =
            Json{{"amount", resource.amount}, {"reserved", resource.reserved},
                 {"location", authoredResource.location}, {"unit", authoredResource.unit},
                 {"kind", authoredResource.kind}};
    }
    std::size_t visibleEntityCount = 0;
    for (const auto& [peerId, peer] : state.npcs) {
        if (peerId == npcId || peer.location != npc.location) continue;
        if (visibleEntityCount++ >= content.decisionContextPolicy.maxVisibleEntities) break;
        context.worldSnapshot["visible_entities"].push_back(
            Json{{"id", peerId}, {"kind", "npc"}, {"location", peer.location}, {"mode", peer.mode}});
    }
    std::size_t visibleLinkCount = 0;
    for (const auto& link : content.links) {
        if (std::find(link.link.endpoints.begin(), link.link.endpoints.end(), npc.location) == link.link.endpoints.end()) {
            continue;
        }
        if (visibleLinkCount++ >= content.decisionContextPolicy.maxVisibleLinks) break;
        const auto runtime = state.links.find(link.id);
        const RuntimeLinkState linkState = runtime == state.links.end() ? RuntimeLinkState{} : runtime->second;
        context.worldSnapshot["visible_links"].push_back(
            Json{{"id", link.id}, {"endpoints", link.link.endpoints},
                 {"traversable", linkState.traversable}, {"locked", linkState.locked}});
    }
    for (const auto& stimulus : context.stimuli) {
        context.worldSnapshot["active_stimuli"].push_back(
            Json{{"id", stimulus.id}, {"category", stimulus.category},
                 {"source_sequence", stimulus.sourceSequence},
                 {"salience", stimulus.effectiveSalience}, {"expires", stimulus.expiresAt.toString()}});
    }

    const auto inventory = state.inventories.find(npcEntityId(npcId));
    const auto inventoryQuantities = inventory == state.inventories.end()
                                         ? std::map<std::string, int>{}
                                         : inventory->second;
    context.selfState = Json{{"id", npcId}, {"name", authoredDefinition.name},
                             {"role", authoredDefinition.role}, {"location", npc.location},
                             {"mode", npc.mode}, {"strategy", npc.strategy},
                             {"goal_stage", npc.goalStage}, {"state_version", npc.stateVersion},
                             {"needs", Json::object()}, {"relationships", Json::object()},
                             {"inventory", quantityMapJson(inventoryQuantities)},
                             {"activity", context.currentActivity}};
    for (const auto& [needId, need] : npc.needs) {
        const auto authoredNeed = std::find_if(needs.begin(), needs.end(),
                                               [&needId](const auto& item) { return item.id == needId; });
        const double drive = authoredNeed == needs.end() ? 0.0 : DriveSystem::drive(*authoredNeed, need);
        context.selfState["needs"][needId] = Json{{"deficit", need.deficit}, {"drive", drive},
                                                    {"sensitivity", need.sensitivity},
                                                    {"confidence", need.evidenceConfidence}};
    }
    std::size_t selfRelationshipCount = 0;
    for (const auto& [peerId, relation] : npc.relationships) {
        if (selfRelationshipCount++ >= content.decisionContextPolicy.maxRelationships) break;
        context.selfState["relationships"][peerId] =
            Json{{"trust", relation.trust}, {"respect", relation.respect},
                 {"fear", relation.fear}, {"owed", relation.owed}};
    }
    for (const auto& candidate : context.candidates) {
        Json plan{{"action_id", candidate.actionId}, {"plan_id", candidate.planId},
                  {"recipe_id", candidate.recipeId}, {"label", candidate.label},
                  {"duration_minutes", candidate.durationMinutes},
                  {"interruptible", candidate.interruptible}, {"priority", candidate.priority},
                  {"preemption_policy", candidate.preemptionPolicy},
                  {"utility_score", candidate.utilityScore},
                  {"utility_contributions", candidate.utilityContributions},
                  {"reservations", candidate.reservations},
                  {"parameter_schema", candidate.parameterSchema}};
        const auto recipe = std::find_if(content.behaviorRecipes.begin(), content.behaviorRecipes.end(),
                                         [&candidate](const auto& item) { return item.id == candidate.recipeId; });
        if (recipe != content.behaviorRecipes.end()) plan["behavior_tree"] = behaviorNodeProjection(recipe->root);
        context.planCatalog.push_back(std::move(plan));
    }
    return context;
}

}  // namespace tide_marks
