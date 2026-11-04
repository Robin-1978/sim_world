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
                      << " content=" << choice.at("message").value("content", "")
                      << " tool_calls=" << choice.at("message").value("tool_calls", Json::array()).dump()
                      << "\n";
        }
        if (choice.value("finish_reason", "") == "length") {
            return {false, {}, "provider_truncated:" + stage};
        }
        const auto& message = choice.at("message");
        // OpenAI-compatible providers may return a forced function call
        // instead of assistant content. Treat the function arguments as the
        // structured payload; no natural-language repair is attempted.
        if (message.contains("tool_calls") && message.at("tool_calls").is_array() &&
            !message.at("tool_calls").empty()) {
            const auto& function = message.at("tool_calls").at(0).at("function");
            return {true, Json::parse(function.at("arguments").get<std::string>()), {}};
        }
        return {true, Json::parse(message.at("content").get<std::string>()), {}};
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
    return decide(context, playerInput, Json::object());
}

ProviderResponse LlamaCppProvider::decide(const DecisionContext& context, const std::string& playerInput,
                                          const Json& validationFeedback) {
    Json candidates = Json::array();
    for (const auto& candidate : context.candidates) {
        candidates.push_back(Json{{"action_id", candidate.actionId}, {"plan_id", candidate.planId},
                                  {"label", candidate.label}, {"duration_minutes", candidate.durationMinutes},
                                  {"interruptible", candidate.interruptible},
                                  {"interrupt_salience_threshold", candidate.interruptSalienceThreshold},
                                  {"reservation_mode", candidate.reservationMode},
                                  {"reservations", candidate.reservations},
                                  {"parameter_schema", candidate.parameterSchema},
                                  {"priority", candidate.priority},
                                  {"preemption_policy", candidate.preemptionPolicy},
                                  {"utility_score", candidate.utilityScore},
                                  {"utility_contributions", candidate.utilityContributions}});
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
                                     {"runtime_metadata", context.runtimeMetadata},
                                     {"character", context.character},
                                     {"player_input", playerInput},
                                     {"speech_act_candidates", speechActs}};
        Json speechActIds = Json::array();
        for (const auto& item : context.speechActs) speechActIds.push_back(item.id);
        const std::string understandingPrompt =
            "Interpret the meaning of the player's utterance in its language and context. "
            "runtime_metadata is an audit envelope only; do not use it as observable world state or infer facts from it. "
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
                           {"runtime_metadata", context.runtimeMetadata},
                           {"decision_channel", context.channel}, {"decision_trigger", context.trigger},
                           {"world_snapshot", context.worldSnapshot},
                           {"self_state", context.selfState},
                           {"plan_catalog", context.planCatalog},
                           {"utterance_understanding", utteranceUnderstanding},
                           {"selected_speech_act", selectedSpeechAct},
                           {"world_action_allowed", worldActionPermitted},
                           {"perception", context.perception}, {"memories", context.memories},
                           {"knowledge", context.knowledge}, {"experiences", context.experiences},
                           {"character", context.character}, {"internal_drives", context.internalDrives},
                           {"private_character", context.privateCharacter},
                           {"drives", drives},
                           {"external_stimuli", context.externalStimuli},
                           {"stimuli", stimuli},
                           {"structural_constraints", context.structuralConstraints},
                           {"constraints", constraints},
                           {"current_activity", context.currentActivity},
                           {"player_input", playerInput},
                           {"validation_feedback", validationFeedback},
                           {"candidates", candidates}};
    std::vector<std::string> stimulusIds;
    stimulusIds.reserve(context.stimuli.size());
    for (const auto& stimulus : context.stimuli) stimulusIds.push_back(stimulus.id);
    std::vector<std::string> needIds;
    needIds.reserve(context.drives.size());
    for (const auto& drive : context.drives) needIds.push_back(drive.id);
    const std::string systemPrompt =
        "Return one JSON object only. The utterance_understanding stage has already classified the player's communication. "
        "runtime_metadata is an audit envelope containing identifiers and versions; do not treat it as observable world state "
        "and do not use it to infer facts, motives, locations, resources, or actions. "
        "Choose exactly one action_id from plan_catalog, or use an empty action_id for a conversation-only response or refusal. "
        "Each non-empty action_id selects one registered behavior plan: Runtime will execute its complete content-authored behavior tree. "
        "The behavior_tree shown in plan_catalog is descriptive context; do not invent or partially execute its nodes in this response. "
        "If the selected plan has a parameter_schema, fill only those declared parameters (for example destination); "
        "parameters are proposals for Runtime validation, not direct world writes. Use an empty object when none are needed. "
        "A non-empty action_id starts an authored world process and may change world facts; it is never merely a dialogue style or answer type. "
        "For a conversation-only answer, action_id MUST be empty even when a candidate label sounds like 'explain' or 'report'. "
        "world_action_allowed is derived by Runtime from the selected_speech_act. If it is false, action_id MUST be empty and the dialogue must answer without promising or performing a world action. "
        "For information_request or inquiry, answer the exact question from current_activity, perception, knowledge, memories and experiences; do not invent a current activity. "
        "If current_activity.status is idle, use exactly one short sentence in this shape: \"I am not in an active process right now; I am at <location>.\" Replace <location> with self_state.location or world_snapshot.observer_location and add no duty, goal, role, or inferred activity. "
        "When status is idle, do not relabel it as resting, waiting, monitoring, keeping an eye on something, working, or another activity unless that activity is explicitly present in current_activity or an observable fact. "
        "private_character contains internal motives and boundaries. Treat it as confidential: never reveal or quote a hidden goal, private boundary, or other private motive in dialogue, memories, or a player-facing event unless the supplied authored context explicitly grants a disclosure rule. "
        "An information_request about current activity must be answered from current_activity and observable facts, not by volunteering private_character. "
        "The player_input is already the complete question; never say you need to hear it, wait for it, or ask the player to repeat it. "
        "Do not ask the player to repeat information already present in player_input. "
        "cited_stimulus_ids may contain only IDs from the supplied stimuli array; the valid stimulus IDs for this request are " +
        Json(stimulusIds).dump() + ". If that list is empty, cited_stimulus_ids MUST be []. Never use a drive or need ID as a stimulus ID. "
        "expected_relief may contain only need IDs from the supplied drives array; the valid need IDs for this request are " +
        Json(needIds).dump() + ". Use empty arrays/objects when none are supplied, and expected_relief MUST be empty when action_id is empty. "
        "confidence and expected_relief values must be numbers from 0 to 1. dialogue must be one or two natural first-person sentences spoken by the NPC, never a dump of context fields. "
        "knowledge and experiences are NPC-scoped, sourced context; they may guide a proposal but never authorize bypassing current facts or constraints. "
        "If validation_feedback is non-empty, repair only the reported action or parameter problem in one response. "
        "Do not repeat an invalid action or parameter, and do not invent a candidate outside plan_catalog. "
        "motivation and tradeoffs explain the decision; they are not actions. Do not use markdown or add explanation outside the JSON object.";
    Json messages = Json::array();
    messages.push_back(Json{{"role", "system"}, {"content", systemPrompt}});
    messages.push_back(Json{{"role", "user"}, {"content", userContent.dump()}});
    const Json responseSchema{
        {"type", "object"}, {"additionalProperties", false},
        {"required", Json::array({"action_id", "intent", "motivation", "expected_outcome", "confidence",
                                   "dialogue", "cited_stimulus_ids", "tradeoffs", "expected_relief", "parameters"})},
        {"properties", Json{{"action_id", Json{{"type", "string"}}},
                             {"intent", Json{{"type", "string"}}},
                             {"motivation", Json{{"type", "string"}}},
                             {"expected_outcome", Json{{"type", "string"}}},
                             {"confidence", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
                             {"dialogue", Json{{"type", "string"}}},
                             {"cited_stimulus_ids", Json{{"type", "array"}, {"items", Json{{"type", "string"}}}}},
                             {"tradeoffs", Json{{"type", "array"}, {"items", Json{{"type", "string"}}}}},
                             {"expected_relief", Json{{"type", "object"},
                                                       {"additionalProperties", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}}}},
                             {"parameters", Json{{"type", "object"}}}}}};
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
        proposal.parameters = answer.value("parameters", Json::object());
        if (!proposal.parameters.is_object()) return ProviderResponse{false, {}, {}, "invalid_action_parameters"};
        proposal.speechAct = selectedSpeechAct;
        proposal.utteranceUnderstanding = utteranceUnderstanding;
        proposal.worldActionRequested = !proposal.actionId.empty();
        return ProviderResponse{true, std::move(proposal), answer.value("dialogue", ""), {}};
    } catch (const std::exception& error) {
        return ProviderResponse{false, {}, {}, std::string("invalid_provider_json:") + error.what()};
    }
}

ProviderResponse LlamaCppProvider::proposeEvolution(const Json& context,
                                                     const std::string& evolutionLevel) {
    return proposeEvolution(context, evolutionLevel, Json::object());
}

ProviderResponse LlamaCppProvider::proposeEvolution(const Json& context,
                                                     const std::string& evolutionLevel,
                                                     const Json& validationFeedback) {
    const auto configuredPolicy = config_.decisionPolicies.find("evolution_proposal");
    if (configuredPolicy == config_.decisionPolicies.end()) {
        return ProviderResponse{false, {}, {}, "missing_provider_policy:evolution_proposal", Json::object()};
    }
    const DecisionPolicy& policy = configuredPolicy->second;
    const std::string systemPrompt =
        "You are the World Intelligence evolution proposer. Return exactly one JSON object and no prose. "
        "Propose a bounded declarative change for the supplied evolution level. "
        "Use only operation names and registered references from the context. "
        "For level1, the operation MUST be overlay_strategy, overlay_schedule, or register_behavior_recipe. For level2, the operation MUST be "
        "create_entity_archetype or register_behavior_recipe; never use overlay_strategy for level2. "
        "Cite only evidence event sequences present in the context. "
        "Never request Runtime Core mutation, arbitrary code, scripts, permissions, transactions, event schemas, "
        "or persistence changes. A proposal is only a suggestion: the Runtime will validate it, run it in an isolated "
        "sandbox, and require independent approval before activation. "
        "For level1, prefer an NPC strategy or behavior recipe change. For level2, prefer a content/archetype or "
        "behavior capability change that can be expressed by the registered declarative operations. "
        "The proposal object uses proposal_kind (content, knowledge, strategy, or declarative_capability); the operation "
        "create_entity_archetype separately uses kind for the entity kind. Every operation MUST use the exact keys operation and id. "
        "For create_entity_archetype, kind is REQUIRED and "
        "must be a short noun such as facility, place, or object; use optional "
        "default_metadata. For register_behavior_recipe use plan_id, duration_minutes, interruptible, available_to "
        "(a non-empty list of NPC ids from the supplied world), and a complete "
        "root behavior node with type/id/name/args/children. A recipe may declare a parameters JSON Schema for "
        "dynamic bindings such as destination. Do not use name, params, entity_type, recipe_id, or "
        "free-form behavior keys in place of the registered operation schema. Prefer one small valid operation over "
        "multiple speculative operations. A valid minimal Level 2 archetype operation is "
        "{\"operation\":\"create_entity_archetype\",\"id\":\"new_facility\",\"kind\":\"facility\"}. "
        "A valid minimal behavior recipe root is "
        "{\"type\":\"meta_action\",\"id\":\"root\",\"name\":\"relieve_need\",\"args\":{\"action\":\"rest\"},\"max_ticks\":0,\"children\":[]}. "
        "Do not claim that an operation was applied; provide a concise rationale in the rationale field. "
        "If validation_feedback is non-empty, repair only the reported schema/reference problem while keeping the "
        "same world evidence; do not argue with the validator or invent a new Runtime capability. "
        "If feedback says level_operation_mismatch for level2, replace the entire operation with a valid "
        "create_entity_archetype operation containing operation, id, kind and default_metadata; do not retain "
        "npc_id, peer_id, strategy, memory or event_type fields. If feedback says invalid_behavior_recipe, use a "
        "single meta_action root with a registered name from content.registered_meta_actions and children=[]; do not "
        "invent a node name or create an empty sequence/selector. If feedback says invalid_behavior_parameters, "
        "replace parameters with a JSON Schema such as {\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}; "
        "never put a runtime parameter value such as destination directly beside type.";
    Json responseSchema = Json::object();
    responseSchema["type"] = "object";
    responseSchema["additionalProperties"] = false;
    responseSchema["required"] = Json::array({"id", "proposal_kind", "evidence_event_sequences", "operations", "rationale"});
    responseSchema["properties"] = Json::object();
    responseSchema["properties"]["id"] = Json{{"type", "string"}, {"minLength", 1}};
    responseSchema["properties"]["proposal_kind"] = Json{{"type", "string"},
                                                             {"enum", Json::array({"content", "knowledge", "strategy", "declarative_capability"})}};
    Json evidenceItem = Json{{"type", "integer"}, {"minimum", 1}};
    if (context.contains("evidence_events") && context.at("evidence_events").is_array()) {
        Json validSequences = Json::array();
        for (const auto& event : context.at("evidence_events")) {
            if (event.is_object() && event.contains("sequence") && event.at("sequence").is_number_unsigned()) {
                validSequences.push_back(event.at("sequence"));
            }
        }
        if (!validSequences.empty()) evidenceItem["enum"] = std::move(validSequences);
    }
    responseSchema["properties"]["evidence_event_sequences"] =
        Json{{"type", "array"}, {"minItems", 1}, {"items", std::move(evidenceItem)}};
    Json createOperation = Json::object();
    createOperation["type"] = "object";
    createOperation["additionalProperties"] = false;
    createOperation["required"] = Json::array({"operation", "id", "kind", "default_metadata"});
    createOperation["properties"] = Json{
        {"operation", Json{{"const", "create_entity_archetype"}}},
        {"id", Json{{"type", "string"}, {"minLength", 1}}},
        {"kind", Json{{"type", "string"}, {"minLength", 1}}},
        {"default_metadata", Json{{"type", "object"}}}};

    Json strategyOperation = Json::object();
    strategyOperation["type"] = "object";
    strategyOperation["additionalProperties"] = false;
    strategyOperation["required"] = Json::array({"operation", "id", "npc_id", "peer_id", "min_peer_trust",
                                                   "strategy", "goal_stage", "memory", "event_type"});
    strategyOperation["properties"] = Json{
        {"operation", Json{{"const", "overlay_strategy"}}},
        {"id", Json{{"type", "string"}, {"minLength", 1}}},
        {"npc_id", Json{{"type", "string"}, {"minLength", 1}}},
        {"peer_id", Json{{"type", "string"}, {"minLength", 1}}},
        {"min_peer_trust", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"strategy", Json{{"type", "string"}, {"minLength", 1}}},
        {"goal_stage", Json{{"type", "integer"}, {"minimum", 0}}},
        {"memory", Json{{"type", "string"}}},
        {"event_type", Json{{"type", "string"}, {"minLength", 1}}}};

    Json scheduleOperation = Json::object();
    scheduleOperation["type"] = "object";
    scheduleOperation["additionalProperties"] = false;
    scheduleOperation["required"] = Json::array({"operation", "id", "npc_id", "slots"});
    Json scheduleSlot = Json{{"type", "object"}, {"additionalProperties", false},
                              {"required", Json::array({"start_minute", "end_minute", "location"})}};
    scheduleSlot["properties"] = Json{
        {"start_minute", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 1439}}},
        {"end_minute", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 1439}}},
        {"location", Json{{"type", "string"}, {"minLength", 1}}}};
    scheduleOperation["properties"] = Json{
        {"operation", Json{{"const", "overlay_schedule"}}},
        {"id", Json{{"type", "string"}, {"minLength", 1}}},
        {"npc_id", Json{{"type", "string"}, {"minLength", 1}}},
        {"slots", Json{{"type", "array"}, {"minItems", 1}, {"items", scheduleSlot}}}};

    Json node = Json::object();
    node["type"] = "object";
    node["additionalProperties"] = false;
    node["required"] = Json::array({"type", "id", "name", "args", "max_ticks", "children"});
    Json nodeName = Json{{"type", "string"}};
    Json registeredNames = Json::array();
    if (context.contains("content") && context.at("content").is_object()) {
        for (const auto& key : {"registered_meta_actions", "registered_conditions"}) {
            const auto names = context.at("content").value(key, Json::array());
            if (!names.is_array()) continue;
            for (const auto& name : names) if (name.is_string()) registeredNames.push_back(name);
        }
    }
    if (!registeredNames.empty()) nodeName["enum"] = std::move(registeredNames);
    node["properties"] = Json{
        {"type", Json{{"enum", Json::array({"sequence", "selector", "condition", "meta_action", "interruptible", "timeout"})}}},
        {"id", Json{{"type", "string"}, {"minLength", 1}}},
        {"name", std::move(nodeName)},
        {"args", Json{{"type", "object"}}},
        {"max_ticks", Json{{"type", "integer"}, {"minimum", 0}}},
        {"children", Json{{"type", "array"}, {"items", Json{{"type", "object"}}}}}};

    Json recipeOperation = Json::object();
    recipeOperation["type"] = "object";
    recipeOperation["additionalProperties"] = false;
    recipeOperation["required"] = Json::array({"operation", "id", "plan_id", "duration_minutes", "interruptible",
                                                 "interrupt_salience_threshold", "reservation_mode", "priority",
                                                 "preemption_policy", "parameters", "available_to", "root"});
    recipeOperation["properties"] = Json{
        {"operation", Json{{"const", "register_behavior_recipe"}}},
        {"id", Json{{"type", "string"}, {"minLength", 1}}},
        {"plan_id", Json{{"type", "string"}, {"minLength", 1}}},
        {"duration_minutes", Json{{"type", "integer"}, {"minimum", 0}}},
        {"interruptible", Json{{"type", "boolean"}}},
        {"interrupt_salience_threshold", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"reservation_mode", Json{{"enum", Json::array({"none", "input_resources"})}}},
        {"priority", Json{{"type", "integer"}, {"minimum", 0}}},
        {"preemption_policy", Json{{"enum", Json::array({"none", "lower_priority"})}}},
        {"parameters", Json{{"type", "object"}}},
        {"available_to", Json{{"type", "array"}, {"minItems", 1}, {"uniqueItems", true},
                                {"items", Json{{"type", "string"}, {"minLength", 1}}}}},
        {"root", node}};

    Json operationItems = Json::object();
    operationItems["oneOf"] = evolutionLevel == "level1"
                                    ? Json::array({strategyOperation, scheduleOperation, recipeOperation})
                                    : Json::array({createOperation, recipeOperation});
    responseSchema["properties"]["operations"] = Json::object();
    responseSchema["properties"]["operations"]["type"] = "array";
    responseSchema["properties"]["operations"]["minItems"] = 1;
    responseSchema["properties"]["operations"]["maxItems"] = 1;
    responseSchema["properties"]["operations"]["items"] = std::move(operationItems);
    responseSchema["properties"]["rationale"] = Json{{"type", "string"}};
    Json request = Json::object();
    request["model"] = config_.model;
    request["temperature"] = config_.temperature;
    request["max_tokens"] = policy.maxTokens;
    request["messages"] = Json::array({
        Json{{"role", "system"}, {"content", systemPrompt}},
        Json{{"role", "user"}, {"content", Json{{"evolution_level", evolutionLevel},
                                                     {"world", context},
                                                     {"validation_feedback", validationFeedback}}.dump()}}});
    // Use a single forced tool call for proposals.  This follows the same
    // provider boundary as BieTheirBed: the model emits typed arguments, and
    // Runtime validates those arguments before treating them as data.  The
    // JSON-content parser remains as an interoperability path for older
    // llama.cpp builds that ignore tools but honor structured output.
    const std::string toolName = "propose_evolution_" + evolutionLevel;
    request["tools"] = Json::array({Json{{"type", "function"},
                                          {"function", Json{{"name", toolName},
                                                             {"description", "Propose one bounded, declarative world evolution change for this exact level."},
                                                             {"parameters", responseSchema}, {"strict", true}}}}});
    // llama.cpp accepts the OpenAI-compatible string form here.  There is
    // exactly one tool in this request, so `required` is equivalent to naming
    // that function and avoids a provider-specific object warning.
    request["tool_choice"] = "required";
    auto configureDecoding = [](Json& target, const DecisionPolicy& selectedPolicy) {
        target["chat_template_kwargs"] = Json{{"enable_thinking", selectedPolicy.enableThinking}};
        if (selectedPolicy.enableThinking) {
            target["reasoning_budget_tokens"] = selectedPolicy.reasoningBudgetTokens;
        } else {
            target["reasoning_effort"] = "none";
        }
    };
    configureDecoding(request, policy);
    auto completed = requestProviderJson(config_, request, "evolution_" + evolutionLevel);
    if (!completed.ok) return ProviderResponse{false, {}, {}, completed.error, Json::object()};
    if (!completed.value.is_object()) {
        return ProviderResponse{false, {}, {}, "invalid_evolution_proposal", Json::object()};
    }
    return ProviderResponse{true, {}, completed.value.value("rationale", ""), {}, completed.value};
}


}  // namespace tide_marks
