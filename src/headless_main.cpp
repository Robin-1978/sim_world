#include "tide_marks.hpp"
#include "core/json_utils.hpp"
#include "evolution/evolution.hpp"
#include "world/world_evolution_system.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>

using namespace tide_marks;

namespace {

void usage(const char* program) {
    std::cout << "Usage: " << program
              << " [--content-root PATH] [--days N] [--profile PATH]"
                 " [--provider mock|profile|llama_cpp] [--replay PATH] [--dump-events]"
                 " [--input-replay PATH] [--probe decision|reflection|level1|level2|all]"
                 " [--probe-target NPC_ID]"
                 " [--probe-trigger TRIGGER]"
                 " [--database PATH]"
                 " [--anomalies] [--experiment PATH] [--evolve level1|level2]"
                 " [--governance-auto]"
                 " [--wait-llm-ms N] [--durable-checkpoint PATH]"
                 " [--checkpoint-every-minutes N|--checkpoint-daily]\n";
}

Json probeProviderStages(WorldService& service, const std::string& stage,
                         const std::string& probeTarget,
                         const std::string& probeTrigger) {
    Json report{{"provider", service.providerName()}, {"stage", stage}, {"results", Json::array()}};
    const auto addResult = [&report](Json value) { report["results"].push_back(std::move(value)); };
    const auto runDecision = [&]() {
        for (const auto& authored : service.content().npcs) {
            const auto& npcId = authored.definition.id;
            if (!probeTarget.empty() && npcId != probeTarget) continue;
            const auto turn = service.deliberate(npcId, probeTrigger, "probe:decision:" + npcId);
            const auto context = service.decisionContext(npcId);
            const auto selected = std::find_if(
                context.candidates.begin(), context.candidates.end(),
                [&turn](const auto& candidate) { return candidate.actionId == turn.actionId; });
            const bool eligible = selected != context.candidates.end() && selected->availableNow;
            const bool evidenceValid = [&turn, &context] {
                if (!turn.event) return false;
                const auto refs = turn.event->data.value("cited_evidence_refs", Json::array());
                if (!refs.is_array()) return false;
                return std::all_of(refs.begin(), refs.end(), [&context](const auto& ref) {
                    return ref.is_string() && context.evidenceRefs.contains(ref.template get<std::string>());
                });
            }();
            addResult(Json{{"kind", "decision"}, {"npc_id", npcId}, {"accepted", turn.accepted},
                           {"action_id", turn.actionId}, {"error", turn.error},
                           {"dialogue_bytes", turn.dialogue.size()},
                           {"quality", Json{{"catalog_plan_count", context.planCatalog.size()},
                                             {"eligible_plan_count", std::count_if(
                                                 context.candidates.begin(), context.candidates.end(),
                                                 [](const auto& candidate) { return candidate.availableNow; })},
                                             {"selected_plan_is_eligible", eligible},
                                             {"evidence_refs_valid", evidenceValid},
                                             {"llm_choice_accepted_by_runtime", turn.accepted},
                                             {"process_running_after_commit", service.state().npcs.at(npcId).activity.process.has_value()}}}});
        }
    };
    const auto runReflection = [&]() {
        for (const auto& authored : service.content().npcs) {
            const auto& npcId = authored.definition.id;
            const auto response = service.requestDailyReflection(npcId);
            Json result{{"kind", "reflection"}, {"npc_id", npcId}, {"provider_ok", response.ok},
                        {"error", response.error}};
            if (response.ok && response.reflectionResult.is_object()) {
                const auto& reflection = response.reflectionResult;
                const auto cognition = reflection.value("cognition_updates", Json::object());
                result["belief_count"] = cognition.value("belief_updates", Json::array()).size();
                result["experience_count"] = cognition.value("experience_updates", Json::array()).size();
                result["memory_count"] = cognition.value("memory_updates", Json::array()).size();
                result["innovation_count"] = reflection.value("innovation_hypotheses", Json::array()).size();
                result["evidence_count"] = reflection.value("evidence_event_sequences", Json::array()).size();
                const auto committed = service.commitDailyReflection(npcId, response);
                result["committed"] = committed.accepted;
                result["commit_code"] = committed.code;
            } else {
                result["committed"] = false;
                result["commit_code"] = "provider_failed";
            }
            addResult(std::move(result));
        }
    };
    const auto runEvolution = [&](const std::string& level) {
        std::string target;
        if (level == "level1") {
            target = probeTarget;
            if (target.empty() && !service.content().npcs.empty()) {
                target = service.content().npcs.front().definition.id;
            }
        }
        // Use the same bounded normalization/validation/repair path as an
        // explicit Runtime request. A raw provider response is not evidence
        // of proposal quality, and validating it without the request-scoped
        // Level 1 target produces a false `missing_target_npc` failure.
        const auto outcome = EvolutionManager::propose(service, level, "probe:" + level, target);
        Json result{{"kind", level}, {"provider_ok", outcome.accepted}, {"error", outcome.accepted ? "" : outcome.code},
                    {"message", outcome.message}};
        if (outcome.accepted) {
            const auto proposal = EvolutionManager::toJson(outcome.proposal);
            result["proposal_id"] = proposal.value("id", "");
            result["proposal_kind"] = proposal.value("kind", proposal.value("proposal_kind", ""));
            result["operation_count"] = proposal.value("operations", Json::array()).size();
            try {
                auto normalized = EvolutionManager::normalizeProviderProposal(
                    proposal, service, level, service.providerName());
                if (level == "level1") normalized.targetNpcId = target;
                const auto validation = EvolutionManager::validate(
                    normalized, service.content(), service.state());
                result["runtime_valid"] = validation.valid;
                result["runtime_code"] = validation.code;
                result["runtime_message"] = validation.message;
                if (level == "level2") {
                    const auto captured = WorldEvolutionSystem::proposalContext(
                        service, level, "probe:" + level, Json::object(), target);
                    std::set<std::uint64_t> problemSequences;
                    for (const auto& item : captured.value("problem_evidence", Json::array())) {
                        if (item.is_object()) problemSequences.insert(item.value("sequence", std::uint64_t{0}));
                    }
                    bool evidenceSupported = false;
                    for (const auto& item : proposal.value("evidence_event_sequences", Json::array())) {
                        if (!item.is_number_integer() && !item.is_number_unsigned()) continue;
                        const auto sequence = item.is_number_unsigned()
                                                   ? item.get<std::uint64_t>()
                                                   : item.get<std::int64_t>() > 0
                                                         ? static_cast<std::uint64_t>(item.get<std::int64_t>())
                                                         : 0;
                        if (sequence != 0 && problemSequences.contains(sequence)) evidenceSupported = true;
                    }
                    result["quality"] = Json{{"public_problem_evidence_count", problemSequences.size()},
                                              {"cited_public_problem_evidence", evidenceSupported},
                                              {"runtime_validation_passed", validation.valid},
                                              {"automatic_governor_admissible", validation.valid && evidenceSupported}};
                } else {
                    result["quality"] = Json{{"runtime_validation_passed", validation.valid},
                                              {"automatic_governor_admissible", validation.valid}};
                }
            } catch (const std::exception& error) {
                result["runtime_valid"] = false;
                result["runtime_code"] = "probe_normalization_error";
                result["runtime_message"] = error.what();
            }
        }
        addResult(std::move(result));
    };
    if (stage == "decision" || stage == "all") runDecision();
    if (stage == "reflection" || stage == "all") runReflection();
    if (stage == "level1" || stage == "all") runEvolution("level1");
    if (stage == "level2" || stage == "all") runEvolution("level2");
    return report;
}

}  // namespace

// This is an explicit external-governance harness for long-running evidence
// runs. It is opt-in and lives in the headless tool, not in the Runtime or
// Provider: the LLM can only produce a proposal, while this separately named
// actor re-runs the deterministic sandbox, approves it, activates the overlay,
// and (for a private innovation) starts adoption through the normal APIs.
void runGovernanceHarness(WorldService& service, std::size_t& cursor) {
    while (cursor < service.state().events.size()) {
        // Copy before calling a mutating governance API: activation and
        // adoption append events and may reallocate the authoritative vector.
        const auto event = service.state().events[cursor++];
        if (event.type == "evolution.proposal_ready" && event.data.is_object() &&
            event.data.contains("proposal") && event.data.at("proposal").is_object()) {
            try {
                auto proposal = EvolutionManager::fromJson(event.data.at("proposal"));
                proposal.baseWorldRevision = service.state().revision;
                const auto sandbox = EvolutionManager::sandbox(proposal, service);
                if (!sandbox.passed) continue;
                proposal.status = EvolutionProposalStatus::Validated;
                const auto approval = EvolutionManager::approve(proposal, "headless_governor", true);
                if (!approval.valid) continue;
                const auto activated = service.activateOverlay(proposal, sandbox);
                std::cout << "governance: proposal=" << proposal.id
                          << " activation=" << (activated.accepted ? "active" : activated.code) << '\n';
            } catch (const std::exception& error) {
                std::cerr << "governance: proposal error=" << error.what() << '\n';
            }
            continue;
        }
        if (event.type != "innovation.hypothesis_created" || !event.data.is_object() ||
            !event.data.contains("proposal") || !event.data.at("proposal").is_object()) {
            continue;
        }
        const std::string npcId = event.data.value("target_npc", event.actor);
        const std::string proposalId = event.data.value("proposal_id", "");
        if (npcId.empty() || proposalId.empty()) continue;
        try {
            const auto accepted = service.transitionInnovationHypothesis(
                npcId, proposalId, "accepted", "headless_governor");
            if (!accepted.accepted) continue;
            auto proposal = EvolutionManager::fromJson(event.data.at("proposal"));
            proposal.innovationStatus = "accepted";
            proposal.baseWorldRevision = service.state().revision;
            const auto validation = EvolutionManager::validate(proposal, service.content(), service.state());
            if (!validation.valid) continue;
            const auto sandbox = EvolutionManager::sandbox(proposal, service);
            if (!sandbox.passed) continue;
            proposal.status = EvolutionProposalStatus::Validated;
            const auto approval = EvolutionManager::approve(proposal, "headless_governor", true);
            if (!approval.valid) continue;
            const auto activated = service.activateOverlay(proposal, sandbox);
            if (!activated.accepted) continue;
            const auto adoption = service.startInnovationAdoption(npcId, proposalId);
            std::cout << "governance: hypothesis=" << proposalId
                      << " activation=active adoption=" << adoption.code << '\n';
        } catch (const std::exception& error) {
            std::cerr << "governance: hypothesis error=" << error.what() << '\n';
        }
    }
}

int main(int argc, char** argv) {
    int days = 2;
    std::string replayPath;
    std::string contentRoot = ContentPackLoader::defaultRoot();
    std::string experimentPath;
    std::string evolutionLevel;
    std::string providerName = "profile";
    std::string profilePath = defaultIntelligenceProfilePath();
    std::int64_t waitLlmMilliseconds = 0;
    std::string durableCheckpointPath;
    std::string databasePath;
    std::string inputReplayPath;
    std::string probeStage;
    std::string probeTarget;
    std::string probeTrigger = "high_salience_context";
    std::int64_t checkpointIntervalMinutes = 0;
    bool checkpointDaily = false;
    bool dumpEvents = false;
    bool governanceAuto = false;
    bool dumpAnomalies = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--content-root" && i + 1 < argc) {
            contentRoot = argv[++i];
        } else if (argument == "--experiment" && i + 1 < argc) {
            experimentPath = argv[++i];
        } else if (argument == "--anomalies") {
            dumpAnomalies = true;
        } else if (argument == "--evolve" && i + 1 < argc) {
            evolutionLevel = argv[++i];
        } else if (argument == "--governance-auto") {
            governanceAuto = true;
        } else if (argument == "--days" && i + 1 < argc) {
            days = std::max(0, std::atoi(argv[++i]));
        } else if (argument == "--replay" && i + 1 < argc) {
            replayPath = argv[++i];
        } else if (argument == "--input-replay" && i + 1 < argc) {
            inputReplayPath = argv[++i];
        } else if (argument == "--probe" && i + 1 < argc) {
            probeStage = argv[++i];
        } else if (argument == "--probe-target" && i + 1 < argc) {
            probeTarget = argv[++i];
        } else if (argument == "--probe-trigger" && i + 1 < argc) {
            probeTrigger = argv[++i];
        } else if (argument == "--provider" && i + 1 < argc) {
            providerName = argv[++i];
        } else if (argument == "--profile" && i + 1 < argc) {
            profilePath = argv[++i];
        } else if (argument == "--database" && i + 1 < argc) {
            databasePath = argv[++i];
        } else if (argument == "--wait-llm-ms" && i + 1 < argc) {
            waitLlmMilliseconds = std::max<std::int64_t>(0, std::atoll(argv[++i]));
        } else if (argument == "--durable-checkpoint" && i + 1 < argc) {
            durableCheckpointPath = argv[++i];
        } else if (argument == "--checkpoint-every-minutes" && i + 1 < argc) {
            checkpointIntervalMinutes = std::max<std::int64_t>(0, std::atoll(argv[++i]));
        } else if (argument == "--checkpoint-daily") {
            checkpointDaily = true;
        } else if (argument == "--dump-events") {
            dumpEvents = true;
        }
    }

    std::unique_ptr<IProvider> provider;
    if (providerName == "mock") {
        provider = std::make_unique<MockProvider>();
    } else {
        const auto profile = loadIntelligenceProfile(profilePath);
        provider = makeProviderFromProfile(profile, providerName);
    }
    WorldService service = [&]() {
        if (!inputReplayPath.empty()) {
            if (!databasePath.empty()) throw std::runtime_error("--input-replay and --database are mutually exclusive");
            return WorldService::replay(inputReplayPath, contentRoot, std::move(provider));
        }
        if (databasePath.empty()) return WorldService::load(contentRoot, std::move(provider));
        const bool initialized = std::filesystem::exists(databasePath) &&
                                  std::filesystem::file_size(databasePath) > 0;
        if (initialized) return WorldService::loadSqliteInstance(databasePath, std::move(provider));
        return WorldService::createSqliteInstance(databasePath, contentRoot, std::move(provider));
    }();
    if (checkpointIntervalMinutes > 0 && durableCheckpointPath.empty()) {
        std::cerr << "--checkpoint-every-minutes requires --durable-checkpoint PATH\n";
        return 2;
    }
    if (checkpointDaily && durableCheckpointPath.empty()) {
        std::cerr << "--checkpoint-daily requires --durable-checkpoint PATH\n";
        return 2;
    }
    if (checkpointDaily && checkpointIntervalMinutes > 0) {
        std::cerr << "--checkpoint-daily and --checkpoint-every-minutes are mutually exclusive\n";
        return 2;
    }
    if (!durableCheckpointPath.empty()) {
        service.configureDurableCheckpoint(DurableCheckpointPolicy{
            checkpointIntervalMinutes > 0 ? DurableCheckpointMode::PeriodicMinutes
                                           : DurableCheckpointMode::DayBoundary,
            durableCheckpointPath,
            checkpointIntervalMinutes});
    }
    const auto worldName = service.content().instance
                               ? service.content().instance->displayName
                               : service.content().worldId;
    std::cout << "《潮痕》 " << worldName << " | provider=" << service.providerName() << '\n';
    std::cout << summarize(service.state()) << '\n';

    if (!probeStage.empty()) {
        static const std::set<std::string> validStages{"decision", "reflection", "level1", "level2", "all"};
        if (!validStages.contains(probeStage)) {
            std::cerr << "unknown --probe stage: " << probeStage << '\n';
            return 2;
        }
        std::cout << "probe: " << probeProviderStages(service, probeStage, probeTarget, probeTrigger).dump() << '\n';
        return 0;
    }

    if (!evolutionLevel.empty()) {
        const auto proposal = EvolutionManager::propose(service, evolutionLevel, "headless_explicit_request");
        Json output{{"accepted", proposal.accepted}, {"code", proposal.code},
                    {"message", proposal.message}, {"provider", proposal.provider}};
        if (proposal.accepted) output["proposal"] = EvolutionManager::toJson(proposal.proposal);
        std::cout << "evolution: " << output.dump() << '\n';
        return proposal.accepted ? 0 : 3;
    }

    if (dumpAnomalies) {
        Json findings = Json::array();
        for (const auto& finding : AnomalyDetector::scan(service.content(), service.state())) {
            findings.push_back(anomalyFindingToJson(finding));
        }
        std::cout << "anomalies: " << findings.dump() << '\n';
    }
    if (!experimentPath.empty()) {
        std::ifstream input(experimentPath);
        if (!input) {
            std::cerr << "cannot read experiment proposal: " << experimentPath << '\n';
            return 2;
        }
        try {
            Json document;
            input >> document;
            const auto result = ScenarioManager::run(ScenarioManager::fromJson(document), service);
            std::cout << "experiment: " << scenarioExperimentResultToJson(result).dump() << '\n';
            return result.passed ? 0 : 3;
        } catch (const std::exception& error) {
            std::cerr << "experiment error: " << error.what() << '\n';
            return 2;
        }
    }

    if (!service.content().npcs.empty()) {
        const auto& openingNpc = service.content().npcs.front().definition.id;
        const auto dialogue = service.converse(openingNpc, "Please tell me what you are doing.",
                                               "opening:" + service.state().instanceId);
        std::cout << "dialogue: " << (dialogue.accepted ? dialogue.dialogue : dialogue.error)
                  << " provider=" << dialogue.provider
                  << (dialogue.error.empty() ? "" : " error=" + dialogue.error) << '\n';
    }
    const auto totalMinutes = static_cast<std::int64_t>(days) * kMinutesPerDay;
    std::size_t drainedTurns = 0;
    std::size_t governanceCursor = 0;
    if (waitLlmMilliseconds > 0) {
        // A large offline jump would make every asynchronous decision older
        // than its authored game-time deadline before the worker can answer.
        // Advance one scheduler hour at a time when the caller explicitly
        // requests an LLM drain, preserving the same boundary semantics as an
        // interactive loop without blocking the normal runtime path.
        std::int64_t remaining = totalMinutes;
        while (remaining > 0) {
            const auto step = std::min<std::int64_t>(kMinutesPerHour, remaining);
            service.advanceMinutes(step);
            drainedTurns += service.waitForAsyncTurns(waitLlmMilliseconds);
            if (governanceAuto) runGovernanceHarness(service, governanceCursor);
            remaining -= step;
        }
    } else {
        service.advanceMinutes(totalMinutes);
    }
    if (waitLlmMilliseconds > 0) {
        // A day/hour loop uses a bounded wait so simulation time cannot be
        // held hostage by a provider.  Before reporting a long-run result,
        // give the serial provider lane one explicit final drain window; this
        // prevents a valid reflection or repair that finished at the boundary
        // from being mistaken for a permanently pending evolution.
        if (service.pendingAsyncTurns() > 0) {
            const auto finalDrainMilliseconds = std::max<std::int64_t>(waitLlmMilliseconds, 120000);
            drainedTurns += service.waitForAsyncTurns(finalDrainMilliseconds);
            if (governanceAuto) runGovernanceHarness(service, governanceCursor);
            std::cout << "llm-final-drain: consumed=" << drainedTurns
                      << " pending=" << service.pendingAsyncTurns()
                      << " timeout_ms=" << finalDrainMilliseconds << '\n';
        }
        if (governanceAuto) runGovernanceHarness(service, governanceCursor);
        std::cout << "llm-drain: consumed=" << drainedTurns
                  << " pending=" << service.pendingAsyncTurns() << '\n';
    }
    std::cout << "after " << days << " day(s): " << summarize(service.state()) << '\n';
    if (!replayPath.empty()) {
        service.writeReplay(replayPath);
        const auto replayed = WorldService::replay(replayPath, contentRoot);
        std::cout << "replay: " << replayed.state().worldId << " | " << summarize(replayed.state()) << '\n';
    }
    if (dumpEvents) {
        for (const auto& event : service.state().events) {
            std::cout << "event " << event.sequence << ' ' << event.at.toString() << ' '
                      << event.type << ' ' << event.actor << " -> " << event.target
                      << " | " << event.detail << " | id=" << event.eventId
                      << " correlation=" << event.correlationId
                      << " causation=" << event.causationSequence
                      << " data=" << compactJsonNumbers(event.data).dump() << '\n';
        }
    }
    return 0;
}
