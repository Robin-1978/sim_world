#include "provider/provider.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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
}  // namespace
ProviderResponse MockProvider::decide(const DecisionContext& context, const std::string& playerInput) {
    if (context.candidates.empty()) {
        return ProviderResponse{false, {}, {}, {}, {}, "no_candidates"};
    }
    std::string selected = context.candidates.front().actionId;
    const std::string lowered = [&playerInput] {
        std::string value = playerInput;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }();
    for (const auto& candidate : context.candidates) {
        if ((lowered.find("ration") != std::string::npos && candidate.actionId == "offer_ration") ||
            (lowered.find("ferry") != std::string::npos && candidate.actionId == "repair_ferry") ||
            (lowered.find("record") != std::string::npos && candidate.actionId == "share_record") ||
            (lowered.find("secret") != std::string::npos && candidate.actionId == "protect_secret")) {
            selected = candidate.actionId;
            break;
        }
    }
    const auto label = actionLabel(context, selected);
    std::string name = context.npcId;
    if (context.character.find("Mira") != std::string::npos) name = "Mira";
    if (context.character.find("Lorn") != std::string::npos) name = "Lorn";
    if (context.character.find("Eve") != std::string::npos) name = "Eve";
    return ProviderResponse{true, selected, "respond", "I need to " + label + " before I can promise more.",
                            name + " considers the request and chooses to " + label + ".", {}};
}

LlamaCppProvider::LlamaCppProvider(LlamaCppConfig config) : config_(std::move(config)) {}

ProviderResponse LlamaCppProvider::decide(const DecisionContext& context, const std::string& playerInput) {
    Json candidates = Json::array();
    for (const auto& candidate : context.candidates) {
        candidates.push_back(Json{{"action_id", candidate.actionId}, {"plan_id", candidate.planId},
                                  {"label", candidate.label}});
    }
    const Json userContent{{"npc_id", context.npcId}, {"time", context.now.toString()},
                           {"perception", context.perception}, {"memories", context.memories},
                           {"character", context.character}, {"player_input", playerInput},
                           {"candidates", candidates}};
    Json messages = Json::array();
    messages.push_back(Json{{"role", "system"},
                            {"content", "Return JSON with action_id, intent, motivation, dialogue. Choose only an action_id from the supplied candidates."}});
    messages.push_back(Json{{"role", "user"}, {"content", userContent.dump()}});
    Json request{{"model", config_.model}, {"temperature", 0.2}, {"messages", messages},
                 {"response_format", Json{{"type", "json_object"}}}};
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
        return ProviderResponse{false, {}, {}, {}, {}, "curl_unavailable"};
    }
    std::string response;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        response += buffer.data();
    }
    const int result = pclose(pipe);
    std::filesystem::remove(temp);
    if (result != 0 || response.empty()) {
        return ProviderResponse{false, {}, {}, {}, {}, "provider_request_failed"};
    }
    try {
        const Json outer = Json::parse(response);
        std::string content = outer.at("choices").at(0).at("message").at("content").get<std::string>();
        const Json answer = Json::parse(content);
        const std::string action = answer.value("action_id", "");
        const auto allowed = std::find_if(context.candidates.begin(), context.candidates.end(),
                                          [&action](const auto& candidate) { return candidate.actionId == action; });
        if (allowed == context.candidates.end()) {
            return ProviderResponse{false, {}, {}, {}, {}, "unknown_action_id"};
        }
        return ProviderResponse{true, action, answer.value("intent", "respond"),
                                answer.value("motivation", ""), answer.value("dialogue", ""), {}};
    } catch (const std::exception& error) {
        return ProviderResponse{false, {}, {}, {}, {}, std::string("invalid_provider_json:") + error.what()};
    }
}


}  // namespace tide_marks
