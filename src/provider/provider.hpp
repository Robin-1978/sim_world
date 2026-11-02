#pragma once

#include "core/types.hpp"
#include <memory>

namespace tide_marks {
struct ActionCandidate {
    std::string actionId;
    std::string planId;
    std::string label;
};

struct DecisionContext {
    std::string npcId;
    GameTime now;
    std::string perception;
    std::string memories;
    std::string character;
    std::vector<ActionCandidate> candidates;
    std::uint64_t worldRevision{0};
    std::uint64_t npcStateVersion{0};
};

struct ProviderResponse {
    bool ok{false};
    std::string actionId;
    std::string intent;
    std::string motivation;
    std::string dialogue;
    std::string error;
};

class IProvider {
public:
    virtual ~IProvider() = default;
    virtual ProviderResponse decide(const DecisionContext& context,
                                    const std::string& playerInput) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

class MockProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context,
                            const std::string& playerInput) override;
    [[nodiscard]] std::string name() const override { return "mock"; }
};

struct LlamaCppConfig {
    std::string baseUrl{"http://127.0.0.1:8192/v1"};
    std::string model{"local"};
    std::string apiKeyEnv;
    int timeoutMs{15000};
};

class LlamaCppProvider final : public IProvider {
public:
    explicit LlamaCppProvider(LlamaCppConfig config = {});
    ProviderResponse decide(const DecisionContext& context,
                            const std::string& playerInput) override;
    [[nodiscard]] std::string name() const override { return "llama.cpp"; }

private:
    LlamaCppConfig config_;
};


}  // namespace tide_marks
