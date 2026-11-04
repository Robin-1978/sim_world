#include "tide_marks.hpp"

#include <cstdlib>
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
                 " [--anomalies] [--experiment PATH]\n";
}

}  // namespace

int main(int argc, char** argv) {
    int days = 2;
    std::string replayPath;
    std::string contentRoot = ContentPackLoader::defaultRoot();
    std::string experimentPath;
    std::string providerName = "profile";
    std::string profilePath = defaultIntelligenceProfilePath();
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
        } else if (argument == "--days" && i + 1 < argc) {
            days = std::max(0, std::atoi(argv[++i]));
        } else if (argument == "--replay" && i + 1 < argc) {
            replayPath = argv[++i];
        } else if (argument == "--provider" && i + 1 < argc) {
            providerName = argv[++i];
        } else if (argument == "--profile" && i + 1 < argc) {
            profilePath = argv[++i];
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
    auto service = WorldService::load(contentRoot, std::move(provider));
    const auto worldName = service.content().instance
                               ? service.content().instance->displayName
                               : service.content().worldId;
    std::cout << "《潮痕》 " << worldName << " | provider=" << service.providerName() << '\n';
    std::cout << summarize(service.state()) << '\n';

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
    service.advanceMinutes(static_cast<std::int64_t>(days) * kMinutesPerDay);
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
