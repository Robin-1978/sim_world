#include "provider/provider_profile.hpp"

#include <fstream>
#include <stdexcept>

namespace tide_marks {
namespace {

Json readObject(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open intelligence profile: " + path);
    Json value;
    input >> value;
    if (!value.is_object()) throw std::runtime_error("intelligence profile must be a JSON object: " + path);
    return value;
}

DecisionPolicy readPolicy(const Json& profile, const std::string& key, const std::string& path) {
    if (!profile.contains("decision_policy") || !profile.at("decision_policy").is_object()) {
        throw std::runtime_error("intelligence profile requires decision_policy: " + path);
    }
    const auto& policies = profile.at("decision_policy");
    if (!policies.contains(key) || !policies.at(key).is_object()) {
        throw std::runtime_error("intelligence profile requires decision_policy." + key + ": " + path);
    }
    const auto& value = policies.at(key);
    if (!value.contains("thinking") || !value.at("thinking").is_boolean()) {
        throw std::runtime_error("decision_policy." + key + ".thinking must be boolean: " + path);
    }
    if (!value.contains("max_tokens") || !value.at("max_tokens").is_number_integer() ||
        value.at("max_tokens").get<int>() <= 0) {
        throw std::runtime_error("decision_policy." + key + ".max_tokens must be positive: " + path);
    }
    DecisionPolicy result;
    result.enableThinking = value.at("thinking").get<bool>();
    result.maxTokens = value.at("max_tokens").get<int>();
    if (value.contains("reasoning_budget_tokens")) {
        if (!value.at("reasoning_budget_tokens").is_number_integer() ||
            value.at("reasoning_budget_tokens").get<int>() < 0) {
            throw std::runtime_error("decision_policy." + key +
                                     ".reasoning_budget_tokens must be non-negative: " + path);
        }
        result.reasoningBudgetTokens = value.at("reasoning_budget_tokens").get<int>();
    }
    if (result.enableThinking && result.reasoningBudgetTokens <= 0) {
        throw std::runtime_error("decision_policy." + key +
                                 " enables thinking but has no reasoning_budget_tokens: " + path);
    }
    if (result.enableThinking && result.maxTokens <= result.reasoningBudgetTokens) {
        throw std::runtime_error("decision_policy." + key +
                                 ".max_tokens must exceed reasoning_budget_tokens: " + path);
    }
    return result;
}

}  // namespace

std::string defaultIntelligenceProfilePath() {
#ifdef TIDE_MARKS_DATA_DIR
    return std::string(TIDE_MARKS_DATA_DIR) + "/intelligence-profile.json";
#else
    return "data/intelligence-profile.json";
#endif
}

IntelligenceProfile loadIntelligenceProfile(const std::string& path) {
    const Json root = readObject(path);
    if (root.value("kind", "") != "intelligence_profile" || root.value("schema_version", 0) != 1) {
        throw std::runtime_error("unsupported intelligence profile schema: " + path);
    }
    IntelligenceProfile result;
    result.id = root.value("id", "");
    if (result.id.empty()) throw std::runtime_error("intelligence profile requires a non-empty id: " + path);
    if (!root.contains("provider") || !root.at("provider").is_object()) {
        throw std::runtime_error("intelligence profile requires provider: " + path);
    }
    const auto& provider = root.at("provider");
    result.providerType = provider.value("type", "");
    if (result.providerType != "mock" && result.providerType != "llama_cpp" &&
        result.providerType != "openai_compatible") {
        throw std::runtime_error("unsupported intelligence provider type: " + path);
    }
    result.llama.model = provider.value("model", "");
    result.llama.baseUrl = provider.value("base_url", "");
    if (provider.contains("api_key_env") && !provider.at("api_key_env").is_null()) {
        if (!provider.at("api_key_env").is_string()) {
            throw std::runtime_error("provider.api_key_env must be a string or null: " + path);
        }
        result.llama.apiKeyEnv = provider.at("api_key_env").get<std::string>();
    }
    result.llama.timeoutMs = provider.value("timeout_ms", 120000);
    result.llama.temperature = provider.value("temperature", 0.2);
    if (result.llama.timeoutMs <= 0) {
        throw std::runtime_error("provider.timeout_ms must be positive: " + path);
    }
    if (result.llama.temperature < 0.0 || result.llama.temperature > 2.0) {
        throw std::runtime_error("provider.temperature must be between 0 and 2: " + path);
    }
    if (result.providerType != "mock" && (result.llama.model.empty() || result.llama.baseUrl.empty())) {
        throw std::runtime_error("non-mock provider requires model and base_url: " + path);
    }
    result.llama.playerDialogue = readPolicy(root, "player_dialogue", path);
    result.llama.autonomousReappraisal = readPolicy(root, "autonomous_reappraisal", path);
    return result;
}

std::unique_ptr<IProvider> makeProviderFromProfile(const IntelligenceProfile& profile,
                                                    const std::string& providerOverride) {
    const std::string type = providerOverride.empty() || providerOverride == "profile"
                                 ? profile.providerType
                                 : providerOverride;
    if (type == "mock") return std::make_unique<MockProvider>();
    if (type == "llama" || type == "llama_cpp" || type == "openai_compatible") {
        if (profile.llama.model.empty() || profile.llama.baseUrl.empty()) {
            throw std::runtime_error("llama provider requires model and base_url in the intelligence profile");
        }
        return std::make_unique<LlamaCppProvider>(profile.llama);
    }
    throw std::runtime_error("unsupported provider override: " + type);
}

}  // namespace tide_marks
