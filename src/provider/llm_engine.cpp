#include "provider/llm_engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <set>
#include <string>
#include <vector>

namespace tide_marks {
namespace {

struct QualityBuilder {
    std::string stage;
    int repairAttempt{0};
    Json checks = Json::object();
    std::vector<std::string> issues;

    QualityBuilder(std::string stageName, const int attempt)
        : stage(std::move(stageName)), repairAttempt(attempt) {}

    void check(const std::string& id, const bool passed, const Json& detail = Json::object()) {
        checks[id] = detail.is_object() && !detail.empty() ? detail : Json{{"passed", passed}};
        if (detail.is_object() && !detail.empty()) checks[id]["passed"] = passed;
        if (!passed) issues.push_back(id);
    }

    Json finish() const {
        const auto total = checks.size();
        const double score = total == 0 ? 0.0
                                       : static_cast<double>(total - issues.size()) /
                                             static_cast<double>(total);
        const std::string agent = stage == "decision"       ? "decision_agent"
                                  : stage == "reflection"   ? "reflection_agent"
                                  : stage == "level1_evolution" ? "level1_evolution_agent"
                                                                 : "level2_evolution_agent";
        Json inputChecks = Json::object();
        Json outputChecks = Json::object();
        std::vector<std::string> inputIssues;
        std::vector<std::string> outputIssues;
        for (const auto& item : checks.items()) {
            if (item.key().starts_with("input_")) inputChecks[item.key()] = item.value();
            else outputChecks[item.key()] = item.value();
        }
        for (const auto& issue : issues) {
            if (issue.starts_with("input_")) inputIssues.push_back(issue);
            else outputIssues.push_back(issue);
        }
        const auto inputPassed = inputIssues.empty();
        const auto outputPassed = outputIssues.empty();
        return Json{{"contract_version", 1}, {"agent", agent}, {"stage", stage},
                    {"repair_attempt", repairAttempt}, {"passed", inputPassed && outputPassed},
                    // `score` is retained as a compatibility field for
                    // existing audit readers, but it is explicitly the
                    // deterministic contract score. It is not a semantic
                    // quality score and must never be presented as one.
                    {"score", score}, {"contract_score", score}, {"checks", checks}, {"issues", issues},
                    {"input_quality", Json{{"passed", inputPassed}, {"issues", inputIssues},
                                             {"checks", inputChecks}}},
                    {"output_quality", Json{{"passed", outputPassed}, {"issues", outputIssues},
                                              {"checks", outputChecks}}},
                    {"semantic_quality", Json{{"status", "not_run"},
                                                {"review_agent", "quality_agent"},
                                                {"authoritative", false}}},
                    {"outcome_quality", Json{{"status", "not_measured"},
                                               {"authoritative", false}}},
                    {"runtime_validation", Json{{"status", "pending"},
                                                  {"authoritative", false}}},
                    {"final_disposition", inputPassed && outputPassed ? "candidate_ready" : "engine_rejected"}};
    }
};

bool isFiniteBounded(const Json& value, const double low, const double high) {
    if (!value.is_number()) return false;
    try {
        const auto number = value.get<double>();
        return std::isfinite(number) && number >= low && number <= high;
    } catch (...) {
        return false;
    }
}

bool hasString(const Json& object, const char* key, const std::size_t maxLength = 4096) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) return false;
    try {
        const auto& value = object.at(key).get_ref<const std::string&>();
        return !value.empty() && value.size() <= maxLength;
    } catch (...) {
        return false;
    }
}

bool hasArray(const Json& object, const char* key, const std::size_t maxItems) {
    return object.is_object() && object.contains(key) && object.at(key).is_array() &&
           object.at(key).size() <= maxItems;
}

int integerMember(const Json& object, const char* key, const int fallback = 0) {
    if (!object.is_object() || !object.contains(key)) return fallback;
    const auto& value = object.at(key);
    if (!value.is_number_integer()) return fallback;
    try {
        return value.get<int>();
    } catch (...) {
        return fallback;
    }
}

std::uint64_t unsignedMember(const Json& object, const char* key,
                             const std::uint64_t fallback = 0) {
    if (!object.is_object() || !object.contains(key)) return fallback;
    const auto& value = object.at(key);
    try {
        if (value.is_number_unsigned()) return value.get<std::uint64_t>();
        if (value.is_number_integer()) {
            const auto signedValue = value.get<std::int64_t>();
            return signedValue < 0 ? fallback : static_cast<std::uint64_t>(signedValue);
        }
    } catch (...) {
        return fallback;
    }
    return fallback;
}

std::string stringMember(const Json& object, const char* key, const std::string& fallback = {}) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) return fallback;
    try {
        return object.at(key).get<std::string>();
    } catch (...) {
        return fallback;
    }
}

Json memberOr(const Json& object, const char* key, const Json& fallback) {
    return object.is_object() && object.contains(key) ? object.at(key) : fallback;
}

int repairAttempt(const Json& feedback) {
    return std::max(0, integerMember(feedback, "attempt", 0));
}

Json decisionCandidate(const ProviderResponse& response) {
    const auto& proposal = response.proposal;
    return Json{{"action_id", proposal.actionId},
                {"intent", proposal.intent},
                {"rationale", proposal.rationale},
                {"expected_outcome", proposal.expectedOutcome},
                {"confidence", proposal.confidence},
                {"speech_act", proposal.speechAct},
                {"utterance_understanding", proposal.utteranceUnderstanding},
                {"world_action_requested", proposal.worldActionRequested},
                {"cited_stimulus_ids", proposal.citedStimulusIds},
                {"cited_evidence_refs", proposal.citedEvidenceRefs},
                {"expected_relief", proposal.expectedRelief},
                {"parameters", proposal.parameters},
                {"dialogue", response.dialogue}};
}

Json reviewRequest(const std::string& stage, const Json& context,
                   const Json& candidate, const Json& contract) {
    return Json{{"schema_version", 1}, {"stage", stage},
                {"context", context}, {"candidate", candidate},
                {"contract_validation", contract}};
}

void collectReviewEvidenceRefs(const Json& value, std::set<std::string>& refs, const int depth = 0) {
    if (depth > 10) return;
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            const auto& key = item.key();
            const bool refKey = key.find("ref") != std::string::npos || key == "sequence" ||
                                key.starts_with("memory_") || key.starts_with("knowledge_") ||
                                key.starts_with("experience_");
            if (refKey) {
                if (item.value().is_string()) refs.insert(item.value().get<std::string>());
                else if (item.value().is_number_integer()) refs.insert(item.value().dump());
                else if (item.value().is_array()) {
                    for (const auto& entry : item.value()) {
                        if (entry.is_string()) refs.insert(entry.get<std::string>());
                        else if (entry.is_number_integer()) refs.insert(entry.dump());
                    }
                } else if (item.value().is_object()) {
                    for (auto nested = item.value().begin(); nested != item.value().end(); ++nested) {
                        refs.insert(nested.key());
                        if (nested.value().is_string()) refs.insert(nested.value().get<std::string>());
                    }
                }
                // Continue walking because evidence can be nested below a
                // typed section such as function_context.evidence.
            }
            collectReviewEvidenceRefs(item.value(), refs, depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) collectReviewEvidenceRefs(item, refs, depth + 1);
    }
}

}  // namespace

void LlmEngine::attach(Json& responseQuality, ProviderResponse& response, const Json& report) {
    const Json adapterQuality = responseQuality;
    responseQuality = report;
    if (adapterQuality.is_object() && !adapterQuality.empty()) {
        // Adapters may add transport facts such as finish_reason or usage. Keep
        // them below a separate key so Runtime quality fields remain stable.
        responseQuality["adapter"] = adapterQuality;
    }
    response.quality = std::move(responseQuality);
}

void LlmEngine::attachSemanticReview(const std::string& stage, const Json& context,
                                     const Json& candidate, Json& report) const {
    if (!report.is_object()) report = Json::object();
    if (!report.value("passed", false)) {
        report["semantic_quality"] = Json{{"status", "skipped"},
                                            {"review_agent", "quality_agent"},
                                            {"reason", "contract_validation_failed"},
                                            {"authoritative", false}};
        report["quality_status"] = "contract_rejected";
        return;
    }
    QualityReviewResponse review;
    try {
        review = provider_.reviewQuality(reviewRequest(stage, context, candidate, report));
    } catch (const std::exception& error) {
        review.ok = false;
        review.error = std::string("quality_reviewer_exception:") + error.what();
    } catch (...) {
        review.ok = false;
        review.error = "quality_reviewer_exception:unknown";
    }

    const auto invalid = [&report](const std::string& reason, const std::string& error = "") {
        report["semantic_quality"] = Json{{"status", "inconclusive"},
                                            {"review_agent", "quality_agent"},
                                            {"reason", reason}, {"error", error},
                                            {"authoritative", false}};
        report["quality_status"] = "semantic_review_inconclusive";
        report["final_disposition"] = "candidate_semantic_review_pending";
    };
    if (!review.ok) {
        invalid("review_unavailable", review.error);
        return;
    }
    if (!review.review.is_object()) {
        invalid("review_not_object");
        return;
    }
    const auto verdict = stringMember(review.review, "verdict");
    const auto scores = memberOr(review.review, "scores", Json::object());
    const auto confidence = review.review.value("confidence", Json());
    const auto evidence = memberOr(review.review, "evidence", Json::array());
    const auto evidenceRefs = memberOr(review.review, "evidence_refs", Json::array());
    const auto concerns = memberOr(review.review, "concerns", Json::array());
    const auto recommendation = stringMember(review.review, "recommendation");
    bool valid = integerMember(review.review, "schema_version") == 1 &&
                 (verdict == "accept" || verdict == "revise" || verdict == "reject" ||
                  verdict == "inconclusive") &&
                 scores.is_object() && isFiniteBounded(confidence, 0.0, 1.0) &&
                 evidence.is_array() && evidence.size() <= 8 && evidenceRefs.is_array() &&
                 evidenceRefs.size() <= 8 && concerns.is_array() &&
                 concerns.size() <= 8 && recommendation.size() <= 512;
    static const std::array<const char*, 4> scoreNames{
        "grounding", "coherence", "usefulness", "risk_control"};
    double scoreTotal = 0.0;
    for (const auto* name : scoreNames) {
        const auto score = scores.is_object() ? scores.value(name, Json()) : Json();
        if (!isFiniteBounded(score, 0.0, 1.0)) valid = false;
        else scoreTotal += score.get<double>();
    }
    std::set<std::string> availableRefs;
    collectReviewEvidenceRefs(context, availableRefs);
    bool evidenceRefsValid = true;
    for (const auto& ref : evidenceRefs) {
        if (!ref.is_string() || !availableRefs.contains(ref.get<std::string>())) {
            evidenceRefsValid = false;
            break;
        }
    }
    if (!availableRefs.empty() && evidenceRefs.empty()) evidenceRefsValid = false;
    if (!evidenceRefsValid) valid = false;
    if (!valid) {
        invalid("review_contract_failed");
        return;
    }
    const double semanticScore = scoreTotal / static_cast<double>(scoreNames.size());
    const double reviewConfidence = confidence.get<double>();
    const bool advisoryPass = verdict == "accept" && semanticScore >= 0.60 && reviewConfidence >= 0.40;
    const bool advisoryReject = verdict == "reject" || (verdict == "revise" && semanticScore < 0.45);
    const std::string status = advisoryPass ? "passed" : advisoryReject ? "failed" : "inconclusive";
    report["semantic_quality"] = Json{
        {"status", status}, {"review_agent", "quality_agent"}, {"authoritative", false},
        {"verdict", verdict}, {"score", semanticScore}, {"confidence", reviewConfidence},
        {"scores", scores}, {"evidence", evidence}, {"concerns", concerns},
        {"evidence_refs", evidenceRefs}, {"evidence_refs_valid", evidenceRefsValid},
        {"recommendation", recommendation}};
    if (stage == "decision") {
        const auto task = context.value("function_context", Json::object()).value("task", Json::object());
        if (task.value("speech_act_classification_required", false)) {
            report["subagents"]["utterance_understanding_agent"] = Json{
                {"stage", "utterance_understanding"},
                {"status", candidate.value("utterance_understanding", Json::object()).is_object()
                                ? "included_in_semantic_review"
                                : "missing"},
                {"semantic_review_agent", "quality_agent"},
                {"authoritative", false}};
        }
    }
    report["quality_status"] = status == "passed" ? "semantically_reviewed"
                                                    : status == "failed" ? "semantic_review_failed"
                                                                          : "semantic_review_inconclusive";
    report["final_disposition"] = status == "passed"
                                       ? "candidate_semantically_supported"
                                       : status == "failed" ? "candidate_semantically_rejected"
                                                             : "candidate_semantic_review_pending";
}

ProviderResponse LlmEngine::decide(const DecisionContext& context,
                                   const std::string& playerInput,
                                   const Json& validationFeedback) const {
    ProviderResponse response;
    try {
        response = provider_.decide(context, playerInput, validationFeedback);
    } catch (const std::exception& error) {
        response.ok = false;
        response.error = std::string("provider_exception:") + error.what();
    } catch (...) {
        response.ok = false;
        response.error = "provider_exception:unknown";
    }
    auto report = assessDecision(context, response, repairAttempt(validationFeedback));
    attachSemanticReview("decision", Json{{"function_context", context.functionContext}},
                         decisionCandidate(response), report);
    attach(response.quality, response, report);
    return response;
}

ProviderResponse LlmEngine::proposeEvolution(const Json& context,
                                             const std::string& evolutionLevel,
                                             const Json& validationFeedback) const {
    ProviderResponse response;
    try {
        response = provider_.proposeEvolution(context, evolutionLevel, validationFeedback);
    } catch (const std::exception& error) {
        response.ok = false;
        response.error = std::string("provider_exception:") + error.what();
    } catch (...) {
        response.ok = false;
        response.error = "provider_exception:unknown";
    }
    auto report = assessEvolution(context, evolutionLevel, response,
                                  repairAttempt(validationFeedback));
    attachSemanticReview(evolutionLevel == "level1" ? "level1_evolution" : "level2_evolution",
                         context, response.evolutionProposal, report);
    attach(response.quality, response, report);
    return response;
}

ProviderResponse LlmEngine::proposeReflection(const Json& context,
                                              const std::string& targetNpcId,
                                              const Json& validationFeedback) const {
    ProviderResponse response;
    try {
        response = provider_.proposeReflection(context, targetNpcId, validationFeedback);
    } catch (const std::exception& error) {
        response.ok = false;
        response.error = std::string("provider_exception:") + error.what();
    } catch (...) {
        response.ok = false;
        response.error = "provider_exception:unknown";
    }
    auto report = assessReflection(context, response, repairAttempt(validationFeedback));
    attachSemanticReview("reflection", context, response.reflectionResult, report);
    attach(response.quality, response, report);
    return response;
}

Json LlmEngine::assessDecision(const DecisionContext& context,
                               const ProviderResponse& response,
                               const int repairAttempt) {
    QualityBuilder builder{"decision", repairAttempt};
    const auto& functionContext = context.functionContext;
    const bool functionContextShape = functionContext.is_object() &&
                                      integerMember(functionContext, "schema_version") == 1 &&
                                      functionContext.contains("agent") && functionContext.contains("capture") &&
                                      functionContext.contains("task") && functionContext.contains("world") &&
                                      functionContext.contains("self") && functionContext.contains("evidence") &&
                                      functionContext.contains("options");
    builder.check("input_context_contract", functionContextShape,
                  Json{{"schema_version", integerMember(functionContext, "schema_version")},
                       {"plan_count", context.planCatalog.size()}, {"candidate_count", context.candidates.size()}});
    if (functionContextShape) {
        const auto capture = memberOr(functionContext, "capture", Json::object());
        builder.check("input_revision_consistent",
                      unsignedMember(capture, "world_revision") == context.worldRevision &&
                          unsignedMember(capture, "npc_state_version") == context.npcStateVersion,
                      Json{{"context_world_revision", context.worldRevision},
                           {"captured_world_revision", unsignedMember(capture, "world_revision")}});
    }
    const auto privateKeys = [](const Json& root) {
        const auto scan = [](const auto& self, const Json& value, const int depth) -> bool {
            if (depth > 8) return false;
            if (value.is_object()) {
                for (auto item = value.begin(); item != value.end(); ++item) {
                    if (item.key().find("private") != std::string::npos ||
                        item.key().find("hidden") != std::string::npos) return true;
                    if (self(self, item.value(), depth + 1)) return true;
                }
            } else if (value.is_array()) {
                for (const auto& item : value) if (self(self, item, depth + 1)) return true;
            }
            return false;
        };
        return scan(scan, root, 0);
    };
    builder.check("input_visibility_boundary",
                  !privateKeys(memberOr(functionContext, "world", Json::object())),
                  Json{{"world_private_fields", privateKeys(memberOr(functionContext, "world", Json::object()))}});
    builder.check("provider_ok", response.ok,
                  Json{{"error", response.error}, {"dialogue_bytes", response.dialogue.size()}});
    if (!response.ok) return builder.finish();

    const auto& proposal = response.proposal;
    builder.check("revision_bound", proposal.worldRevision == context.worldRevision &&
                                      proposal.npcStateVersion == context.npcStateVersion,
                  Json{{"proposal_world_revision", proposal.worldRevision},
                       {"context_world_revision", context.worldRevision},
                       {"proposal_npc_state_version", proposal.npcStateVersion},
                       {"context_npc_state_version", context.npcStateVersion}});
    builder.check("confidence_bounded", isFiniteBounded(Json(proposal.confidence), 0.0, 1.0));
    builder.check("intent_present", !proposal.intent.empty() && proposal.intent.size() <= 128,
                  Json{{"bytes", proposal.intent.size()}});
    // Rationale and expected outcome are optional explanations. Autonomous
    // decisions intentionally use the smaller action contract; only their
    // bounded size is a quality gate. Runtime execution and the deterministic
    // event explanation remain authoritative when these fields are omitted.
    builder.check("rationale_bounded", proposal.rationale.size() <= 4096,
                  Json{{"bytes", proposal.rationale.size()}, {"required", false}});
    builder.check("outcome_bounded", proposal.expectedOutcome.size() <= 4096,
                  Json{{"bytes", proposal.expectedOutcome.size()}, {"required", false}});
    const auto candidate = std::find_if(context.candidates.begin(), context.candidates.end(),
                                        [&proposal](const auto& item) { return item.actionId == proposal.actionId; });
    const bool selectedKnown = proposal.actionId.empty() || candidate != context.candidates.end();
    const bool selectedEligible = proposal.actionId.empty() ||
                                  (candidate != context.candidates.end() && candidate->availableNow);
    builder.check("action_known", selectedKnown,
                  Json{{"action_id", proposal.actionId}, {"candidate_count", context.candidates.size()}});
    builder.check("action_eligible", selectedEligible,
                  Json{{"action_id", proposal.actionId},
                       {"available_now", candidate == context.candidates.end() ? false : candidate->availableNow}});
    const bool actionRequirement = !context.actionRequired || !context.worldActionAllowed || !proposal.actionId.empty();
    builder.check("action_requirement", actionRequirement,
                  Json{{"required", context.actionRequired}, {"selected", !proposal.actionId.empty()}});
    bool reliefValid = true;
    for (const auto& [needId, value] : proposal.expectedRelief) {
        if (!context.evidenceRefs.empty() && needId.empty()) reliefValid = false;
        if (!isFiniteBounded(Json(value), 0.0, 1.0)) reliefValid = false;
        if (std::none_of(context.drives.begin(), context.drives.end(),
                         [&needId](const auto& drive) { return drive.id == needId; })) reliefValid = false;
    }
    builder.check("expected_relief_valid", reliefValid,
                  Json{{"entry_count", proposal.expectedRelief.size()}});
    bool evidenceValid = true;
    for (const auto& ref : proposal.citedEvidenceRefs) evidenceValid = evidenceValid && context.evidenceRefs.contains(ref);
    for (const auto& id : proposal.citedStimulusIds) {
        evidenceValid = evidenceValid && std::any_of(context.stimuli.begin(), context.stimuli.end(),
                                                     [&id](const auto& stimulus) { return stimulus.id == id; });
    }
    builder.check("evidence_valid", evidenceValid,
                  Json{{"evidence_refs", proposal.citedEvidenceRefs.size()},
                       {"stimulus_refs", proposal.citedStimulusIds.size()}});
    if (context.speechActClassificationRequired) {
        builder.check("speech_act_understood", !proposal.speechAct.empty() &&
                                                    !proposal.utteranceUnderstanding.empty(),
                      Json{{"speech_act", proposal.speechAct}});
        builder.check("dialogue_present", !response.dialogue.empty(),
                      Json{{"dialogue_bytes", response.dialogue.size()}});
    }
    return builder.finish();
}

Json LlmEngine::assessReflection(const Json& context,
                                 const ProviderResponse& response,
                                 const int repairAttempt) {
    QualityBuilder builder{"reflection", repairAttempt};
    const bool contextShape = context.is_object() && integerMember(context, "schema_version") == 1 &&
                              stringMember(context, "agent_role") == "reflection" &&
                              context.contains("current_state") && context.contains("new_evidence") &&
                              context.contains("innovation_gate") && context.contains("function_context") &&
                              context.at("function_context").is_object() &&
                              integerMember(context.at("function_context"), "schema_version") == 1;
    builder.check("input_context_contract", contextShape,
                  Json{{"schema_version", integerMember(context, "schema_version")},
                       {"evidence_count", memberOr(context, "new_evidence", Json::array()).is_array()
                                                ? memberOr(context, "new_evidence", Json::array()).size()
                                                : 0}});
    builder.check("provider_ok", response.ok, Json{{"error", response.error}});
    if (!response.ok) return builder.finish();
    const auto& value = response.reflectionResult;
    builder.check("object_result", value.is_object());
    if (!value.is_object()) return builder.finish();
    builder.check("schema_version", integerMember(value, "schema_version") == 1);
    const auto expectedNpc = stringMember(context, "target_npc");
    const auto actualNpc = stringMember(value, "npc_id");
    builder.check("target_bound", expectedNpc.empty() || actualNpc == expectedNpc,
                  Json{{"expected", expectedNpc}, {"actual", actualNpc}});
    builder.check("evidence_bounded", hasArray(value, "evidence_event_sequences", 32));
    const auto cognition = memberOr(value, "cognition_updates", Json::object());
    builder.check("cognition_shape", cognition.is_object() &&
                                      cognition.contains("belief_updates") &&
                                      cognition.contains("experience_updates") &&
                                      cognition.contains("memory_updates"));
    builder.check("issues_bounded", hasArray(value, "unresolved_issues", 16) &&
                                    hasArray(value, "resolved_issues", 16));
    builder.check("innovation_bounded", hasArray(value, "innovation_hypotheses", 1) &&
                                        hasArray(value, "innovation_actions", 8));
    return builder.finish();
}

Json LlmEngine::assessEvolution(const Json& context,
                                const std::string& evolutionLevel,
                                const ProviderResponse& response,
                                const int repairAttempt) {
    QualityBuilder builder{evolutionLevel == "level1" ? "level1_evolution" : "level2_evolution",
                           repairAttempt};
    const std::string expectedRole = evolutionLevel == "level1" ? "level1_evolution" : "level2_evolution";
    const bool contextShape = context.is_object() && integerMember(context, "schema_version") == 1 &&
                              stringMember(context, "agent_role") == expectedRole &&
                              context.contains("content") && context.contains("allowed_operations") &&
                              context.contains("evidence_events") && context.contains("function_context") &&
                              context.at("function_context").is_object() &&
                              integerMember(context.at("function_context"), "schema_version") == 1;
    builder.check("input_context_contract", contextShape,
                  Json{{"schema_version", integerMember(context, "schema_version")},
                       {"allowed_operation_count", memberOr(context, "allowed_operations", Json::array()).is_array()
                                                           ? memberOr(context, "allowed_operations", Json::array()).size()
                                                           : 0}});
    builder.check("provider_ok", response.ok, Json{{"error", response.error}});
    if (!response.ok) return builder.finish();
    const auto& value = response.evolutionProposal;
    builder.check("object_result", value.is_object());
    if (!value.is_object()) return builder.finish();
    const auto kind = stringMember(value, "proposal_kind", stringMember(value, "kind"));
    builder.check("proposal_kind", kind == "no_change" || kind == "content" || kind == "strategy" ||
                                           kind == "declarative_capability" || kind == "innovation",
                  Json{{"kind", kind}});
    const auto evidence = memberOr(value, "evidence_event_sequences", Json::array());
    builder.check("evidence_present", evidence.is_array() && !evidence.empty() && evidence.size() <= 32,
                  Json{{"count", evidence.is_array() ? evidence.size() : 0}});
    const auto operations = memberOr(value, "operations", Json::array());
    const bool noChange = kind == "no_change";
    builder.check("operation_budget", operations.is_array() && operations.size() <= 1 &&
                                      (noChange ? operations.empty() : !operations.empty()),
                  Json{{"count", operations.is_array() ? operations.size() : 0}});
    if (kind == "innovation") {
        const auto innovation = memberOr(value, "innovation", Json::object());
        const auto plan = memberOr(innovation, "test_plan", Json::object());
        builder.check("innovation_plan", innovation.is_object() && hasString(innovation, "hypothesis", 1024) &&
                                           hasString(innovation, "novelty_basis", 64) && plan.is_object() &&
                                           hasString(plan, "baseline", 512) && hasString(plan, "scenario", 512) &&
                                           hasString(plan, "success_criteria", 512) && hasString(plan, "failure_criteria", 512),
                      Json{{"operation_ids", memberOr(innovation, "operation_ids", Json::array())}});
    }
    if (evolutionLevel == "level1") {
        const auto target = stringMember(context, "target_npc");
        builder.check("private_scope_context", !target.empty(), Json{{"target_npc_present", !target.empty()}});
    }
    return builder.finish();
}

}  // namespace tide_marks
