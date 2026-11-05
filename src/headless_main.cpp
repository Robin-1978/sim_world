#include "tide_marks.hpp"
#include "evolution/evolution.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using namespace tide_marks;

namespace {

void usage(const char* program) {
    std::cout << "Usage: " << program
              << " [--content-root PATH] [--days N] [--profile PATH]"
                 " [--provider mock|profile|llama_cpp] [--replay PATH] [--dump-events]"
                 " [--database PATH]"
                 " [--anomalies] [--experiment PATH] [--evolve level1|level2]"
                 " [--wait-llm-ms N] [--durable-checkpoint PATH]"
                 " [--checkpoint-every-minutes N|--checkpoint-daily]\n";
}

}  // namespace

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
    std::int64_t checkpointIntervalMinutes = 0;
    bool checkpointDaily = false;
    bool dumpEvents = false;
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
        } else if (argument == "--days" && i + 1 < argc) {
            days = std::max(0, std::atoi(argv[++i]));
        } else if (argument == "--replay" && i + 1 < argc) {
            replayPath = argv[++i];
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
            remaining -= step;
        }
    } else {
        service.advanceMinutes(totalMinutes);
    }
    if (waitLlmMilliseconds > 0) {
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
                      << " causation=" << event.causationSequence << '\n';
        }
    }
    return 0;
}
