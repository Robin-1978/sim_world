#pragma once

#include "core/types.hpp"
#include "npc/conversation.hpp"
#include <memory>
#include <mutex>

namespace tide_marks {
struct ActionCandidate {
    std::string actionId;
    std::string planId;
    std::string label;
    // A candidate may start a long-running, data-authored process.  Zero means
    // that the candidate is an instantaneous interaction.
    int durationMinutes{0};
    bool interruptible{true};
    double interruptSalienceThreshold{1.0};
    std::string reservationMode{"none"};
    std::string recipeId;
    std::map<std::string, int> reservations;
    int priority{0};
    std::string preemptionPolicy{"none"};
    double utilityScore{0.0};
    Json utilityContributions = Json::object();
    // Optional, content-authored input contract for a behavior plan.  The
    // model may fill these parameters, but Runtime validates them before a
    // recipe can resolve a dynamic target such as a destination.
    Json parameterSchema = Json::object();
    // Author-declared, bounded utility terms. Runtime never invents story
    // weights; it only combines these terms with deterministic drive relief.
    double resourceCost{0.0};
    double risk{0.0};
    double relationshipEffect{0.0};
    double futureValue{0.0};
    double constraintPenalty{0.0};
    // Content-authored coefficients for private trait preferences. Runtime
    // multiplies each coefficient by a bounded nonlinear centered response.
    std::map<std::string, double> traitEffects;
    // Deterministic projection state at the captured decision revision. The
    // provider only receives true entries; Runtime keeps false entries in the
    // request context so a stale or adversarial response still fails closed
    // with the original validation reason.
    bool availableNow{true};
    std::string unavailableReason;
};

struct StructuralConstraintView {
    std::string id;
    std::string category;
    std::string scope;
    std::string source;
    std::string reason;
    bool hard{true};
};

struct SpeechActPermissionView {
    std::string id;
    std::string description;
    bool worldActionAllowed{false};
};

struct DriveView {
    std::string id;
    std::string domain;
    std::string dimension;
    double deficit{0.0};
    double drive{0.0};
    double sensitivity{1.0};
    double evidenceConfidence{1.0};
    bool critical{false};
    std::map<std::string, double> expectedRelief;
};

struct DecisionContext {
    std::string npcId;
    std::string npcName;
    std::string channel;
    std::string providerPolicy;
    // Trigger describes why the channel was awakened (for example a salient
    // event); it is not used to select provider behavior.
    std::string trigger;
    std::string speechAct;
    bool worldActionAllowed{true};
    bool speechActClassificationRequired{false};
    // Versions are carried with an asynchronous request. They describe the
    // content and prompt contract used to produce the proposal, not mutable
    // world facts.
    std::string contentVersion;
    std::string promptVersion;
    GameTime now;
    std::string perception;
    std::string memories;
    std::string knowledge;
    std::string experiences;
    std::string character;
    // Private motives are available for internal deliberation but are not
    // part of the public character description. Providers must not copy them
    // into dialogue unless an authored disclosure rule permits it.
    std::string privateCharacter;
    Json privateTraits = Json::object();
    std::string internalDrives;
    std::string externalStimuli;
    std::string structuralConstraints;
    // The provider-facing projection is deliberately split into bounded,
    // typed sections.  These are not a second WorldState: they are a
    // visibility-filtered decision snapshot assembled for one NPC and one
    // trigger.  The Runtime always includes time/revision metadata, while
    // world facts are limited to what the actor can observe or already knows.
    Json worldSnapshot = Json::object();
    // Runtime identity and version metadata is an audit envelope, not a
    // world fact. Providers may echo it in diagnostics, but prompts must not
    // use it to infer locations, resources, motives, or actions.
    Json runtimeMetadata = Json::object();
    Json selfState = Json::object();
    Json planCatalog = Json::array();
    Json currentActivity = Json{{"status", "idle"}};
    std::vector<SpeechActPermissionView> speechActs;
    std::vector<ExternalStimulus> stimuli;
    std::vector<StructuralConstraintView> constraints;
    std::vector<DriveView> drives;
    std::vector<ActionCandidate> candidates;
    std::uint64_t worldRevision{0};
    std::uint64_t npcStateVersion{0};
};

struct IntentProposal {
    std::string actionId;
    std::string intent;
    std::string rationale;
    std::string expectedOutcome;
    double confidence{0.0};
    std::uint64_t worldRevision{0};
    std::uint64_t npcStateVersion{0};
    std::vector<std::string> citedStimulusIds;
    std::vector<std::string> tradeoffs;
    std::map<std::string, double> expectedRelief;
    std::string speechAct;
    Json utteranceUnderstanding = Json::object();
    bool worldActionRequested{false};
    Json parameters = Json::object();
};

struct ProviderResponse {
    bool ok{false};
    IntentProposal proposal;
    std::string dialogue;
    std::string error;
    // Level 1/Level 2 evolution uses the same provider boundary as NPC
    // intent. The Runtime parses and governs this JSON; the provider never
    // mutates WorldState or approves its own proposal.
    Json evolutionProposal = Json::object();
    // Daily reflection is a separate structured cognition result. Runtime
    // routes ordinary updates locally and only forwards embedded innovation
    // hypotheses to the governed evolution lane.
    Json reflectionResult = Json::object();
};

// Runtime decisions are based on adapter capabilities, never on a display
// name or a concrete model/backend string.  A capability only describes what
// the adapter can do at its boundary; it does not grant authority to mutate
// WorldState.
struct ProviderCapabilities {
    bool structuredRepair{false};
    bool automaticEvolution{false};
    bool promptPrefixCaching{false};
};

// A decision policy is content/runtime configuration, not a model name
// special case.  The same provider can use cheap output for a player turn and
// a bounded reasoning pass for an autonomous reappraisal.
struct DecisionPolicy {
    bool enableThinking{false};
    int reasoningBudgetTokens{0};
    // OpenAI-compatible reasoning models use a named effort level rather
    // than llama.cpp's token budget. Keep this optional so local profiles can
    // remain token-budget driven.
    std::string reasoningEffort;
    int maxTokens{512};
};

class IProvider {
public:
    virtual ~IProvider() = default;
    virtual ProviderResponse decide(const DecisionContext& context,
                                    const std::string& playerInput) = 0;
    // Runtime validation may return one bounded, structured repair hint. The
    // default adapter preserves existing providers and tests that only
    // implement the one-shot decision contract.
    virtual ProviderResponse decide(const DecisionContext& context,
                                    const std::string& playerInput,
                                    const Json& validationFeedback) {
        (void)validationFeedback;
        return decide(context, playerInput);
    }
    // Evolution is explicit and infrequent, so it has a separate structured
    // request. A provider that does not implement an LLM evolution path must
    // fail closed instead of silently inventing a deterministic proposal.
    virtual ProviderResponse proposeEvolution(const Json& context,
                                              const std::string& evolutionLevel) {
        (void)context;
        (void)evolutionLevel;
        return ProviderResponse{false, {}, {}, "evolution_provider_unavailable", Json::object()};
    }
    // A validation result may be returned to the same provider request as a
    // bounded repair hint. Providers that do not support iterative repair
    // retain the one-shot behavior above; they never receive authority to
    // bypass Runtime validation.
    virtual ProviderResponse proposeEvolution(const Json& context,
                                              const std::string& evolutionLevel,
                                              const Json& validationFeedback) {
        (void)validationFeedback;
        return proposeEvolution(context, evolutionLevel);
    }
    virtual ProviderResponse proposeReflection(const Json& context,
                                               const std::string& targetNpcId) {
        (void)context;
        (void)targetNpcId;
        return ProviderResponse{false, {}, {}, "reflection_provider_unavailable", Json::object(), Json::object()};
    }
    virtual ProviderResponse proposeReflection(const Json& context,
                                               const std::string& targetNpcId,
                                               const Json& validationFeedback) {
        (void)validationFeedback;
        return proposeReflection(context, targetNpcId);
    }
    // Returns non-secret execution identity for audit envelopes.  It never
    // includes API key values or environment variable contents.
    [[nodiscard]] virtual Json runtimeMetadata(const std::string& purpose) const {
        return Json{{"provider_id", name()}, {"provider_type", name()}, {"purpose", purpose}};
    }
    // Adapters opt into bounded Runtime validation repair and automatic
    // evolution explicitly. Runtime must not infer capabilities from a
    // provider display name.
    [[nodiscard]] virtual ProviderCapabilities capabilities() const { return {}; }
    [[nodiscard]] virtual std::string name() const = 0;
};

class MockProvider final : public IProvider {
public:
    ProviderResponse decide(const DecisionContext& context,
                            const std::string& playerInput) override;
    [[nodiscard]] std::string name() const override { return "mock"; }
};

// A reusable Provider definition. API keys are never stored here; apiKeyEnv is
// only the name of an environment variable read at request time by the HTTP
// adapter.
struct ProviderDefinition {
    std::string type{"llama_cpp"};
    std::string baseUrl;
    std::string model;
    std::string apiKeyEnv;
    // Non-secret transport headers such as a provider session or tenant. API
    // credentials remain environment-backed and are never accepted here.
    std::map<std::string, std::string> headers;
    int timeoutMs{120000};
    double temperature{0.2};
};

// A purpose selects one reusable Provider and may override only the transport
// or sampling fields needed for that workload. An explicit null api_key_env
// disables the selected Provider's credential for this purpose.
struct PurposeProvider {
    std::string providerId;
    std::string baseUrl;
    std::string model;
    std::string apiKeyEnv;
    bool apiKeyConfigured{false};
    int timeoutMs{0};
    double temperature{-1.0};
    std::map<std::string, DecisionPolicy> policies;
};

struct LlamaCppConfig {
    std::string profileId;
    // The adapter supports both local llama.cpp and generic OpenAI-compatible
    // endpoints.  Keep provider-specific decoding hints out of the latter.
    std::string providerType{"llama_cpp"};
    std::string baseUrl{"http://127.0.0.1:8192/v1"};
    std::string model{"local"};
    std::string apiKeyEnv;
    std::map<std::string, std::string> headers;
    int timeoutMs{120000};
    double temperature{0.2};
    DecisionPolicy fallbackPolicy;
    std::map<std::string, DecisionPolicy> decisionPolicies;
    std::map<std::string, DecisionPolicy> purposePolicies;
    std::map<std::string, ProviderDefinition> providers;
    std::map<std::string, PurposeProvider> purposes;
};

class LlamaCppProvider final : public IProvider {
public:
    explicit LlamaCppProvider(LlamaCppConfig config = {});
    ProviderResponse decide(const DecisionContext& context,
                            const std::string& playerInput) override;
    ProviderResponse decide(const DecisionContext& context,
                            const std::string& playerInput,
                            const Json& validationFeedback) override;
    ProviderResponse proposeEvolution(const Json& context,
                                      const std::string& evolutionLevel) override;
    ProviderResponse proposeEvolution(const Json& context,
                                      const std::string& evolutionLevel,
                                      const Json& validationFeedback) override;
    ProviderResponse proposeReflection(const Json& context,
                                       const std::string& targetNpcId) override;
    ProviderResponse proposeReflection(const Json& context,
                                       const std::string& targetNpcId,
                                       const Json& validationFeedback) override;
    [[nodiscard]] Json runtimeMetadata(const std::string& purpose) const override;
    [[nodiscard]] ProviderCapabilities capabilities() const override {
        return ProviderCapabilities{true, true, true};
    }
    [[nodiscard]] std::string name() const override { return "llama.cpp"; }

private:
    LlamaCppConfig config_;
    mutable std::mutex callMutex_;
};


}  // namespace tide_marks
