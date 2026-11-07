#pragma once

#include "provider/provider.hpp"

#include <string>

namespace tide_marks {

// The provider adapter is deliberately a small transport boundary. This
// engine is the shared orchestration boundary used by synchronous calls and
// asynchronous queues. It catches adapter failures, labels the stage, and
// attaches a bounded quality report without granting the model any Runtime
// authority.
class LlmEngine {
public:
    explicit LlmEngine(IProvider& provider) : provider_(provider) {}

    ProviderResponse decide(const DecisionContext& context,
                            const std::string& playerInput,
                            const Json& validationFeedback = Json::object()) const;
    ProviderResponse proposeEvolution(const Json& context,
                                      const std::string& evolutionLevel,
                                      const Json& validationFeedback = Json::object()) const;
    ProviderResponse proposeReflection(const Json& context,
                                       const std::string& targetNpcId,
                                       const Json& validationFeedback = Json::object()) const;

    [[nodiscard]] std::string name() const { return provider_.name(); }
    [[nodiscard]] IProvider& provider() const { return provider_; }
    [[nodiscard]] ProviderCapabilities capabilities() const { return provider_.capabilities(); }
    [[nodiscard]] Json runtimeMetadata(const std::string& purpose) const {
        return provider_.runtimeMetadata(purpose);
    }

    // These are public so a probe or a Runtime validator can assess a result
    // produced by a custom adapter without making another model call.
    [[nodiscard]] static Json assessDecision(const DecisionContext& context,
                                             const ProviderResponse& response,
                                             int repairAttempt = 0);
    [[nodiscard]] static Json assessReflection(const Json& context,
                                                const ProviderResponse& response,
                                                int repairAttempt = 0);
    [[nodiscard]] static Json assessEvolution(const Json& context,
                                              const std::string& evolutionLevel,
                                              const ProviderResponse& response,
                                              int repairAttempt = 0);

private:
    IProvider& provider_;

    static void attach(Json& responseQuality, ProviderResponse& response,
                       const Json& report);
    void attachSemanticReview(const std::string& stage, const Json& context,
                              const Json& candidate, Json& report) const;
};

}  // namespace tide_marks
