#include "world/npc_simulation_system.hpp"

#include "world/world_service.hpp"
#include "world/ids.hpp"
#include "npc/state_machine.hpp"

#include <algorithm>
#include <numeric>

namespace tide_marks {

namespace {
void applyNpcSignal(NpcState& npc, NpcSignal signal) {
    npc.mode = toString(transition(npcModeFromString(npc.mode), signal));
}
}  // namespace

void NpcSimulationSystem::processActivity(const ActivityContent& activity) {
    if (!activity.decisionChannel.empty()) {
        if (activity.actor == kWorldActor || !world_.hasNpc(activity.actor)) {
            world_.addEvent("activity.failed", activity.actor, activity.target,
                     "An activity requested a decision channel for a non-NPC actor",
                     Json{{"activity_id", activity.id}, {"decision_channel", activity.decisionChannel}});
            return;
        }
        const auto& npc = world_.state_.npcs.at(activity.actor);
        if (npc.activity.status == ActivityStatus::Active) return;
        // This is a trigger, not an action. Effects remain in the content as
        // authored affordances for review, but they are only applied by the
        // selected recipe after the LLM proposal passes Runtime validation.
        world_.addEvent("npc.activity_opportunity", activity.actor, activity.target,
                 activity.detail, Json{{"activity_id", activity.id},
                                       {"decision_channel", activity.decisionChannel},
                                       {"effects_deferred_to_provider", true}});
        world_.pendingDecisionTriggers_.emplace_back(activity.actor, "scheduled_activity:" + activity.id,
                                                      activity.decisionChannel);
        return;
    }
    std::map<std::string, int> required;
    for (const auto& effect : activity.effects) {
        if (effect.operation == "resource.consume" || effect.operation == "resource.reserve") {
            required[effect.resource] += effect.amount;
        }
    }
    for (const auto& [resource, amount] : required) {
        if (world_.availableResourceValue(resource) < amount) return;
    }

    WorldService::TransactionScope activityTransaction(world_);
    Json changes = Json::object();
    for (const auto& effect : activity.effects) {
        if (effect.operation == "meta_action") {
            BehaviorNodeContent node;
            node.type = "meta_action";
            node.id = activity.id + ":" + effect.metaActionId;
            node.name = effect.metaActionId;
            node.args = effect.args;
            if (!world_.applyMetaAction(activity.actor, effect.metaActionId, node, nullptr)) {
                activityTransaction.rollback();
                world_.addEvent("activity.failed", activity.actor, activity.target,
                         "A scheduled activity was rejected by a registered meta action",
                         Json{{"activity_id", activity.id}, {"meta_action", effect.metaActionId}});
                return;
            }
            continue;
        }
        if (effect.operation == "need.relieve") {
            if (activity.actor != kWorldActor) world_.applyNeedRelief(activity.actor, effect.effectAction);
            continue;
        }
        if (effect.operation == "link.set_state") {
            world_.state_.links[effect.linkId] = RuntimeLinkState{effect.traversable, effect.locked};
            changes[effect.linkId] = Json{{"traversable", effect.traversable}, {"locked", effect.locked}};
            continue;
        }
        const int before = world_.resourceValue(effect.resource);
        if (effect.operation == "resource.consume") world_.setResource(effect.resource, before - effect.amount);
        if (effect.operation == "resource.produce") world_.setResource(effect.resource, before + effect.amount);
        if (effect.operation == "resource.reserve") world_.state_.resources[effect.resource].reserved += effect.amount;
        if (effect.operation == "resource.release") {
            auto& resource = world_.state_.resources[effect.resource];
            resource.reserved = std::max(0, resource.reserved - effect.amount);
        }
        if (effect.operation != "resource.release") {
            changes[effect.resource] = Json{{"before", before}, {"after", world_.resourceValue(effect.resource)}};
        }
    }
    world_.addEvent(activity.eventType, activity.actor, activity.target, activity.detail,
             Json{{"activity_id", activity.id}, {"changes", changes}});
}

void NpcSimulationSystem::tickProcesses(const std::set<std::string>& dueNpcIds) {
    // Completion may run a nested recipe savepoint. A failed recipe restores
    // WorldState and invalidates references/iterators into the NPC map, so
    // collect durable work items before invoking code that may roll back.
    std::vector<std::pair<std::string, ProcessExecution>> due;
    for (const auto& [npcId, npc] : world_.state_.npcs) {
        if (!dueNpcIds.contains(npcId)) continue;
        if (npc.activity.status != ActivityStatus::Active ||
            !npc.activity.process || npc.activity.process->status != ProcessStatus::Running) {
            continue;
        }
        auto process = *npc.activity.process;
        const auto duration = std::max<std::int64_t>(1, process.completesAt.minute - process.startedAt.minute);
        process.progress = std::clamp(static_cast<double>(world_.state_.now.minute - process.startedAt.minute) /
                                          static_cast<double>(duration),
                                      0.0, 1.0);
        double interruptSalience = 0.0;
        for (const auto& stimulus : npc.stimuli) {
            if (!stimulus.active || stimulus.observedAt.minute <= process.startedAt.minute) continue;
            interruptSalience = std::max(interruptSalience, stimulus.effectiveSalience);
        }
        if (process.interruptible &&
            interruptSalience >= std::max(process.interruptSalienceThreshold,
                                          world_.content_.npcDeliberation.minInterruptSalience)) {
            world_.state_.npcs.at(npcId).activity.process->progress = process.progress;
            suspendProcess(npcId, world_.state_.npcs.at(npcId).activity.process.value(),
                           "new_high_salience_stimulus");
            continue;
        }
        if (world_.state_.now.minute < process.completesAt.minute) continue;
        due.emplace_back(npcId, process);
    }
    for (const auto& [npcId, process] : due) {
        completeProcess(npcId, process);
    }
}

void NpcSimulationSystem::resumeSuspendedProcesses(const std::set<std::string>& dueNpcIds) {
    for (auto& [npcId, npc] : world_.state_.npcs) {
        if (!dueNpcIds.contains(npcId)) continue;
        if (npc.activity.status != ActivityStatus::Suspended || !npc.activity.process) continue;
        auto& process = *npc.activity.process;
        double highestSalience = 0.0;
        for (const auto& stimulus : npc.stimuli) {
            if (stimulus.active && world_.state_.now.minute < stimulus.expiresAt.minute) {
                highestSalience = std::max(highestSalience, stimulus.effectiveSalience);
            }
        }
        if (highestSalience >= std::max(process.interruptSalienceThreshold,
                                        world_.content_.npcDeliberation.minInterruptSalience)) continue;
        bool resourcesAvailable = true;
        for (const auto& [resource, amount] : process.requiredResources) {
            if (world_.availableResourceValue(resource) < amount) {
                resourcesAvailable = false;
                break;
            }
        }
        if (!resourcesAvailable) continue;
        for (const auto& [resource, amount] : process.requiredResources) {
            world_.state_.resources[resource].reserved += amount;
            process.reservedResources[resource] = amount;
        }
        const std::string previousReason = process.interruptionReason;
        if (process.suspendedAt.minute >= 0) {
            process.completesAt.minute += world_.state_.now.minute - process.suspendedAt.minute;
        }
        process.suspendedAt = GameTime{-1};
        process.interruptionReason.clear();
        process.behaviorStatus = "running";
        setActivityStatus(npc.activity, ActivityStatus::Active);
        applyNpcSignal(npc, NpcSignal::StartProcess);
        world_.addEvent("npc.process_resumed", npcId, process.actionId,
                 "A suspended process resumed after its interruption cleared",
                 Json{{"process_id", process.id}, {"action_id", process.actionId},
                      {"previous_reason", previousReason},
                      {"completes_at", process.completesAt.minute}});
    }
}

void NpcSimulationSystem::releaseReservations(ProcessExecution& process) {
    for (const auto& [resource, amount] : process.reservedResources) {
        auto found = world_.state_.resources.find(resource);
        if (found != world_.state_.resources.end()) found->second.reserved = std::max(0, found->second.reserved - amount);
    }
    process.reservedResources.clear();
}

void NpcSimulationSystem::suspendProcess(const std::string& npcId, ProcessExecution& process,
                                  const std::string& reason, Json eventData) {
    releaseReservations(process);
    process.status = ProcessStatus::Suspended;
    process.behaviorStatus = "suspended";
    process.suspendedAt = world_.state_.now;
    process.interruptionReason = reason;
    if (auto found = world_.state_.npcs.find(npcId); found != world_.state_.npcs.end() &&
        found->second.activity.goal && found->second.activity.goal->id == process.goalId) {
        setActivityStatus(found->second.activity, ActivityStatus::Suspended);
        applyNpcSignal(found->second, NpcSignal::CancelProcess);
    }
    eventData["process_id"] = process.id;
    eventData["action_id"] = process.actionId;
    eventData["reason"] = reason;
    eventData["progress"] = process.progress;
    world_.addEvent("npc.process_suspended", npcId, process.actionId,
             reason == "resource_preempted"
                 ? "A lower-priority process released reservations for a higher-priority process"
                 : "A process was suspended by a high-salience stimulus",
             std::move(eventData));
}

std::optional<std::vector<std::string>> NpcSimulationSystem::planResourcePreemption(
    const std::string& requesterId, const std::map<std::string, int>& requirements,
    int requesterPriority) const {
    std::map<std::string, int> remaining;
    for (const auto& [resource, amount] : requirements) {
        remaining[resource] = std::max(0, amount - world_.availableResourceValue(resource));
    }
    const bool alreadyAvailable = std::all_of(remaining.begin(), remaining.end(),
                                              [](const auto& entry) { return entry.second == 0; });
    if (alreadyAvailable) return std::vector<std::string>{};

    struct Candidate {
        const std::string* npcId;
        int priority;
    };
    std::vector<Candidate> candidates;
    for (const auto& [npcId, npc] : world_.state_.npcs) {
        if (npcId == requesterId || !npc.activity.process ||
            npc.activity.process->status != ProcessStatus::Running ||
            !npc.activity.process->interruptible ||
            npc.activity.process->priority >= requesterPriority) {
            continue;
        }
        const bool ownsNeededReservation = std::any_of(
            remaining.begin(), remaining.end(), [&npc](const auto& entry) {
                const auto reserved = npc.activity.process->reservedResources.find(entry.first);
                return entry.second > 0 && reserved != npc.activity.process->reservedResources.end() &&
                       reserved->second > 0;
            });
        if (ownsNeededReservation) candidates.push_back(Candidate{&npcId, npc.activity.process->priority});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.priority != right.priority) return left.priority < right.priority;
        return *left.npcId < *right.npcId;
    });

    std::vector<std::string> victims;
    for (const auto& candidate : candidates) {
        const auto& process = world_.state_.npcs.at(*candidate.npcId).activity.process.value();
        bool contributes = false;
        for (const auto& [resource, amount] : process.reservedResources) {
            auto need = remaining.find(resource);
            if (need == remaining.end() || need->second <= 0 || amount <= 0) continue;
            contributes = true;
            need->second = std::max(0, need->second - amount);
        }
        if (contributes) victims.push_back(*candidate.npcId);
        if (std::all_of(remaining.begin(), remaining.end(),
                        [](const auto& entry) { return entry.second == 0; })) {
            return victims;
        }
    }
    return std::nullopt;
}

void NpcSimulationSystem::cancelProcess(const std::string& npcId, ProcessExecution& process,
                                 const std::string& reason) {
    releaseReservations(process);
    process.behaviorStatus = "cancelled";
    process.status = ProcessStatus::Cancelled;
    process.interruptionReason = reason;
    if (auto found = world_.state_.npcs.find(npcId); found != world_.state_.npcs.end() && found->second.activity.goal &&
        found->second.activity.goal->id == process.goalId) {
        setActivityStatus(found->second.activity, ActivityStatus::Cancelled);
        applyNpcSignal(found->second, NpcSignal::CancelProcess);
    }
    world_.addEvent("npc.process_interrupted", npcId, process.actionId,
             "A committed process was interrupted before completion",
             Json{{"process_id", process.id}, {"action_id", process.actionId},
                  {"reason", reason}, {"progress", process.progress}, {"status", toString(process.status)}});
}

void NpcSimulationSystem::finalizeInnovationAdoption(const std::string& npcId,
                                                     const ProcessExecution& process,
                                                     bool recipeApplied) {
    if (process.trigger.rfind("innovation_adoption:", 0) != 0) return;
    const auto proposalId = process.trigger.substr(std::string("innovation_adoption:").size());
    auto npc = world_.state_.npcs.find(npcId);
    if (npc == world_.state_.npcs.end()) return;
    auto adoption = std::find_if(npc->second.innovationAdoptions.begin(), npc->second.innovationAdoptions.end(),
                                 [&process, &proposalId](const auto& item) {
                                     return item.processId == process.id || item.proposalId == proposalId;
                                 });
    if (adoption == npc->second.innovationAdoptions.end()) return;
    Json observed = Json::object();
    const auto eventCount = std::count_if(world_.state_.events.begin(), world_.state_.events.end(),
                                          [&adoption](const auto& event) {
                                              return event.sequence > adoption->startEventSequence;
                                          });
    observed["events_added"] = eventCount;
    observed["final_revision"] = world_.state_.revision;
    observed["npc_count"] = world_.state_.npcs.size();
    observed["knowledge_count"] = std::accumulate(
        world_.state_.npcs.begin(), world_.state_.npcs.end(), std::size_t{0},
        [](std::size_t total, const auto& entry) { return total + entry.second.knowledge.size(); });
    observed["experience_count"] = std::accumulate(
        world_.state_.npcs.begin(), world_.state_.npcs.end(), std::size_t{0},
        [](std::size_t total, const auto& entry) { return total + entry.second.experiences.size(); });
    observed["advance_minutes"] = std::max<std::int64_t>(0, world_.state_.now.minute - process.startedAt.minute);
    observed["resource_total"] = std::accumulate(
        world_.state_.resources.begin(), world_.state_.resources.end(), 0,
        [](int total, const auto& entry) { return total + entry.second.amount; });

    Json evaluation = Json{{"passed", recipeApplied}, {"metrics", Json::object()}};
    bool metricsPassed = recipeApplied;
    const auto hypothesis = std::find_if(npc->second.innovationHypotheses.begin(), npc->second.innovationHypotheses.end(),
                                         [&proposalId](const Json& item) { return item.value("id", "") == proposalId; });
    if (hypothesis != npc->second.innovationHypotheses.end()) {
        const auto expected = hypothesis->value("innovation", Json::object())
                                  .value("test_plan", Json::object())
                                  .value("expected_metrics", Json::object());
        if (!expected.is_object() || expected.empty()) metricsPassed = false;
        for (auto item = expected.begin(); item != expected.end(); ++item) {
            const auto actual = observed.value(item.key(), Json());
            const bool comparable = actual.is_number() && item.value().is_number();
            const double actualValue = comparable ? actual.get<double>() : 0.0;
            const double target = item.value().is_number() ? item.value().get<double>() : 0.0;
            // Counts and progress targets are lower bounds. Resource budgets
            // are upper bounds so a successful innovation cannot spend more
            // than the declared budget.
            const bool passed = comparable && (item.key() == "resource_total" ? actualValue <= target
                                                                                : actualValue >= target);
            evaluation["metrics"][item.key()] = Json{{"actual", actualValue}, {"expected", target},
                                                       {"passed", passed}};
            metricsPassed = metricsPassed && passed;
        }
    }
    evaluation["passed"] = metricsPassed;
    adoption->observedMetrics = observed;
    adoption->metricEvaluation = evaluation;
    adoption->updatedAt = world_.state_.now;
    adoption->status = metricsPassed ? InnovationAdoptionStatus::Completed : InnovationAdoptionStatus::Failed;
    adoption->reason = metricsPassed ? "recipe completed and effect metrics passed"
                                     : (recipeApplied ? "effect metrics failed" : "recipe completion failed");
    ++npc->second.stateVersion;
    world_.addEvent(metricsPassed ? "innovation.adoption_completed" : "innovation.adoption_failed", npcId,
                    proposalId, metricsPassed ? "Innovation adoption completed with measured effects"
                                               : "Innovation adoption failed its measured effect criteria",
                    Json{{"proposal_id", proposalId}, {"process_id", process.id},
                         {"status", toString(adoption->status)}, {"reason", adoption->reason},
                         {"observed_metrics", observed}, {"metric_evaluation", evaluation}});
}

void NpcSimulationSystem::completeProcess(const std::string& npcId, ProcessExecution process) {
    if (!world_.state_.npcs.contains(npcId)) return;
    const bool applied = world_.applyActionEffects(
        npcId, process.recipeId.empty() ? process.actionId : process.recipeId,
        &process.reservedResources, &process.parameters);
    process.progress = 1.0;
    process.status = applied ? ProcessStatus::Completed : ProcessStatus::Failed;
    process.behaviorStatus = applied ? "success" : "failure";
    // A successful recipe may consume only part of its declared reservation
    // (or none at all). Release the remainder on every terminal path so an
    // adoption cannot strand capacity after its ProcessExecution disappears.
    releaseReservations(process);
    world_.addEvent(applied ? "npc.process_completed" : "npc.process_failed", npcId, process.actionId,
             applied ? "A committed process completed and its effects were applied"
                     : "A committed process could not satisfy its completion preconditions",
             Json{{"process_id", process.id}, {"action_id", process.actionId},
                  {"started_at", process.startedAt.minute}, {"completed_at", world_.state_.now.minute},
                  {"progress", process.progress}, {"status", toString(process.status)}});
    finalizeInnovationAdoption(npcId, process, applied);
    // A failed recipe restores a nested savepoint and invalidates iterators
    // into WorldState. Reacquire the NPC after the recipe has returned.
    auto found = world_.state_.npcs.find(npcId);
    if (found == world_.state_.npcs.end()) return;
    if (found->second.activity.process && found->second.activity.process->id == process.id) {
        found->second.activity.process = process;
        setActivityStatus(found->second.activity,
                          applied ? ActivityStatus::Completed : ActivityStatus::Failed);
    }
    applyNpcSignal(found->second, NpcSignal::FinishProcess);
    const auto* authored = world_.authoredNpc(npcId);
    applyNpcSignal(found->second, authored != nullptr && found->second.location == authored->definition.home
                                      ? NpcSignal::AtHome
                                      : NpcSignal::AtWork);
    found->second.activity = NpcActivity{};
    ++found->second.stateVersion;
}

void NpcSimulationSystem::processAutonomousTurns(const std::set<std::string>& dueNpcIds) {
    if (!world_.content_.npcDeliberation.enabled) return;
    world_.pumpAsyncTurns();
    for (auto& [npcId, npc] : world_.state_.npcs) {
        if (!dueNpcIds.contains(npcId)) continue;
        if (!world_.hasNpc(npcId) || npc.activity.status == ActivityStatus::Active) continue;
        if (npc.lastAutonomousTurn.minute >= 0 &&
            world_.state_.now.minute - npc.lastAutonomousTurn.minute < world_.content_.npcDeliberation.cooldownMinutes) {
            continue;
        }
        const auto context = world_.decisionContext(npcId);
        double highestStimulus = 0.0;
        for (const auto& stimulus : context.stimuli) {
            highestStimulus = std::max(highestStimulus, stimulus.effectiveSalience);
        }
        double highestDrive = 0.0;
        for (const auto& drive : context.drives) highestDrive = std::max(highestDrive, drive.drive);
        if (highestStimulus < world_.content_.npcDeliberation.minStimulusSalience &&
            highestDrive < world_.content_.npcDeliberation.minDrivePressure) {
            continue;
        }
        // The request captures the current decision context and returns to the
        // main thread later. Marking the scheduling time prevents an hourly
        // boundary from enqueueing the same NPC repeatedly while the model is
        // still thinking.
        world_.enqueueAutonomousTurn(npcId, "high_salience_context");
    }
}

void NpcSimulationSystem::processDayBoundary(int day, const std::set<std::string>& dueNpcIds) {
    for (const auto& activity : world_.content_.activities) {
        if (activity.trigger != "day") continue;
        if (!activity.conditionResource.empty() && world_.resourceValue(activity.conditionResource) >= activity.conditionBelow) continue;
        processActivity(activity);
    }
    for (auto& [id, npc] : world_.state_.npcs) {
        if (!dueNpcIds.contains(id)) continue;
        const auto entity = world_.state_.entities.find(npcEntityId(id));
        if (entity != world_.state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        ++npc.stateVersion;
        npc.memories.push_back(Memory{"episode", "A day passed while the world kept working.", world_.state_.now, 1.0});
    }
    reviseNpcStrategies(day);
    Json resources = Json::object();
    for (const auto& [id, resource] : world_.state_.resources) {
        resources[id] = Json{{"amount", resource.amount}, {"reserved", resource.reserved}};
    }
    world_.addEvent("life.day", kWorldActor, "instance", "The world completes another day",
             Json{{"day", day}, {"resources", resources}});
}

void NpcSimulationSystem::reviseNpcStrategies(int day) {
    for (const auto& rule : world_.content_.strategyRules) {
        auto found = world_.state_.npcs.find(rule.npcId);
        if (found == world_.state_.npcs.end() || found->second.strategy == rule.strategy) continue;
        const auto trust = found->second.relationships.find(rule.peerId);
        if (trust == found->second.relationships.end() || trust->second.trust < rule.minPeerTrust) continue;
        auto& npc = found->second;
        npc.strategy = rule.strategy;
        npc.goalStage = std::max(npc.goalStage, rule.goalStage);
        npc.memories.push_back(Memory{"plan", rule.memory, world_.state_.now, trust->second.trust});
        ++npc.stateVersion;
        world_.addEvent(rule.eventType, rule.npcId, rule.peerId, rule.memory,
                 Json{{"strategy", npc.strategy}, {"rule", rule.id}, {"peer_trust", trust->second.trust}, {"day", day}});
    }
}

}  // namespace tide_marks
