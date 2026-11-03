#include "provider/provider.hpp"
#include <algorithm>
#include <array>
#include <cctype>
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
bool containsKeyword(const DecisionContext& context, const std::string& actionId,
                     const std::string& loweredInput) {
    for (const auto& candidate : context.candidates) {
        if (candidate.actionId != actionId) continue;
        for (const auto& keyword : candidate.keywords) {
            std::string loweredKeyword = keyword;
            std::transform(loweredKeyword.begin(), loweredKeyword.end(), loweredKeyword.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!loweredKeyword.empty() && loweredInput.find(loweredKeyword) != std::string::npos) return true;
        }
    }
    return false;
}
}  // namespace
ProviderResponse MockProvider::decide(const DecisionContext& context, const std::string& playerInput) {
    if (context.candidates.empty()) {
        return ProviderResponse{false, {}, {}, "no_candidates"};
    }
    const auto speechAct = speechActFromString(context.speechAct);
    if (!context.worldActionAllowed) {
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
    const std::string lowered = [&playerInput] {
        std::string value = playerInput;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }();
    for (const auto& candidate : context.candidates) {
        if (containsKeyword(context, candidate.actionId, lowered)) {
            selected = candidate.actionId;
            break;
        }
    }
    const auto label = actionLabel(context, selected);
    const std::string name = context.npcName.empty() ? context.npcId : context.npcName;
    IntentProposal proposal;
    proposal.actionId = selected;
    proposal.intent = "respond";
    proposal.speechAct = context.speechAct.empty() ? "autonomous" : context.speechAct;
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
                                  {"interrupt_salience_threshold", candidate.interruptSalienceThreshold}});
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
    const bool useThinking = policy.enableThinking;
    const int effectiveThinkingBudget = policy.reasoningBudgetTokens;
    const Json userContent{{"npc_id", context.npcId}, {"time", context.now.toString()},
                           {"decision_channel", context.channel}, {"decision_trigger", context.trigger},
                           {"speech_act", context.speechAct}, {"world_action_allowed", context.worldActionAllowed},
                           {"perception", context.perception}, {"memories", context.memories},
                           {"character", context.character}, {"internal_drives", context.internalDrives},
                           {"drives", drives},
                           {"external_stimuli", context.externalStimuli},
                           {"stimuli", stimuli},
                           {"structural_constraints", context.structuralConstraints},
                           {"constraints", constraints},
                           {"player_input", playerInput},
                           {"candidates", candidates}};
    std::vector<std::string> stimulusIds;
    stimulusIds.reserve(context.stimuli.size());
    for (const auto& stimulus : context.stimuli) stimulusIds.push_back(stimulus.id);
    std::vector<std::string> needIds;
    needIds.reserve(context.drives.size());
    for (const auto& drive : context.drives) needIds.push_back(drive.id);
    const std::string systemPrompt =
        "Return one JSON object only. Choose exactly one action_id from the supplied candidates, or use an empty action_id for a conversation-only response. "
        "The current speech_act is " + context.speechAct + ". world_action_allowed is " +
        (context.worldActionAllowed ? std::string("true") : std::string("false")) + ". "
        "If world_action_allowed is false, action_id MUST be empty and the dialogue must answer the player without promising or performing a world action. "
        "cited_stimulus_ids may contain only IDs from the supplied stimuli array; the valid stimulus IDs for this request are " +
        Json(stimulusIds).dump() + ". If that list is empty, cited_stimulus_ids MUST be []. Never use a drive or need ID as a stimulus ID. "
        "expected_relief may contain only need IDs from the supplied drives array; the valid need IDs for this request are " +
        Json(needIds).dump() + ". Use empty arrays/objects when none are supplied. "
        "confidence and expected_relief values must be numbers from 0 to 1. dialogue must be one or two natural first-person sentences spoken by the NPC, never a dump of context fields. "
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
    request["chat_template_kwargs"] = Json{{"enable_thinking", useThinking}};
    if (useThinking) {
        // llama.cpp applies this sampler budget only through the top-level
        // reasoning_budget_tokens field.  A template-only thinking_budget is
        // ignored by current servers and can therefore consume max_tokens.
        request["reasoning_budget_tokens"] = effectiveThinkingBudget;
    } else {
        // Be explicit for servers whose model defaults enable reasoning.
        request["reasoning_effort"] = "none";
    }
    const auto temp = std::filesystem::temp_directory_path() /
                      ("tide_marks_llm_" + std::to_string(static_cast<long long>(std::rand())) + ".json");
    {
        std::ofstream output(temp);
        output << request.dump();
    }
    std::string command = "curl --silent --show-error --max-time " +
                          std::to_string(std::max(1, config_.timeoutMs / 1000)) +
                          " -H 'Content-Type: application/json'";
    if (!config_.apiKeyEnv.empty()) {
        if (const char* key = std::getenv(config_.apiKeyEnv.c_str()); key != nullptr && *key != '\0') {
            command += " -H " + shellQuote("Authorization: Bearer " + std::string(key));
        }
    }
    command += " --data-binary @" + shellQuote(temp.string()) + " " + shellQuote(config_.baseUrl + "/chat/completions");
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        std::filesystem::remove(temp);
        return ProviderResponse{false, {}, {}, "curl_unavailable"};
    }
    std::string response;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        response += buffer.data();
    }
    const int result = pclose(pipe);
    std::filesystem::remove(temp);
    if (result != 0 || response.empty()) {
        return ProviderResponse{false, {}, {}, "provider_request_failed"};
    }
    try {
        const Json outer = Json::parse(response);
        const auto& choice = outer.at("choices").at(0);
        if (const char* debug = std::getenv("TIDE_MARKS_DEBUG_PROVIDER");
            debug != nullptr && *debug != '\0' && std::string(debug) != "0") {
            std::cerr << "provider_debug finish_reason=" << choice.value("finish_reason", "")
                      << " trigger=" << context.trigger
                      << " thinking=" << useThinking
                      << " reasoning_budget=" << (useThinking ? effectiveThinkingBudget : 0)
                      << " max_tokens=" << policy.maxTokens
                      << " usage=" << outer.value("usage", Json::object()).dump()
                      << " content=" << choice.at("message").value("content", "")
                      << "\n";
        }
        if (choice.value("finish_reason", "") == "length") {
            return ProviderResponse{false, {}, {}, "provider_truncated"};
        }
        std::string content = choice.at("message").at("content").get<std::string>();
        const Json answer = Json::parse(content);
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
        proposal.speechAct = context.speechAct;
        proposal.worldActionRequested = !proposal.actionId.empty();
        return ProviderResponse{true, std::move(proposal), answer.value("dialogue", ""), {}};
    } catch (const std::exception& error) {
        return ProviderResponse{false, {}, {}, std::string("invalid_provider_json:") + error.what()};
    }
}


}  // namespace tide_marks
