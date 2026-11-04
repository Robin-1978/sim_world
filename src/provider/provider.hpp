#pragma once

#include "core/types.hpp"
#include "npc/conversation.hpp"
#include <memory>

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
    std::string internalDrives;
    std::string externalStimuli;
    std::string structuralConstraints;
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
};

struct ProviderResponse {
    bool ok{false};
    IntentProposal proposal;
    std::string dialogue;
    std::string error;
};

// A decision policy is content/runtime configuration, not a model name
// special case.  The same provider can use cheap output for a player turn and
// a bounded reasoning pass for an autonomous reappraisal.
struct DecisionPolicy {
    bool enableThinking{false};
    int reasoningBudgetTokens{0};
    int maxTokens{512};
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
    int timeoutMs{120000};
    double temperature{0.2};
    DecisionPolicy fallbackPolicy;
    std::map<std::string, DecisionPolicy> decisionPolicies;
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
