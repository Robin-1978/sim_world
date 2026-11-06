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

DecisionPolicy readPolicyValue(const Json& value, const std::string& key, const std::string& path) {
    if (!value.is_object()) {
        throw std::runtime_error("policy " + key + " must be an object: " + path);
    }
    if (!value.contains("thinking") || !value.at("thinking").is_boolean()) {
        throw std::runtime_error("policy." + key + ".thinking must be boolean: " + path);
    }
    if (!value.contains("max_tokens") || !value.at("max_tokens").is_number_integer() ||
        value.at("max_tokens").get<int>() <= 0) {
        throw std::runtime_error("policy." + key + ".max_tokens must be positive: " + path);
    }
    DecisionPolicy result;
    result.enableThinking = value.at("thinking").get<bool>();
    result.maxTokens = value.at("max_tokens").get<int>();
    if (value.contains("reasoning_budget_tokens")) {
        if (!value.at("reasoning_budget_tokens").is_number_integer() ||
            value.at("reasoning_budget_tokens").get<int>() < 0) {
            throw std::runtime_error("policy." + key +
                                     ".reasoning_budget_tokens must be non-negative: " + path);
        }
        result.reasoningBudgetTokens = value.at("reasoning_budget_tokens").get<int>();
    }
    if (result.enableThinking && result.reasoningBudgetTokens <= 0) {
        throw std::runtime_error("policy." + key +
                                 " enables thinking but has no reasoning_budget_tokens: " + path);
    }
    if (result.enableThinking && result.maxTokens <= result.reasoningBudgetTokens) {
        throw std::runtime_error("policy." + key +
                                 ".max_tokens must exceed reasoning_budget_tokens: " + path);
    }
    return result;
}

std::map<std::string, DecisionPolicy> readPurposePolicies(const Json& value,
                                                          const std::string& purpose,
                                                          const std::string& path) {
    const char* key = purpose == "decision" ? "policies" : "policy";
    if (!value.contains(key) || !value.at(key).is_object() || value.at(key).empty()) {
        throw std::runtime_error("purposes." + purpose + " requires non-empty " + key + ": " + path);
    }
    std::map<std::string, DecisionPolicy> result;
    if (purpose == "decision") {
        for (const auto& [policyId, policy] : value.at(key).items()) {
            if (policyId.empty()) {
                throw std::runtime_error("purposes.decision.policies keys must not be empty: " + path);
            }
            result.emplace(policyId, readPolicyValue(policy, policyId, path));
        }
        for (const auto requiredPolicy : {"utterance_understanding", "player_dialogue", "autonomous_reappraisal"}) {
            if (!result.contains(requiredPolicy)) {
                throw std::runtime_error("purposes.decision.policies requires " +
                                         std::string(requiredPolicy) + ": " + path);
            }
        }
    } else {
        result.emplace("proposal", readPolicyValue(value.at(key), "proposal", path));
    }
    return result;
}

PurposeProvider readPurposeProvider(const Json& value, const std::string& purpose,
                                    const std::string& path) {
    if (!value.is_object()) {
        throw std::runtime_error("purposes." + purpose + " must be an object: " + path);
    }
    if (!value.contains("provider") || !value.at("provider").is_string() ||
        value.at("provider").get<std::string>().empty()) {
        throw std::runtime_error("purposes." + purpose + ".provider must be a non-empty string: " + path);
    }
    PurposeProvider result;
    result.providerId = value.at("provider").get<std::string>();
    result.policies = readPurposePolicies(value, purpose, path);
    if (value.contains("base_url")) {
        if (!value.at("base_url").is_string() || value.at("base_url").get<std::string>().empty()) {
            throw std::runtime_error("purposes." + purpose +
                                     ".base_url must be a non-empty string: " + path);
        }
        result.baseUrl = value.at("base_url").get<std::string>();
    }
    if (value.contains("model")) {
        if (!value.at("model").is_string() || value.at("model").get<std::string>().empty()) {
            throw std::runtime_error("purposes." + purpose +
                                     ".model must be a non-empty string: " + path);
        }
        result.model = value.at("model").get<std::string>();
    }
    if (value.contains("api_key_env")) {
        result.apiKeyConfigured = true;
        if (!value.at("api_key_env").is_null()) {
            if (!value.at("api_key_env").is_string()) {
                throw std::runtime_error("purposes." + purpose +
                                         ".api_key_env must be a string or null: " + path);
            }
            result.apiKeyEnv = value.at("api_key_env").get<std::string>();
            if (result.apiKeyEnv.empty()) {
                throw std::runtime_error("purposes." + purpose +
                                         ".api_key_env must not be empty: " + path);
            }
        } else {
            result.apiKeyEnv.clear();
        }
    }
    if (value.contains("timeout_ms")) {
        if (!value.at("timeout_ms").is_number_integer() || value.at("timeout_ms").get<int>() <= 0) {
            throw std::runtime_error("purposes." + purpose +
                                     ".timeout_ms must be positive: " + path);
        }
        result.timeoutMs = value.at("timeout_ms").get<int>();
    }
    if (value.contains("temperature")) {
        if (!value.at("temperature").is_number() || value.at("temperature").get<double>() < 0.0 ||
            value.at("temperature").get<double>() > 2.0) {
            throw std::runtime_error("purposes." + purpose +
                                     ".temperature must be between 0 and 2: " + path);
        }
        result.temperature = value.at("temperature").get<double>();
    }
    return result;
}

ProviderDefinition readProviderDefinition(const Json& value, const std::string& id,
                                          const std::string& path) {
    if (!value.is_object()) {
        throw std::runtime_error("providers." + id + " must be an object: " + path);
    }
    ProviderDefinition result;
    result.type = value.value("type", "");
    if (result.type != "mock" && result.type != "llama_cpp" &&
        result.type != "openai_compatible") {
        throw std::runtime_error("providers." + id + ".type is unsupported: " + path);
    }
    result.baseUrl = value.value("base_url", "");
    result.model = value.value("model", "");
    if (value.contains("api_key_env") && !value.at("api_key_env").is_null()) {
        if (!value.at("api_key_env").is_string()) {
            throw std::runtime_error("providers." + id + ".api_key_env must be a string or null: " + path);
        }
        result.apiKeyEnv = value.at("api_key_env").get<std::string>();
        if (result.apiKeyEnv.empty()) {
            throw std::runtime_error("providers." + id + ".api_key_env must not be empty: " + path);
        }
    }
    if (value.contains("headers")) {
        if (!value.at("headers").is_object()) {
            throw std::runtime_error("providers." + id + ".headers must be an object: " + path);
        }
        for (auto item = value.at("headers").begin(); item != value.at("headers").end(); ++item) {
            if (item.key().empty() || !item.value().is_string() || item.value().get<std::string>().empty() ||
                item.key() == "Authorization" || item.key() == "authorization") {
                throw std::runtime_error("providers." + id + ".headers contains an invalid or credential header: " + path);
            }
            result.headers.emplace(item.key(), item.value().get<std::string>());
        }
    }
    result.timeoutMs = value.value("timeout_ms", 120000);
    result.temperature = value.value("temperature", 0.2);
    if (result.timeoutMs <= 0) {
        throw std::runtime_error("providers." + id + ".timeout_ms must be positive: " + path);
    }
    if (result.temperature < 0.0 || result.temperature > 2.0) {
        throw std::runtime_error("providers." + id + ".temperature must be between 0 and 2: " + path);
    }
    if (result.type != "mock" && (result.model.empty() || result.baseUrl.empty())) {
        throw std::runtime_error("non-mock provider requires model and base_url: " + path);
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
    if (root.value("kind", "") != "intelligence_profile" || root.value("schema_version", 0) != 2) {
        throw std::runtime_error("unsupported intelligence profile schema: " + path);
    }
    for (const auto legacyKey : {"provider", "purpose_endpoints", "decision_policy"}) {
        if (root.contains(legacyKey)) {
            throw std::runtime_error("legacy intelligence profile field '" +
                                     std::string(legacyKey) + "' is not supported: " + path);
        }
    }
    IntelligenceProfile result;
    result.id = root.value("id", "");
    if (result.id.empty()) throw std::runtime_error("intelligence profile requires a non-empty id: " + path);
    if (!root.contains("providers") || !root.at("providers").is_object() || root.at("providers").empty()) {
        throw std::runtime_error("intelligence profile requires non-empty providers: " + path);
    }
    for (const auto& [id, value] : root.at("providers").items()) {
        if (id.empty()) throw std::runtime_error("provider id must not be empty: " + path);
        if (!result.providers.emplace(id, readProviderDefinition(value, id, path)).second) {
            throw std::runtime_error("duplicate provider id: " + id);
        }
    }
    if (!root.contains("purposes") || !root.at("purposes").is_object() || root.at("purposes").empty()) {
        throw std::runtime_error("intelligence profile requires non-empty purposes: " + path);
    }
    for (const auto& [purpose, value] : root.at("purposes").items()) {
        if (purpose != "decision" && purpose != "npc_evolution" && purpose != "reflection" &&
            purpose != "world_evolution") {
            throw std::runtime_error("unsupported purpose '" + purpose + "': " + path);
        }
        auto binding = readPurposeProvider(value, purpose, path);
        if (!result.providers.contains(binding.providerId)) {
            throw std::runtime_error("purpose " + purpose + " references unknown provider " +
                                     binding.providerId + ": " + path);
        }
        result.purposes.emplace(purpose, std::move(binding));
    }
    for (const auto requiredPurpose : {"decision", "npc_evolution", "world_evolution"}) {
        if (!result.purposes.contains(requiredPurpose)) {
            throw std::runtime_error("intelligence profile requires purposes." +
                                     std::string(requiredPurpose) + ": " + path);
        }
    }
    return result;
}

std::unique_ptr<IProvider> makeProviderFromProfile(const IntelligenceProfile& profile,
                                                    const std::string& providerOverride) {
    const auto decisionPurpose = profile.purposes.find("decision");
    if (decisionPurpose == profile.purposes.end()) {
        throw std::runtime_error("intelligence profile has no decision purpose");
    }
    const auto decisionProvider = profile.providers.find(decisionPurpose->second.providerId);
    if (decisionProvider == profile.providers.end()) {
        throw std::runtime_error("decision purpose references an unknown provider");
    }
    const std::string type = providerOverride.empty() || providerOverride == "profile"
                                 ? decisionProvider->second.type
                                 : providerOverride;
    if (type == "mock") return std::make_unique<MockProvider>();
    if (type == "llama" || type == "llama_cpp" || type == "openai_compatible") {
        if (decisionProvider->second.model.empty() || decisionProvider->second.baseUrl.empty()) {
            throw std::runtime_error("llama provider requires model and base_url in the intelligence profile");
        }
        LlamaCppConfig config;
        config.profileId = profile.id;
        config.baseUrl = decisionProvider->second.baseUrl;
        config.model = decisionProvider->second.model;
        config.apiKeyEnv = decisionProvider->second.apiKeyEnv;
        config.headers = decisionProvider->second.headers;
        config.timeoutMs = decisionProvider->second.timeoutMs;
        config.temperature = decisionProvider->second.temperature;
        config.providers = profile.providers;
        config.purposes = profile.purposes;
        config.decisionPolicies = decisionPurpose->second.policies;
        config.purposePolicies.emplace("npc_evolution",
                                      profile.purposes.at("npc_evolution").policies.at("proposal"));
    const auto reflectionPurpose = profile.purposes.find("reflection");
    // Reflection is a first-class structured purpose. Older local profiles
    // may omit it; inherit the explicitly configured world-evolution budget
    // rather than the provider's 512-token construction default, which can
    // silently truncate a daily reflection tool call.
    const auto worldPurpose = profile.purposes.find("world_evolution");
    const auto& reflectionPolicy = reflectionPurpose != profile.purposes.end()
                                       ? reflectionPurpose->second.policies.at("proposal")
                                       : worldPurpose != profile.purposes.end()
                                             ? worldPurpose->second.policies.at("proposal")
                                             : config.fallbackPolicy;
        config.purposePolicies.emplace("reflection", reflectionPolicy);
        config.purposePolicies.emplace("world_evolution",
                                      profile.purposes.at("world_evolution").policies.at("proposal"));
        config.fallbackPolicy = config.decisionPolicies.begin()->second;
        return std::make_unique<LlamaCppProvider>(std::move(config));
    }
    throw std::runtime_error("unsupported provider override: " + type);
}

std::unique_ptr<IProvider> makeDefaultRuntimeProvider() {
    const auto profile = loadIntelligenceProfile(defaultIntelligenceProfilePath());
    return makeProviderFromProfile(profile);
}

}  // namespace tide_marks
