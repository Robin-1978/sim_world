#include "provider/provider.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace tide_marks {

namespace {
std::string shellQuote(const std::string& text) {
    std::string result("'");
    for (const char ch : text) result += (ch == '\'' ? "'\\''" : std::string(1, ch));
    result += '\'';
    return result;
}
std::string actionLabel(const DecisionContext& context, const std::string& actionId) {
    for (const auto& candidate : context.candidates) if (candidate.actionId == actionId) return candidate.label;
    return actionId;
}
struct ProviderJsonResult {
    bool ok{false};
    Json value = Json::object();
    std::string error;
};

ProviderJsonResult requestProviderJson(const LlamaCppConfig& config, const Json& request,
                                       const std::string& stage) {
    const auto temp = std::filesystem::temp_directory_path() /
                      ("tide_marks_llm_" + stage + "_" +
                       std::to_string(static_cast<long long>(std::rand())) + ".json");
    {
        std::ofstream output(temp);
        if (!output) return {false, {}, "provider_temp_file_failed"};
        output << request.dump();
    }
    std::string command = "curl --silent --show-error --max-time " +
                          std::to_string(std::max(1, config.timeoutMs / 1000)) +
                          " -H 'Content-Type: application/json'";
    if (!config.apiKeyEnv.empty()) {
        if (const char* key = std::getenv(config.apiKeyEnv.c_str()); key != nullptr && *key != '\0') {
            command += " -H " + shellQuote("Authorization: Bearer " + std::string(key));
        }
    }
    command += " --data-binary @" + shellQuote(temp.string()) + " " +
               shellQuote(config.baseUrl + "/chat/completions");
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        std::filesystem::remove(temp);
        return {false, {}, "curl_unavailable"};
    }
    std::string response;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        response += buffer.data();
    }
    const int result = pclose(pipe);
    std::filesystem::remove(temp);
    if (result != 0 || response.empty()) return {false, {}, "provider_request_failed"};
    try {
        const Json outer = Json::parse(response);
        const auto& choice = outer.at("choices").at(0);
        if (const char* debug = std::getenv("TIDE_MARKS_DEBUG_PROVIDER");
            debug != nullptr && *debug != '\0' && std::string(debug) != "0") {
            std::cerr << "provider_debug stage=" << stage
                      << " finish_reason=" << choice.value("finish_reason", "")
                      << " usage=" << outer.value("usage", Json::object()).dump()
                      << " content=" << choice.at("message").value("content", "") << "\n";
        }
        if (choice.value("finish_reason", "") == "length") {
            return {false, {}, "provider_truncated:" + stage};
        }
        return {true, Json::parse(choice.at("message").at("content").get<std::string>()), {}};
    } catch (const std::exception& error) {
        return {false, {}, "invalid_provider_json:" + stage + ":" + error.what()};
    }
}
}  // namespace
ProviderResponse MockProvider::decide(const DecisionContext& context, const std::string&) {
    if (context.candidates.empty()) {
        return ProviderResponse{false, {}, {}, "no_candidates"};
    }
    std::string selectedSpeechAct = context.speechAct;
    bool worldActionAllowed = context.worldActionAllowed;
    if (selectedSpeechAct.empty() && context.speechActClassificationRequired) {
        // The offline provider is deterministic test infrastructure. It does
        // not inspect player text: it selects an authored action-capable speech
        // act when one is available. Production conversation uses an LLM.
        const auto permitted = std::find_if(context.speechActs.begin(), context.speechActs.end(),
                                            [](const auto& item) { return item.worldActionAllowed; });
        selectedSpeechAct = permitted == context.speechActs.end() ? "statement" : permitted->id;
        worldActionAllowed = permitted != context.speechActs.end();
    }
    const auto speechAct = speechActFromString(selectedSpeechAct);
    if (!worldActionAllowed) {
        IntentProposal proposal;
        proposal.intent = toString(speechAct);
        proposal.speechAct = toString(speechAct);
        proposal.worldActionRequested = false;
        proposal.rationale = "The input is conversational and does not request a world action.";
        proposal.expectedOutcome = "The NPC responds without changing world facts.";
        proposal.confidence = 1.0;
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        const std::string name = context.npcName.empty() ? context.npcId : context.npcName;
        const std::string dialogue = speechAct == SpeechAct::Greeting
                                         ? "Hello. It is good to meet you."
                                         : speechAct == SpeechAct::Farewell
                                             ? "Goodbye. I will remember our conversation."
                                             : name + " listens carefully before answering.";
        return ProviderResponse{true, std::move(proposal), dialogue, {}};
    }
    std::string selected = context.candidates.front().actionId;
    const auto label = actionLabel(context, selected);
    const std::string name = context.npcName.empty() ? context.npcId : context.npcName;
    IntentProposal proposal;
    proposal.actionId = selected;
    proposal.intent = "respond";
    proposal.speechAct = selectedSpeechAct.empty() ? "autonomous" : selectedSpeechAct;
    proposal.worldActionRequested = true;
    proposal.rationale = "The authored candidate best fits the current context.";
    proposal.expectedOutcome = "The selected authored action is attempted and validated by Runtime.";
    proposal.confidence = 1.0;
    proposal.worldRevision = context.worldRevision;
    proposal.npcStateVersion = context.npcStateVersion;
    if (!context.stimuli.empty()) proposal.citedStimulusIds.push_back(context.stimuli.front().id);
    proposal.tradeoffs.push_back("Chose the strongest authored satisfier after comparing drive pressure and constraints.");
    for (const auto& drive : context.drives) {
        const auto expected = drive.expectedRelief.find(selected);
        if (expected != drive.expectedRelief.end()) proposal.expectedRelief[drive.id] = expected->second;
    }
    return ProviderResponse{true, std::move(proposal),
                            name + " considers the request and chooses to " + label + ".", {}};
}

LlamaCppProvider::LlamaCppProvider(LlamaCppConfig config) : config_(std::move(config)) {
    if (config_.decisionPolicies.empty()) {
        config_.decisionPolicies.emplace("default", config_.fallbackPolicy);
    }
    config_.fallbackPolicy = config_.decisionPolicies.begin()->second;
}

ProviderResponse LlamaCppProvider::decide(const DecisionContext& context, const std::string& playerInput) {
    Json candidates = Json::array();
    for (const auto& candidate : context.candidates) {
        candidates.push_back(Json{{"action_id", candidate.actionId}, {"plan_id", candidate.planId},
                                  {"label", candidate.label}, {"duration_minutes", candidate.durationMinutes},
                                  {"interruptible", candidate.interruptible},
                                  {"interrupt_salience_threshold", candidate.interruptSalienceThreshold},
                                  {"reservation_mode", candidate.reservationMode}});
    }
    Json stimuli = Json::array();
    for (const auto& stimulus : context.stimuli) {
        stimuli.push_back(Json{{"id", stimulus.id}, {"source_event", stimulus.sourceEvent},
                               {"source_sequence", stimulus.sourceSequence}, {"category", stimulus.category},
                               {"actor", stimulus.actor}, {"target", stimulus.target},
                               {"observed_minute", stimulus.observedAt.minute},
                               {"expires_minute", stimulus.expiresAt.minute},
                               {"visibility", stimulus.visibilityScope}, {"confidence", stimulus.sourceConfidence},
                               {"relevance", stimulus.relevance}, {"intensity", stimulus.intensity},
                               {"urgency", stimulus.urgency}, {"novelty", stimulus.novelty},
                               {"valence", stimulus.valence}, {"salience", stimulus.effectiveSalience}});
    }
    Json constraints = Json::array();
    for (const auto& constraint : context.constraints) {
        constraints.push_back(Json{{"id", constraint.id}, {"category", constraint.category},
                                   {"scope", constraint.scope}, {"source", constraint.source},
                                   {"hard", constraint.hard}, {"reason", constraint.reason}});
    }
    Json speechActs = Json::array();
    for (const auto& speechAct : context.speechActs) {
        speechActs.push_back(Json{{"id", speechAct.id},
                                  {"description", speechAct.description},
                                  {"world_action_allowed", speechAct.worldActionAllowed}});
    }
    Json drives = Json::array();
    for (const auto& drive : context.drives) {
        drives.push_back(Json{{"id", drive.id}, {"domain", drive.domain}, {"dimension", drive.dimension},
                              {"deficit", drive.deficit}, {"drive", drive.drive},
                              {"sensitivity", drive.sensitivity}, {"evidence_confidence", drive.evidenceConfidence},
                              {"critical", drive.critical}, {"expected_relief", drive.expectedRelief}});
    }
    const auto configuredPolicy = config_.decisionPolicies.find(context.providerPolicy);
    if (configuredPolicy == config_.decisionPolicies.end()) {
        return ProviderResponse{false, {}, {}, "unknown_provider_policy:" + context.providerPolicy};
    }
    const DecisionPolicy& policy = configuredPolicy->second;

    auto configureDecoding = [](Json& request, const DecisionPolicy& selectedPolicy) {
        request["chat_template_kwargs"] = Json{{"enable_thinking", selectedPolicy.enableThinking}};
        if (selectedPolicy.enableThinking) {
            // llama.cpp applies this sampler budget only through the top-level
            // reasoning_budget_tokens field. A template-only thinking_budget is
            // ignored by current servers and can therefore consume max_tokens.
            request["reasoning_budget_tokens"] = selectedPolicy.reasoningBudgetTokens;
        } else {
            // Be explicit for servers whose model defaults enable reasoning.
            request["reasoning_effort"] = "none";
        }
    };

    Json utteranceUnderstanding = Json::object();
    std::string selectedSpeechAct = context.speechAct;
    bool worldActionPermitted = context.worldActionAllowed;
    if (context.speechActClassificationRequired) {
        const auto understandingPolicy = config_.decisionPolicies.find("utterance_understanding");
        if (understandingPolicy == config_.decisionPolicies.end()) {
            return ProviderResponse{false, {}, {}, "missing_provider_policy:utterance_understanding"};
        }
        const Json understandingUser{{"npc_id", context.npcId},
                                     {"character", context.character},
                                     {"player_input", playerInput},
                                     {"speech_act_candidates", speechActs}};
        Json speechActIds = Json::array();
        for (const auto& item : context.speechActs) speechActIds.push_back(item.id);
        const std::string understandingPrompt =
            "Interpret the meaning of the player's utterance in its language and context. "
            "Classify by communicative intent, not by individual words or punctuation. "
            "Choose exactly one speech_act from speech_act_candidates using the supplied descriptions; "
            "polite wording does not by itself turn a request for explanation into a request to alter the world. "
            "Set action_requested only when the speaker asks the NPC to perform, change, stop, promise, or negotiate "
            "an in-world action. Do not choose an NPC goal, action_id, dialogue, memory, resource, relationship, or world change.";
        const Json understandingSchema{
            {"type", "object"}, {"additionalProperties", false},
            {"required", Json::array({"speech_act", "communication_goal", "action_requested",
                                       "referenced_entities", "ambiguity"})},
            {"properties", Json{{"speech_act", Json{{"type", "string"}, {"enum", speechActIds}}},
                                 {"communication_goal", Json{{"type", "string"}}},
                                 {"action_requested", Json{{"type", "boolean"}}},
                                 {"referenced_entities", Json{{"type", "array"},
                                                               {"items", Json{{"type", "string"}}}}},
                                 {"ambiguity", Json{{"type", "string"}}}}}};
        Json understandingRequest{
            {"model", config_.model}, {"temperature", config_.temperature},
            {"max_tokens", understandingPolicy->second.maxTokens},
            {"messages", Json::array({Json{{"role", "system"}, {"content", understandingPrompt}},
                                      Json{{"role", "user"}, {"content", understandingUser.dump()}}})},
            {"response_format", Json{{"type", "json_schema"},
                                      {"json_schema", Json{{"name", "tide_marks_utterance_understanding"},
                                                            {"strict", true}, {"schema", understandingSchema}}}}}};
        configureDecoding(understandingRequest, understandingPolicy->second);
        auto understood = requestProviderJson(config_, understandingRequest, "understanding");
        if (!understood.ok) return ProviderResponse{false, {}, {}, understood.error};
        utteranceUnderstanding = std::move(understood.value);
        if (!utteranceUnderstanding.contains("speech_act") ||
            !utteranceUnderstanding.at("speech_act").is_string()) {
            return ProviderResponse{false, {}, {}, "missing_speech_act_understanding"};
        }
        selectedSpeechAct = utteranceUnderstanding.at("speech_act").get<std::string>();
        const auto permission = std::find_if(
            context.speechActs.begin(), context.speechActs.end(),
            [&selectedSpeechAct](const auto& item) { return item.id == selectedSpeechAct; });
        if (permission == context.speechActs.end()) {
            return ProviderResponse{false, {}, {}, "unknown_speech_act"};
        }
        worldActionPermitted = permission->worldActionAllowed;
    }

    const Json userContent{{"npc_id", context.npcId}, {"time", context.now.toString()},
                           {"decision_channel", context.channel}, {"decision_trigger", context.trigger},
                           {"utterance_understanding", utteranceUnderstanding},
                           {"selected_speech_act", selectedSpeechAct},
                           {"world_action_allowed", worldActionPermitted},
                           {"perception", context.perception}, {"memories", context.memories},
                           {"knowledge", context.knowledge}, {"experiences", context.experiences},
                           {"character", context.character}, {"internal_drives", context.internalDrives},
                           {"drives", drives},
                           {"external_stimuli", context.externalStimuli},
                           {"stimuli", stimuli},
                           {"structural_constraints", context.structuralConstraints},
                           {"constraints", constraints},
                           {"current_activity", context.currentActivity},
                           {"player_input", playerInput},
                           {"candidates", candidates}};
    std::vector<std::string> stimulusIds;
    stimulusIds.reserve(context.stimuli.size());
    for (const auto& stimulus : context.stimuli) stimulusIds.push_back(stimulus.id);
    std::vector<std::string> needIds;
    needIds.reserve(context.drives.size());
    for (const auto& drive : context.drives) needIds.push_back(drive.id);
    const std::string systemPrompt =
        "Return one JSON object only. The utterance_understanding stage has already classified the player's communication. "
        "Choose exactly one action_id from the supplied candidates, or use an empty action_id for a conversation-only response or refusal. "
        "A non-empty action_id starts an authored world process and may change world facts; it is never merely a dialogue style or answer type. "
        "For a conversation-only answer, action_id MUST be empty even when a candidate label sounds like 'explain' or 'report'. "
        "world_action_allowed is derived by Runtime from the selected_speech_act. If it is false, action_id MUST be empty and the dialogue must answer without promising or performing a world action. "
        "For information_request or inquiry, answer the exact question from current_activity, perception, knowledge, memories and experiences; do not invent a current activity. "
        "If current_activity.status is idle, say that you are not in an active process instead of inventing work. "
        "Do not ask the player to repeat information already present in player_input. "
        "cited_stimulus_ids may contain only IDs from the supplied stimuli array; the valid stimulus IDs for this request are " +
        Json(stimulusIds).dump() + ". If that list is empty, cited_stimulus_ids MUST be []. Never use a drive or need ID as a stimulus ID. "
        "expected_relief may contain only need IDs from the supplied drives array; the valid need IDs for this request are " +
        Json(needIds).dump() + ". Use empty arrays/objects when none are supplied, and expected_relief MUST be empty when action_id is empty. "
        "confidence and expected_relief values must be numbers from 0 to 1. dialogue must be one or two natural first-person sentences spoken by the NPC, never a dump of context fields. "
        "knowledge and experiences are NPC-scoped, sourced context; they may guide a proposal but never authorize bypassing current facts or constraints. "
        "motivation and tradeoffs explain the decision; they are not actions. Do not use markdown or add explanation outside the JSON object.";
    Json messages = Json::array();
    messages.push_back(Json{{"role", "system"}, {"content", systemPrompt}});
    messages.push_back(Json{{"role", "user"}, {"content", userContent.dump()}});
    const Json responseSchema{
        {"type", "object"}, {"additionalProperties", false},
        {"required", Json::array({"action_id", "intent", "motivation", "expected_outcome", "confidence",
                                   "dialogue", "cited_stimulus_ids", "tradeoffs", "expected_relief"})},
        {"properties", Json{{"action_id", Json{{"type", "string"}}},
                             {"intent", Json{{"type", "string"}}},
                             {"motivation", Json{{"type", "string"}}},
                             {"expected_outcome", Json{{"type", "string"}}},
                             {"confidence", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
                             {"dialogue", Json{{"type", "string"}}},
                             {"cited_stimulus_ids", Json{{"type", "array"}, {"items", Json{{"type", "string"}}}}},
                             {"tradeoffs", Json{{"type", "array"}, {"items", Json{{"type", "string"}}}}},
                             {"expected_relief", Json{{"type", "object"},
                                                       {"additionalProperties", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}}}}}}};
    Json request{{"model", config_.model}, {"temperature", config_.temperature}, {"max_tokens", policy.maxTokens},
                 {"messages", messages},
                 {"response_format", Json{{"type", "json_schema"},
                                           {"json_schema", Json{{"name", "tide_marks_intent_proposal"},
                                                                  {"strict", true}, {"schema", responseSchema}}}}}};
    configureDecoding(request, policy);
    auto completed = requestProviderJson(config_, request, "decision");
    if (!completed.ok) return ProviderResponse{false, {}, {}, completed.error};
    try {
        const Json& answer = completed.value;
        const std::string action = answer.value("action_id", "");
        const auto allowed = std::find_if(context.candidates.begin(), context.candidates.end(),
                                          [&action](const auto& candidate) { return candidate.actionId == action; });
        if (!action.empty() && allowed == context.candidates.end()) {
            return ProviderResponse{false, {}, {}, "unknown_action_id"};
        }
        IntentProposal proposal;
        proposal.actionId = action;
        proposal.intent = answer.value("intent", "respond");
        proposal.rationale = answer.value("motivation", "");
        proposal.expectedOutcome = answer.value("expected_outcome", "");
        proposal.confidence = std::clamp(answer.value("confidence", 0.0), 0.0, 1.0);
        proposal.worldRevision = context.worldRevision;
        proposal.npcStateVersion = context.npcStateVersion;
        proposal.citedStimulusIds = answer.value("cited_stimulus_ids", std::vector<std::string>{});
        proposal.tradeoffs = answer.value("tradeoffs", std::vector<std::string>{});
        const auto expectedRelief = answer.value("expected_relief", Json::object());
        if (!expectedRelief.is_object()) return ProviderResponse{false, {}, {}, "invalid_expected_relief"};
        for (auto item = expectedRelief.begin(); item != expectedRelief.end(); ++item) {
            if (!item.value().is_number()) return ProviderResponse{false, {}, {}, "invalid_expected_relief"};
            proposal.expectedRelief[item.key()] = std::clamp(item.value().get<double>(), 0.0, 1.0);
        }
        proposal.speechAct = selectedSpeechAct;
        proposal.utteranceUnderstanding = utteranceUnderstanding;
        proposal.worldActionRequested = !proposal.actionId.empty();
        return ProviderResponse{true, std::move(proposal), answer.value("dialogue", ""), {}};
    } catch (const std::exception& error) {
        return ProviderResponse{false, {}, {}, std::string("invalid_provider_json:") + error.what()};
    }
}


}  // namespace tide_marks
