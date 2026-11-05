#include "provider/provider_profile.hpp"

#include <fstream>
#include <filesystem>
#include <cstdlib>
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

ProviderEndpoint readPurposeEndpoint(const Json& value, const std::string& purpose,
                                     const std::string& path) {
    if (!value.is_object()) {
        throw std::runtime_error("provider.purpose_endpoints." + purpose + " must be an object: " + path);
    }
    ProviderEndpoint result;
    if (value.contains("base_url")) {
        if (!value.at("base_url").is_string() || value.at("base_url").get<std::string>().empty()) {
            throw std::runtime_error("provider.purpose_endpoints." + purpose +
                                     ".base_url must be a non-empty string: " + path);
        }
        result.baseUrl = value.at("base_url").get<std::string>();
    }
    if (value.contains("model")) {
        if (!value.at("model").is_string() || value.at("model").get<std::string>().empty()) {
            throw std::runtime_error("provider.purpose_endpoints." + purpose +
                                     ".model must be a non-empty string: " + path);
        }
        result.model = value.at("model").get<std::string>();
    }
    if (value.contains("api_key_env")) {
        result.apiKeyConfigured = true;
        if (!value.at("api_key_env").is_null()) {
            if (!value.at("api_key_env").is_string()) {
                throw std::runtime_error("provider.purpose_endpoints." + purpose +
                                         ".api_key_env must be a string or null: " + path);
            }
            result.apiKeyEnv = value.at("api_key_env").get<std::string>();
        }
    }
    if (value.contains("timeout_ms")) {
        if (!value.at("timeout_ms").is_number_integer() || value.at("timeout_ms").get<int>() <= 0) {
            throw std::runtime_error("provider.purpose_endpoints." + purpose +
                                     ".timeout_ms must be positive: " + path);
        }
        result.timeoutMs = value.at("timeout_ms").get<int>();
    }
    if (value.contains("temperature")) {
        if (!value.at("temperature").is_number() || value.at("temperature").get<double>() < 0.0 ||
            value.at("temperature").get<double>() > 2.0) {
            throw std::runtime_error("provider.purpose_endpoints." + purpose +
                                     ".temperature must be between 0 and 2: " + path);
        }
        result.temperature = value.at("temperature").get<double>();
    }
    return result;
}

}  // namespace

std::string defaultIntelligenceProfilePath() {
    const auto userProfile = std::filesystem::path(defaultRuntimeDirectory()) /
                             "config" / "intelligence-profile.json";
    if (std::filesystem::exists(userProfile)) return userProfile.string();
#ifdef TIDE_MARKS_DATA_DIR
    return std::string(TIDE_MARKS_DATA_DIR) + "/intelligence-profile.json";
#else
    return "data/intelligence-profile.json";
#endif
}

std::string defaultRuntimeDirectory() {
    if (const char* configured = std::getenv("SIM_WORLD_HOME"); configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return (std::filesystem::path(home) / ".sim_world").string();
    }
    return ".sim_world";
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
    if (provider.contains("purpose_endpoints")) {
        if (!provider.at("purpose_endpoints").is_object()) {
            throw std::runtime_error("provider.purpose_endpoints must be an object: " + path);
        }
        for (const auto& [purpose, value] : provider.at("purpose_endpoints").items()) {
            if (purpose != "decision" && purpose != "level1_evolution" && purpose != "level2_evolution") {
                throw std::runtime_error("unsupported provider purpose endpoint '" + purpose + "': " + path);
            }
            result.llama.purposeEndpoints.emplace(purpose, readPurposeEndpoint(value, purpose, path));
        }
    }
    if (!root.contains("decision_policy") || !root.at("decision_policy").is_object() ||
        root.at("decision_policy").empty()) {
        throw std::runtime_error("intelligence profile requires a non-empty decision_policy: " + path);
    }
    for (const auto& [key, value] : root.at("decision_policy").items()) {
        (void)value;
        result.llama.decisionPolicies.emplace(key, readPolicy(root, key, path));
    }
    // Preserve the first policy as a safe fallback for callers that construct a
    // context before world content is attached. Dispatch still uses the
    // provider_policy selected by the content channel.
    result.llama.fallbackPolicy = result.llama.decisionPolicies.begin()->second;
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

std::unique_ptr<IProvider> makeDefaultRuntimeProvider() {
    const auto profile = loadIntelligenceProfile(defaultIntelligenceProfilePath());
    return makeProviderFromProfile(profile);
}

}  // namespace tide_marks
