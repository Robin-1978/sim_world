#include "provider/decision_context_projector.hpp"

#include "npc/behavior_recipe.hpp"
#include "npc/drive_system.hpp"
#include "npc/goal_selector.hpp"
#include "world/ids.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
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

Json quantityMapJson(const std::map<std::string, int>& quantities) {
    Json result = Json::object();
    for (const auto& [resource, amount] : quantities) result[resource] = amount;
    return result;
}

const SpatialContent* spatialFor(const WorldContentPack& content, const std::string& locationId) {
    const auto found = std::find_if(content.spatialEntities.begin(), content.spatialEntities.end(),
                                    [&locationId](const auto& item) { return item.id == locationId; });
    return found == content.spatialEntities.end() ? nullptr : &*found;
}

Json locationView(const WorldContentPack& content, const std::string& locationId) {
    const auto* spatial = spatialFor(content, locationId);
    if (spatial == nullptr) return Json{{"name", locationId}, {"type", "place"}, {"tags", Json::array()}};
    return Json{{"name", spatial->spatial.displayName}, {"type", spatial->kind},
                {"tags", spatial->spatial.tags}};
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

std::string behaviorNodeExplanation(const BehaviorNodeContent& node,
                                    const WorldContentPack& content, int depth = 0) {
    if (depth > 4) return "continue the authored behavior";
    if (node.type == "sequence" || node.type == "selector") {
        std::vector<std::string> children;
        const auto limit = std::min<std::size_t>(node.children.size(), 8);
        for (std::size_t index = 0; index < limit; ++index) {
            children.push_back(behaviorNodeExplanation(node.children.at(index), content, depth + 1));
        }
        if (children.empty()) return node.type == "sequence" ? "complete the authored sequence"
                                                                : "try an authored option";
        return (node.type == "sequence" ? "then " : "or ") + join(children, node.type == "sequence" ? ", then " : ", or ");
    }
    if (node.type == "condition") {
        const auto found = content.conditions.find(node.name);
        if (found != content.conditions.end() && !found->second.operation.empty()) {
            return "check the authored condition " + node.name + " (" + found->second.operation + ")";
        }
        return "check the authored condition " + node.name;
    }
    if (node.type == "meta_action") {
        const auto found = content.metaActions.find(node.name);
        if (found != content.metaActions.end() && !found->second.description.empty()) {
            return found->second.description;
        }
        return "perform the authored operation " + node.name;
    }
    if (!node.name.empty()) return "run the authored " + node.name + " step";
    return "run the authored behavior step";
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

const NpcContent* findNpcContent(const WorldContentPack& content, const std::string& npcId) {
    return authoredNpc(content, npcId);
}

bool evaluateCondition(const WorldState& state, const WorldContentPack& content,
                      const std::string& npcId, const BehaviorNodeContent& node) {
    const auto definition = content.conditions.find(node.name);
    if (definition == content.conditions.end()) return false;
    Json args = definition->second.args;
    for (auto item = node.args.begin(); item != node.args.end(); ++item) args[item.key()] = item.value();
    const auto operation = definition->second.operation;
    if (operation == "always") return true;
    if (operation == "resource.available") {
        const auto resource = state.resources.find(args.value("resource", ""));
        const int available = resource == state.resources.end() ? 0 : resource->second.amount - resource->second.reserved;
        return available >= args.value("minimum", 1);
    }
    if (operation == "resource.below") {
        const auto resource = state.resources.find(args.value("resource", ""));
        const int amount = resource == state.resources.end() ? 0 : resource->second.amount;
        return amount < args.value("below", 1);
    }
    if (operation == "link.traversable" || operation == "link.locked") {
        const auto link = state.links.find(args.value("link", ""));
        const bool traversable = link == state.links.end() ||
                                 (link->second.traversable && !link->second.locked);
        return operation == "link.traversable" ? traversable : !traversable;
    }
    if (operation == "need.critical") {
        const auto npc = state.npcs.find(npcId);
        const auto* authored = findNpcContent(content, npcId);
        if (npc == state.npcs.end() || authored == nullptr) return false;
        const auto needId = args.value("need", "");
        const auto definitionNeed = std::find_if(authored->needs.begin(), authored->needs.end(),
                                                 [&needId](const auto& item) { return item.id == needId; });
        const auto currentNeed = npc->second.needs.find(needId);
        return definitionNeed != authored->needs.end() && currentNeed != npc->second.needs.end() &&
               currentNeed->second.deficit >= definitionNeed->criticalThreshold;
    }
    if (operation == "npc.at") {
        const auto npc = state.npcs.find(npcId);
        return npc != state.npcs.end() && npc->second.location == args.value("location", "");
    }
    if (operation == "time.between") {
        const int minuteOfDay = state.now.hour() * 60 + state.now.minuteOfHour();
        return minuteOfDay >= args.value("from_minute", 0) && minuteOfDay < args.value("to_minute", 0);
    }
    return false;
}

bool reservationCanStart(const WorldState& state, const std::string& npcId,
                         const ActionCandidate& candidate) {
    if (candidate.reservationMode != "input_resources" || candidate.reservations.empty()) return true;
    std::map<std::string, int> remaining;
    for (const auto& [resource, amount] : candidate.reservations) {
        const auto found = state.resources.find(resource);
        const int available = found == state.resources.end() ? 0 : found->second.amount - found->second.reserved;
        remaining[resource] = std::max(0, amount - available);
    }
    if (std::all_of(remaining.begin(), remaining.end(), [](const auto& item) { return item.second == 0; })) {
        return true;
    }
    if (candidate.preemptionPolicy != "lower_priority") return false;
    std::vector<std::pair<std::string, int>> victims;
    for (const auto& [otherId, other] : state.npcs) {
        if (otherId == npcId || !other.activity.process ||
            other.activity.process->status != ProcessStatus::Running ||
            !other.activity.process->interruptible ||
            other.activity.process->priority >= candidate.priority) continue;
        bool contributes = false;
        for (const auto& [resource, amount] : other.activity.process->reservedResources) {
            const auto need = remaining.find(resource);
            if (need == remaining.end() || need->second <= 0 || amount <= 0) continue;
            contributes = true;
        }
        if (contributes) victims.emplace_back(otherId, other.activity.process->priority);
    }
    std::sort(victims.begin(), victims.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) return left.second < right.second;
        return left.first < right.first;
    });
    for (const auto& [victimId, priority] : victims) {
        (void)priority;
        const auto& process = state.npcs.at(victimId).activity.process.value();
        for (const auto& [resource, amount] : process.reservedResources) {
            const auto need = remaining.find(resource);
            if (need != remaining.end()) need->second = std::max(0, need->second - amount);
        }
        if (std::all_of(remaining.begin(), remaining.end(), [](const auto& item) { return item.second == 0; })) {
            return true;
        }
    }
    return false;
}

void markAvailability(const WorldState& state, const WorldContentPack& content,
                      const std::string& npcId, ActionCandidate& candidate) {
    const auto recipe = std::find_if(content.behaviorRecipes.begin(), content.behaviorRecipes.end(),
                                     [&candidate](const auto& item) {
                                         return item.id == candidate.recipeId || item.id == candidate.actionId;
                                     });
    if (recipe == content.behaviorRecipes.end()) {
        candidate.availableNow = false;
        candidate.unavailableReason = "missing_behavior_recipe";
        return;
    }
    if (!reservationCanStart(state, npcId, candidate)) {
        candidate.availableNow = false;
        candidate.unavailableReason = "insufficient_resource";
        return;
    }
    if (!BehaviorRecipeRuntime::canStart(
            *recipe, [&state, &content, &npcId](const BehaviorNodeContent& node) {
                return evaluateCondition(state, content, npcId, node);
            })) {
        candidate.availableNow = false;
        candidate.unavailableReason = "precondition_failed";
    }
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
    context.perception = "At " + locationView(content, npc.location).value("name", npc.location) +
                         " (" + locationView(content, npc.location).value("type", "place") + "), it is " + state.now.toString() +
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
    for (std::size_t index = 0; index < context.stimuli.size(); ++index) {
        context.evidenceRefs["evidence_" + std::to_string(index + 1)] =
            "stimulus:" + context.stimuli.at(index).id;
    }

    const auto recency = [&state](const GameTime at) {
        const auto age = std::max<std::int64_t>(0, state.now.minute - at.minute);
        return 1.0 / (1.0 + static_cast<double>(age) / static_cast<double>(kMinutesPerDay));
    };
    std::vector<std::size_t> memoryOrder(npc.memories.size());
    std::iota(memoryOrder.begin(), memoryOrder.end(), 0);
    std::stable_sort(memoryOrder.begin(), memoryOrder.end(), [&npc, &recency](const auto left, const auto right) {
        const auto& a = npc.memories.at(left);
        const auto& b = npc.memories.at(right);
        const double aScore = 0.7 * std::clamp(a.confidence, 0.0, 1.0) + 0.3 * recency(a.at);
        const double bScore = 0.7 * std::clamp(b.confidence, 0.0, 1.0) + 0.3 * recency(b.at);
        if (aScore != bScore) return aScore > bScore;
        if (a.at.minute != b.at.minute) return a.at.minute > b.at.minute;
        return a.text < b.text;
    });
    std::vector<std::string> memories;
    const auto memoryLimit = std::min(content.decisionContextPolicy.maxMemories, memoryOrder.size());
    for (std::size_t rank = 0; rank < memoryLimit; ++rank) {
        const auto& memory = npc.memories.at(memoryOrder.at(rank));
        const std::string ref = "memory_" + std::to_string(rank + 1);
        context.evidenceRefs[ref] = "memory";
        context.cognitiveEvidence["memories"].push_back(
            Json{{"ref", ref}, {"kind", memory.kind}, {"text", memory.text},
                 {"at", memory.at.toString()}, {"confidence", std::clamp(memory.confidence, 0.0, 1.0)}});
        memories.push_back(ref + ": " + memory.text + " [kind=" + memory.kind + ", at=" +
                           memory.at.toString() + ", confidence=" + std::to_string(memory.confidence) + "]");
    }
    context.memories = join(memories, " | ");

    std::vector<std::size_t> knowledgeOrder(npc.knowledge.size());
    std::iota(knowledgeOrder.begin(), knowledgeOrder.end(), 0);
    std::stable_sort(knowledgeOrder.begin(), knowledgeOrder.end(), [&npc, &recency](const auto left, const auto right) {
        const auto& a = npc.knowledge.at(left);
        const auto& b = npc.knowledge.at(right);
        const double aScore = 0.55 * std::clamp(a.confidence, 0.0, 1.0) +
                              0.3 * (a.confirmed ? 1.0 : 0.0) + 0.15 * recency(a.learnedAt);
        const double bScore = 0.55 * std::clamp(b.confidence, 0.0, 1.0) +
                              0.3 * (b.confirmed ? 1.0 : 0.0) + 0.15 * recency(b.learnedAt);
        if (aScore != bScore) return aScore > bScore;
        if (a.learnedAt.minute != b.learnedAt.minute) return a.learnedAt.minute > b.learnedAt.minute;
        return a.id < b.id;
    });
    std::vector<std::string> knowledge;
    const auto knowledgeLimit = std::min(content.decisionContextPolicy.maxKnowledge, knowledgeOrder.size());
    for (std::size_t rank = 0; rank < knowledgeLimit; ++rank) {
        const auto& fact = npc.knowledge.at(knowledgeOrder.at(rank));
        const std::string ref = "knowledge_" + std::to_string(rank + 1);
        context.evidenceRefs[ref] = "knowledge";
        context.cognitiveEvidence["knowledge"].push_back(
            Json{{"ref", ref}, {"subject", fact.subject}, {"predicate", fact.predicate},
                 {"value", fact.value}, {"source_sequence", fact.sourceSequence},
                 {"source_type", fact.sourceType}, {"learned_at", fact.learnedAt.toString()},
                 {"scope", fact.scope}, {"confidence", std::clamp(fact.confidence, 0.0, 1.0)},
                 {"confirmed", fact.confirmed}});
        knowledge.push_back(ref + ": " + fact.subject + " " + fact.predicate + " " + fact.value +
                            " [confirmed=" + (fact.confirmed ? "true" : "false") +
                            ", confidence=" + std::to_string(fact.confidence) + ", source=" +
                            std::to_string(fact.sourceSequence) + ", scope=" + fact.scope + "]");
    }
    context.knowledge = join(knowledge, " | ");

    std::vector<std::size_t> experienceOrder(npc.experiences.size());
    std::iota(experienceOrder.begin(), experienceOrder.end(), 0);
    std::stable_sort(experienceOrder.begin(), experienceOrder.end(), [&npc, &recency](const auto left, const auto right) {
        const auto& a = npc.experiences.at(left);
        const auto& b = npc.experiences.at(right);
        const double aScore = 0.65 * std::clamp(a.confidence, 0.0, 1.0) + 0.35 * recency(a.lastUsed);
        const double bScore = 0.65 * std::clamp(b.confidence, 0.0, 1.0) + 0.35 * recency(b.lastUsed);
        if (aScore != bScore) return aScore > bScore;
        if (a.lastUsed.minute != b.lastUsed.minute) return a.lastUsed.minute > b.lastUsed.minute;
        return a.id < b.id;
    });
    std::vector<std::string> experiences;
    const auto experienceLimit = std::min(content.decisionContextPolicy.maxExperiences, experienceOrder.size());
    for (std::size_t rank = 0; rank < experienceLimit; ++rank) {
        const auto& experience = npc.experiences.at(experienceOrder.at(rank));
        const std::string ref = "experience_" + std::to_string(rank + 1);
        context.evidenceRefs[ref] = "experience";
        context.cognitiveEvidence["experiences"].push_back(
            Json{{"ref", ref}, {"domain", experience.domain}, {"trigger", experience.trigger},
                 {"action", experience.action}, {"outcome", experience.outcome},
                 {"source_sequences", experience.sourceSequences}, {"learned_at", experience.learnedAt.toString()},
                 {"last_used", experience.lastUsed.toString()},
                 {"confidence", std::clamp(experience.confidence, 0.0, 1.0)},
                 {"personal", experience.personal}});
        experiences.push_back(ref + ": " + experience.domain + ": when " + experience.trigger + ", " +
                              experience.action + " -> " + experience.outcome +
                              " [confidence=" + std::to_string(experience.confidence) + "]");
    }
    context.experiences = join(experiences, " | ");
    context.character = authoredDefinition.name + ": " + authoredDefinition.role + "; public goal=" +
                        authoredDefinition.publicGoal;
    context.privateCharacter = "hidden goal=" + authoredDefinition.hiddenGoal +
                               "; boundary=" + authoredDefinition.boundary +
                               "; strategy=" + npc.strategy;
    for (const auto& [trait, value] : authoredDefinition.privateTraits) {
        context.privateTraits[trait] = value;
    }
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
    context.currentActivity = Json{{"status", npc.activity.goal || npc.activity.process
                                               ? toString(npc.activity.status)
                                               : "idle"},
                                   {"active_goal", "none"}, {"active_process", "none"}};
    if (npc.activity.goal) {
        context.currentActivity["active_goal"] = Json{
            {"intent", npc.activity.goal->intent},
            {"plan_key", npc.activity.goal->target},
            {"reason", npc.activity.goal->reason},
            {"status", toString(npc.activity.goal->status)},
            {"priority", npc.activity.goal->priority},
            {"deadline", npc.activity.goal->deadline.toString()}};
        context.currentActivity["goal_id"] = npc.activity.goal->id;
        context.currentActivity["intent"] = npc.activity.goal->intent;
        context.currentActivity["action_id"] = npc.activity.goal->target;
        context.currentActivity["reason"] = npc.activity.goal->reason;
        context.currentActivity["deadline"] = npc.activity.goal->deadline.toString();
    }
    if (npc.activity.process) {
        context.currentActivity["active_process"] = Json{
            {"plan_key", npc.activity.process->planId},
            {"recipe_key", npc.activity.process->recipeId},
            {"progress", npc.activity.process->progress},
            {"behavior_step", npc.activity.process->behaviorStep},
            {"behavior_status", npc.activity.process->behaviorStatus},
            {"completes_at", npc.activity.process->completesAt.toString()}};
        context.currentActivity["process_id"] = npc.activity.process->id;
        context.currentActivity["recipe_id"] = npc.activity.process->recipeId;
        context.currentActivity["plan_id"] = npc.activity.process->planId;
        context.currentActivity["progress"] = npc.activity.process->progress;
        context.currentActivity["behavior_step"] = npc.activity.process->behaviorStep;
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
        candidate.label = recipe.description.empty()
                              ? (recipe.planId.empty() ? recipe.id : recipe.planId)
                              : recipe.description;
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
    for (auto& candidate : context.candidates) {
        markAvailability(state, content, npcId, candidate);
    }
    // A resource constraint is useful only when an authored candidate
    // actually reserves that resource and the captured ledger cannot satisfy
    // the reservation. Do not turn unrelated empty public pools into hard
    // constraints for every plan in the context.
    std::set<std::string> constrainedResources;
    for (const auto& candidate : context.candidates) {
        if (candidate.reservationMode != "input_resources") continue;
        for (const auto& [resourceId, required] : candidate.reservations) {
            if (required <= 0 || constrainedResources.contains(resourceId)) continue;
            const auto found = state.resources.find(resourceId);
            const int available = found == state.resources.end() ? 0 : found->second.amount - found->second.reserved;
            if (available >= required) continue;
            const int missing = required - std::max(0, available);
            constrainedResources.insert(resourceId);
            context.constraints.push_back(StructuralConstraintView{
                "resource:" + resourceId + ":unavailable", "resource", resourceId,
                "world_resource_rule",
                "A plan requires " + std::to_string(required) + " " + resourceId +
                    "; " + std::to_string(std::max(0, available)) + " unreserved, " +
                    std::to_string(missing) + " missing.",
                true});
        }
    }
    std::ostringstream constraints;
    for (const auto& constraint : context.constraints) {
        constraints << constraint.id << " [" << (constraint.hard ? "hard" : "soft") << "] "
                    << constraint.reason << ';';
    }
    context.structuralConstraints = constraints.str();
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

    const auto ranked = GoalSelector::rank(npc, needs, context.candidates, authoredDefinition.privateTraits,
                                           content.decisionContextPolicy.traitResponseExponent);
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
                                 {"observer_location_view", locationView(content, npc.location)},
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
                 {"location", authoredResource.location},
                 {"location_view", locationView(content, authoredResource.location)},
                 {"unit", authoredResource.unit},
                 {"kind", authoredResource.kind}};
    }
    std::size_t visibleEntityCount = 0;
    for (const auto& [peerId, peer] : state.npcs) {
        if (peerId == npcId || peer.location != npc.location) continue;
        if (visibleEntityCount++ >= content.decisionContextPolicy.maxVisibleEntities) break;
        const auto* peerAuthored = authoredNpc(content, peerId);
        context.worldSnapshot["visible_entities"].push_back(
            Json{{"id", peerId}, {"name", peerAuthored == nullptr ? peerId : peerAuthored->definition.name},
                 {"kind", "person"}, {"location", peer.location},
                 {"location_view", locationView(content, peer.location)}, {"mode", peer.mode}});
    }
    std::size_t visibleLinkCount = 0;
    for (const auto& link : content.links) {
        if (std::find(link.link.endpoints.begin(), link.link.endpoints.end(), npc.location) == link.link.endpoints.end()) {
            continue;
        }
        if (visibleLinkCount++ >= content.decisionContextPolicy.maxVisibleLinks) break;
        const auto runtime = state.links.find(link.id);
        const RuntimeLinkState linkState = runtime == state.links.end() ? RuntimeLinkState{} : runtime->second;
        Json endpointViews = Json::array();
        for (const auto& endpoint : link.link.endpoints) endpointViews.push_back(locationView(content, endpoint));
        context.worldSnapshot["visible_links"].push_back(
            Json{{"id", link.id}, {"endpoints", link.link.endpoints},
                 {"endpoint_views", endpointViews}, {"type", link.kind}, {"tags", link.link.tags},
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
                             {"location_view", locationView(content, npc.location)},
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
        const auto* peerAuthored = authoredNpc(content, peerId);
        context.selfState["relationships"][peerId] =
            Json{{"name", peerAuthored == nullptr ? peerId : peerAuthored->definition.name},
                 {"trust", relation.trust}, {"respect", relation.respect},
                 {"fear", relation.fear}, {"owed", relation.owed}};
    }
    for (const auto& candidate : context.candidates) {
        Json plan{{"action_id", candidate.actionId}, {"plan_id", candidate.planId},
                  {"recipe_id", candidate.recipeId}, {"label", candidate.label},
                  {"description", candidate.label},
                  {"semantic_description", candidate.label},
                  {"duration_minutes", candidate.durationMinutes},
                  {"interruptible", candidate.interruptible}, {"priority", candidate.priority},
                  {"preemption_policy", candidate.preemptionPolicy},
                  {"utility_score", candidate.utilityScore},
                  {"utility_contributions", candidate.utilityContributions},
                  {"reservations", candidate.reservations},
                  {"parameter_schema", candidate.parameterSchema},
                  {"available_now", candidate.availableNow},
                  {"unavailable_reason", candidate.unavailableReason}};
        const auto recipe = std::find_if(content.behaviorRecipes.begin(), content.behaviorRecipes.end(),
                                         [&candidate](const auto& item) { return item.id == candidate.recipeId; });
        if (recipe != content.behaviorRecipes.end()) {
            plan["behavior_tree"] = behaviorNodeProjection(recipe->root);
            plan["execution_outline"] = behaviorNodeExplanation(recipe->root, content);
        }
        context.planCatalog.push_back(std::move(plan));
    }
    return context;
}

}  // namespace tide_marks
