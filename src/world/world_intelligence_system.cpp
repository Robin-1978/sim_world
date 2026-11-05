#include "world/world_intelligence_system.hpp"

#include "world/world_service.hpp"
#include "npc/action_validator.hpp"
#include "npc/state_machine.hpp"
#include "world/ids.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace tide_marks {

namespace {
void applyNpcSignal(NpcState& npc, NpcSignal signal) {
    npc.mode = toString(transition(npcModeFromString(npc.mode), signal));
}
}  // namespace

DialogueTurn WorldIntelligenceSystem::converse(const std::string& npcId,
                                               const std::string& playerInput,
                                               const std::string& turnId) {
    return executeTurn(npcId, playerInput, turnId, world_.content_.conversationChannelId,
                       "player_input");
}

DialogueTurn WorldIntelligenceSystem::deliberate(const std::string& npcId,
                                                 const std::string& trigger,
                                                 const std::string& turnId) {
    return executeTurn(npcId, {}, turnId, world_.content_.autonomousChannelId, trigger);
}

TurnRequest WorldIntelligenceSystem::makeTurnRequest(const std::string& npcId, const std::string& input,
                                          const std::string& turnId, const std::string& channelId,
                                          const std::string& trigger) const {
    TurnRequest request;
    request.turnId = turnId;
    request.npcId = npcId;
    request.channelId = channelId;
    request.trigger = trigger;
    request.playerInput = input;
    request.contentVersion = world_.content_.contentVersion;
    request.npcStateVersion = world_.state_.npcs.at(npcId).stateVersion;
    request.createdAt = world_.state_.now;
    const auto channel = world_.content_.decisionChannels.find(channelId);
    request.context = world_.decisionContext(npcId);
    request.context.channel = channelId;
    request.context.providerPolicy = channel->second.providerPolicy;
    request.context.trigger = trigger;
    request.context.promptVersion =
        "tide-marks-intent-v2:" + world_.content_.contentVersion + ":" + world_.content_.id;
    request.context.runtimeMetadata["channel_id"] = channelId;
    request.context.runtimeMetadata["trigger"] = trigger;
    request.context.runtimeMetadata["prompt_version"] = request.context.promptVersion;
    request.promptVersion = request.context.promptVersion;
    request.coalescePending = !channel->second.conversation &&
                              world_.content_.npcDeliberation.coalescePending;
    request.deadline = GameTime{world_.state_.now.minute + world_.content_.npcDeliberation.maxDecisionAgeMinutes};
    if (channel->second.conversation && channel->second.classifySpeechAct) {
        // Semantic interpretation belongs exclusively to the provider. Runtime
        // supplies the content-authored speech-act vocabulary and validates the
        // returned identifier; it never inspects player text for keywords.
        request.context.speechAct.clear();
        request.context.worldActionAllowed = false;
        request.context.speechActClassificationRequired = true;
        for (const auto& rule : world_.content_.conversationPolicy.rules) {
            request.context.speechActs.push_back(
                SpeechActPermissionView{toString(rule.act), rule.description, rule.worldActionAllowed});
        }
    } else {
        request.context.speechAct = channel->second.defaultSpeechAct;
        request.context.worldActionAllowed = channel->second.defaultWorldActionAllowed;
    }
    request.worldRevision = request.context.worldRevision;
    return request;
}

DialogueTurn WorldIntelligenceSystem::executeTurn(const std::string& npcId, const std::string& input,
                                       const std::string& turnId, const std::string& channelId,
                                       const std::string& trigger) {
    constexpr std::size_t kMaxPlayerInputBytes = 8192;
    DialogueTurn result;
    result.npcId = npcId;
    result.provider = world_.provider_->name();
    if (!world_.hasNpc(npcId)) {
        result.error = "unknown_npc";
        return result;
    }
    if (input.size() > kMaxPlayerInputBytes) {
        result.error = "input_too_long";
        return result;
    }
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) {
        result.error = "duplicate_turn";
        return result;
    }
    if (!world_.content_.decisionChannels.contains(channelId)) {
        result.error = "unknown_decision_channel";
        return result;
    }
    auto request = makeTurnRequest(npcId, input, turnId, channelId, trigger);
    auto response = world_.provider_->decide(request.context, input);
    auto turnResult = commitTurn(request, response);
    // A provider response can be structurally valid while its parameters do
    // not satisfy the authored plan contract. Give the real LLM one bounded,
    // structured repair opportunity. Resource, permission, revision and
    // topology failures remain closed: retrying those would merely repeat an
    // impossible world action and increase player latency.
    const auto retryable = [](const std::string& code) {
        return code == "missing_action_parameter" || code == "unknown_action_parameter" ||
               code == "invalid_action_parameter_type" || code == "unexpected_action_parameters" ||
               code == "invalid_action_parameters" || code == "invalid_action_parameter_value" ||
               code == "invalid_expected_relief" || code == "missing_dialogue" ||
               code == "speech_act_disallows_world_action";
    };
    if (!turnResult.accepted && response.ok && world_.provider_->name() == "llama.cpp" && retryable(turnResult.error)) {
        const Json feedback{{"attempt", 1},
                            {"code", turnResult.error},
                            {"message", turnResult.error == "missing_dialogue"
                                             ? "A conversation response must contain a concise player-facing dialogue sentence."
                                             : turnResult.error == "speech_act_disallows_world_action"
                                                 ? "The utterance was classified as information-only; set action_id to an empty string, world_action_requested to false, expected_relief to {}, and answer from observable context."
                                                 : "Runtime rejected a structured proposal field; repair only the declared plan input or expected relief keys."},
                            {"previous_action_id", response.proposal.actionId},
                            {"previous_parameters", response.proposal.parameters},
                            {"previous_dialogue", response.dialogue}};
        auto repaired = world_.provider_->decide(request.context, input, feedback);
        if (repaired.ok) turnResult = commitTurn(request, std::move(repaired));
    }
    return turnResult;
}

DialogueTurn WorldIntelligenceSystem::commitTurn(const TurnRequest& request, ProviderResponse response) {
    WorldService::TransactionScope transaction(world_);
    DialogueTurn result;
    result.npcId = request.npcId;
    result.provider = world_.provider_->name();
    if (!world_.hasNpc(request.npcId)) {
        result.error = "unknown_npc";
        return result;
    }
    if (!request.turnId.empty() && world_.state_.processedTurnIds.contains(request.turnId)) {
        result.error = "duplicate_turn";
        return result;
    }
    if (request.deadline.minute > 0 && world_.state_.now.minute > request.deadline.minute) {
        result.error = "decision_expired";
        return result;
    }
    const auto channel = world_.content_.decisionChannels.find(request.channelId);
    if (channel == world_.content_.decisionChannels.end()) {
        result.error = "unknown_decision_channel";
        return result;
    }
    const auto& npc = world_.state_.npcs.at(request.npcId);
    const bool worldRevisionChanged = world_.state_.revision != request.worldRevision;
    if (request.contentVersion != world_.content_.contentVersion ||
        request.promptVersion != request.context.promptVersion ||
        (worldRevisionChanged && !channel->second.allowInterveningWorldChanges) ||
        npc.stateVersion != request.npcStateVersion) {
        result.error = "stale_intent_proposal";
        return result;
    }
    const auto& context = request.context;
    if (!response.ok) {
        // The provider is the decision authority.  Runtime never silently
        // substitutes a deterministic choice when an LLM fails, because that
        // would make an apparently autonomous world advance without the
        // intelligence profile that the run was configured to use.  Tests and
        // offline tools can still inject MockProvider explicitly.
        result.error = response.error.empty() ? "provider_unavailable" : response.error;
        return result;
    }
    const std::string effectiveSpeechAct = channel->second.conversation
                                               ? (response.proposal.speechAct.empty()
                                                      ? request.context.speechAct
                                                      : response.proposal.speechAct)
                                               : request.context.speechAct;
    const auto parsedSpeechAct = speechActFromString(effectiveSpeechAct);
    if (channel->second.conversation && context.speechActClassificationRequired &&
        parsedSpeechAct == SpeechAct::Unknown) {
        result.error = "invalid_speech_act";
        return result;
    }
    const bool worldActionAllowed =
        channel->second.conversation && context.speechActClassificationRequired
            ? permitsWorldAction(parsedSpeechAct, world_.content_.conversationPolicy)
            : channel->second.defaultWorldActionAllowed;
    // Autonomous channels must produce an action. Conversation channels may
    // validly refuse or answer without one, so their empty action_id is kept.
    if (response.ok && !channel->second.conversation && worldActionAllowed &&
        response.proposal.actionId.empty()) {
        result.error = "world_action_required";
        return result;
    }
    if (response.proposal.worldRevision != context.worldRevision ||
        response.proposal.npcStateVersion != context.npcStateVersion) {
        result.error = "stale_intent_proposal";
        return result;
    }
    if (response.proposal.confidence < 0.0 || response.proposal.confidence > 1.0) {
        result.error = "invalid_intent_confidence";
        return result;
    }
    for (const auto& stimulusId : response.proposal.citedStimulusIds) {
        const auto foundStimulus = std::find_if(context.stimuli.begin(), context.stimuli.end(),
                                                [&stimulusId](const auto& stimulus) { return stimulus.id == stimulusId; });
        if (foundStimulus == context.stimuli.end()) {
            result.error = "unknown_stimulus_citation";
            return result;
        }
    }
    for (const auto& [needId, expected] : response.proposal.expectedRelief) {
        if (expected < 0.0 || expected > 1.0 || !world_.state_.npcs.at(request.npcId).needs.contains(needId)) {
            result.error = "invalid_expected_relief";
            return result;
        }
    }
    // A conversation turn is not successful merely because its structured
    // action proposal is valid.  Empty or whitespace-only dialogue would make
    // the player-facing interaction disappear while still writing a
    // dialogue.turn event.  Keep this boundary in Runtime so every provider
    // (including future adapters) follows the same contract; the LLM adapter
    // receives one bounded repair opportunity in executeTurn().
    if (channel->second.conversation &&
        response.dialogue.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.error = "missing_dialogue";
        return result;
    }
    if (response.proposal.actionId.empty()) {
        if (worldActionAllowed && response.proposal.worldActionRequested) {
            result.error = "world_action_required";
            return result;
        }
        auto& mutableNpc = world_.state_.npcs.at(request.npcId);
        if (!request.turnId.empty()) world_.state_.processedTurnIds.insert(request.turnId);
        ++mutableNpc.stateVersion;
        mutableNpc.memories.push_back(Memory{channel->second.memoryKind, request.playerInput, world_.state_.now,
                                             response.proposal.confidence});
        const auto target = channel->second.eventTarget == "player" ? kPlayerId :
                            channel->second.eventTarget == "self" ? "self" : channel->second.eventTarget;
        const auto event = world_.addEvent(
            channel->second.eventType, request.npcId, target, response.dialogue,
            Json{{"turn_id", request.turnId}, {"action_id", ""}, {"trigger", request.trigger},
                 {"speech_act", effectiveSpeechAct}, {"world_action_requested", false},
                 {"utterance_understanding", response.proposal.utteranceUnderstanding},
                 {"process_status", "conversation_only"}, {"provider", result.provider},
                 {"context_world_revision", response.proposal.worldRevision},
                 {"context_npc_state_version", response.proposal.npcStateVersion}});
        result.accepted = true;
        result.intent = response.proposal.intent;
        result.motivation = response.proposal.rationale;
        result.dialogue = response.dialogue;
        result.event = event;
        world_.syncNpcEntityState();
        return result;
    }
    if (!worldActionAllowed) {
        result.error = "speech_act_disallows_world_action";
        return result;
    }
    const auto validation = ActionValidator::validate(context, response.proposal.actionId,
                                                      response.proposal.parameters);
    if (!validation.valid) {
        result.error = validation.code;
        return result;
    }
    const auto candidate = std::find_if(context.candidates.begin(), context.candidates.end(),
                                        [&response](const auto& item) {
                                            return item.actionId == response.proposal.actionId;
                                        });
    if (candidate == context.candidates.end()) {
        result.error = "unknown_action_id";
        return result;
    }
    auto& mutableNpc = world_.state_.npcs.at(request.npcId);
    if (mutableNpc.activity.process) {
        double incomingSalience = channel->second.conversation ? 1.0 : 0.0;
        for (const auto& stimulus : context.stimuli) {
            incomingSalience = std::max(incomingSalience, stimulus.effectiveSalience);
        }
        if (!mutableNpc.activity.process->interruptible ||
            incomingSalience < mutableNpc.activity.process->interruptSalienceThreshold) {
            result.error = "npc_busy";
            return result;
        }
        world_.cancelProcess(request.npcId, *mutableNpc.activity.process, request.trigger);
        mutableNpc.activity = NpcActivity{};
    }
    std::map<std::string, int> reservations = candidate->reservationMode == "input_resources"
                                                   ? candidate->reservations
                                                   : std::map<std::string, int>{};
    const std::string processId =
        "process:" + request.npcId + ":" +
        (request.turnId.empty() ? std::to_string(world_.state_.events.size() + 1) : request.turnId);
    if (candidate->preemptionPolicy == "lower_priority") {
        // Plan all suspensions before mutating state, then apply one
        // deterministic arbitration decision. This prevents an impossible
        // request from leaving some victims suspended.
        const auto victims = world_.planResourcePreemption(request.npcId, reservations, candidate->priority);
        if (!victims) {
            transaction.rollback();
            result.error = "insufficient_resource";
            return result;
        }
        for (const auto& victimId : *victims) {
            auto& victim = world_.state_.npcs.at(victimId);
            world_.suspendProcess(victimId, *victim.activity.process, "resource_preempted",
                           Json{{"preempted_by", request.npcId},
                                {"preempting_process_id", processId},
                                {"preempting_priority", candidate->priority}});
        }
    }
    for (const auto& [resource, amount] : reservations) {
        if (world_.availableResourceValue(resource) < amount) {
            transaction.rollback();
            result.error = "insufficient_resource";
            return result;
        }
    }
    if (candidate->durationMinutes <= 0 &&
        !world_.applyActionEffects(request.npcId, candidate->actionId, nullptr, &response.proposal.parameters)) {
        transaction.rollback();
        result.error = "insufficient_resource";
        return result;
    }
    result.accepted = true;
    if (!request.turnId.empty()) world_.state_.processedTurnIds.insert(request.turnId);
    ++mutableNpc.stateVersion;
    if (candidate->durationMinutes > 0) {
        for (const auto& [resource, amount] : reservations) world_.state_.resources[resource].reserved += amount;
        const std::string goalId = "goal:" + request.npcId + ":" + processId.substr(processId.find(':') + 1);
        mutableNpc.activity.goal = GoalState{goalId, response.proposal.intent, candidate->actionId,
                                             response.proposal.confidence, GoalStatus::Active, world_.state_.now,
                                             GameTime{world_.state_.now.minute + candidate->durationMinutes},
                                             response.proposal.rationale};
        ProcessExecution process;
        process.id = processId;
        process.goalId = goalId;
        process.actionId = candidate->actionId;
        process.recipeId = candidate->recipeId;
        process.planId = candidate->planId;
        process.trigger = request.trigger;
        process.startedAt = world_.state_.now;
        process.completesAt = GameTime{world_.state_.now.minute + candidate->durationMinutes};
        process.behaviorNodeId = candidate->planId;
        process.parameters = response.proposal.parameters;
        process.interruptible = candidate->interruptible;
        process.interruptSalienceThreshold = candidate->interruptSalienceThreshold;
        process.reservationMode = candidate->reservationMode;
        process.reservedResources = reservations;
        process.requiredResources = reservations;
        process.priority = candidate->priority;
        process.preemptionPolicy = candidate->preemptionPolicy;
        process.proposalWorldRevision = response.proposal.worldRevision;
        process.proposalNpcStateVersion = response.proposal.npcStateVersion;
        mutableNpc.activity.process = std::move(process);
        setActivityStatus(mutableNpc.activity, ActivityStatus::Active);
        applyNpcSignal(mutableNpc, NpcSignal::StartProcess);
    }
    mutableNpc.memories.push_back(Memory{channel->second.memoryKind,
                                         request.playerInput.empty() ? response.proposal.intent : request.playerInput,
                                         world_.state_.now, 1.0});
    const auto target = channel->second.eventTarget == "player" ? kPlayerId :
                        channel->second.eventTarget == "self" ? "self" : channel->second.eventTarget;
    const auto event = world_.addEvent(
        channel->second.eventType, request.npcId, target, response.dialogue,
        Json{{"turn_id", request.turnId}, {"process_id", processId},
             {"action_id", response.proposal.actionId}, {"trigger", request.trigger},
             {"speech_act", effectiveSpeechAct},
             {"utterance_understanding", response.proposal.utteranceUnderstanding},
             {"duration_minutes", candidate->durationMinutes},
             {"process_status", candidate->durationMinutes > 0 ? "running" : "completed"},
             {"intent", response.proposal.intent}, {"motivation", response.proposal.rationale},
             {"expected_outcome", response.proposal.expectedOutcome},
             {"cited_stimulus_ids", response.proposal.citedStimulusIds},
             {"tradeoffs", response.proposal.tradeoffs},
             {"expected_relief", response.proposal.expectedRelief},
             {"parameters", response.proposal.parameters},
             {"utility_score", candidate->utilityScore},
             {"utility_contributions", candidate->utilityContributions},
             {"confidence", response.proposal.confidence}, {"provider", result.provider},
             {"context_world_revision", response.proposal.worldRevision},
             {"context_npc_state_version", response.proposal.npcStateVersion}});
    result.actionId = response.proposal.actionId;
    result.intent = response.proposal.intent;
    result.motivation = response.proposal.rationale;
    result.dialogue = response.dialogue;
    result.event = event;
    world_.syncNpcEntityState();
    return result;
}

void WorldIntelligenceSystem::enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger,
                                         const std::string& channelId) {
    if (!world_.hasNpc(npcId)) return;
    const std::string selectedChannel = channelId.empty() ? world_.content_.autonomousChannelId : channelId;
    if (!world_.content_.decisionChannels.contains(selectedChannel)) return;
    const std::string turnId = "autonomous:" + npcId + ":" + std::to_string(world_.state_.now.minute) + ":" +
                               std::to_string(world_.state_.revision);
    if (world_.intelligence_->isTracked(turnId)) return;
    const auto request = makeTurnRequest(npcId, {}, turnId, selectedChannel, trigger);
    const auto submitted = world_.intelligence_->submit(request);
    if (!submitted.accepted) return;
    world_.intelligence_->track(turnId);
    world_.state_.npcs.at(npcId).lastAutonomousTurn = world_.state_.now;
}

std::size_t WorldIntelligenceSystem::pumpAsyncTurns() {
    if (!world_.intelligence_) return 0;
    const auto results = world_.intelligence_->drain();
    std::size_t consumed = 0;
    for (auto& queued : results) {
        ++consumed;
        world_.intelligence_->forget(queued.request.turnId);
        const auto turn = commitTurn(queued.request, std::move(queued.response));
        if (turn.accepted) continue;
        const auto retryable = [](const std::string& code) {
            return code == "missing_action_parameter" || code == "unknown_action_parameter" ||
                   code == "invalid_action_parameter_type" || code == "unexpected_action_parameters" ||
                   code == "invalid_action_parameters" || code == "invalid_action_parameter_value" ||
                   code == "invalid_expected_relief";
        };
        const auto queuedChannel = world_.content_.decisionChannels.find(queued.request.channelId);
        const bool allowInterveningWorldChanges =
            queuedChannel != world_.content_.decisionChannels.end() &&
            queuedChannel->second.allowInterveningWorldChanges;
        if (queued.provider == "llama.cpp" && queued.request.validationFeedback.empty() && retryable(turn.error) &&
            (!allowInterveningWorldChanges || world_.state_.revision >= queued.request.worldRevision) &&
            world_.hasNpc(queued.request.npcId) &&
            world_.state_.npcs.at(queued.request.npcId).stateVersion == queued.request.npcStateVersion) {
            auto repair = queued.request;
            repair.validationFeedback = Json{
                {"attempt", 1},
                {"code", turn.error},
                {"message", "Runtime rejected a structured proposal field; repair only the declared plan input or expected relief keys."},
                {"previous_turn_id", queued.request.turnId}};
            const auto submitted = world_.intelligence_->submit(std::move(repair));
            if (submitted.accepted) {
                world_.intelligence_->track(queued.request.turnId);
                continue;
            }
        }
        // An asynchronous result is observable even when it arrives too late
        // or fails validation. This is an audit fact, not a world mutation.
        WorldService::TransactionScope transaction(world_);
        const bool expired = turn.error == "stale_intent_proposal" ||
                             turn.error == "decision_expired" ||
                             (!allowInterveningWorldChanges &&
                              world_.state_.revision != queued.request.worldRevision) ||
                             !world_.hasNpc(queued.request.npcId) ||
                             world_.state_.npcs.at(queued.request.npcId).stateVersion != queued.request.npcStateVersion;
        world_.addEvent(expired ? "npc.intent_expired" : "npc.intent_rejected", queued.request.npcId, "self",
                 expired ? "An asynchronous intent arrived after its decision context expired."
                         : "An asynchronous intent was rejected by the runtime validator.",
                 Json{{"turn_id", queued.request.turnId}, {"channel", queued.request.channelId},
                      {"trigger", queued.request.trigger}, {"error", turn.error},
                      {"request_world_revision", queued.request.worldRevision},
                      {"current_world_revision", world_.state_.revision},
                      {"request_npc_state_version", queued.request.npcStateVersion},
                      {"created_at", queued.request.createdAt.minute},
                      {"deadline", queued.request.deadline.minute},
                      {"current_minute", world_.state_.now.minute},
                      {"provider", queued.provider}});
    }
    return consumed;
}

std::size_t WorldIntelligenceSystem::pendingAsyncTurns() const {
    return world_.intelligence_ ? world_.intelligence_->pending() : 0;
}

std::size_t WorldIntelligenceSystem::pendingAsyncTurns(const WorldService& world) {
    return world.intelligence_ ? world.intelligence_->pending() : 0;
}

std::size_t WorldIntelligenceSystem::waitForAsyncTurns(std::int64_t timeoutMilliseconds) {
    if (timeoutMilliseconds <= 0 || !world_.intelligence_) return pumpAsyncTurns();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds);
    std::size_t consumed = 0;
    while (world_.intelligence_->pending() > 0 && std::chrono::steady_clock::now() < deadline) {
        consumed += pumpAsyncTurns();
        if (world_.intelligence_->pending() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    consumed += pumpAsyncTurns();
    return consumed;
}

}  // namespace tide_marks
