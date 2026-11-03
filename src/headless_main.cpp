#include "tide_marks.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace tide_marks;

namespace {

void usage(const char* program) {
    std::cout << "Usage: " << program << " [--scenario village] [--days N] [--provider mock|llama] [--replay PATH] [--dump-events]\n";
}

}  // namespace

int main(int argc, char** argv) {
    int days = 2;
    std::string replayPath;
    std::string providerName = "mock";
    bool dumpEvents = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--days" && i + 1 < argc) {
            days = std::max(0, std::atoi(argv[++i]));
        } else if (argument == "--replay" && i + 1 < argc) {
            replayPath = argv[++i];
        } else if (argument == "--provider" && i + 1 < argc) {
            providerName = argv[++i];
        } else if (argument == "--dump-events") {
            dumpEvents = true;
        }
    }

    std::unique_ptr<IProvider> provider;
    if (providerName == "llama") {
        LlamaCppConfig config;
        if (const char* model = std::getenv("TIDE_MARKS_MODEL"); model != nullptr) config.model = model;
        if (const char* base = std::getenv("TIDE_MARKS_BASE_URL"); base != nullptr) config.baseUrl = base;
        provider = std::make_unique<LlamaCppProvider>(config);
    } else {
        provider = std::make_unique<MockProvider>();
    }
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), std::move(provider));
    std::cout << "《潮痕》 village scenario | provider=" << service.providerName() << '\n';
    std::cout << summarize(service.state()) << '\n';

    const auto dialogue = service.converse("mira", "Can you spare a ration?", "turn-opening-mira");
    std::cout << "dialogue: " << (dialogue.accepted ? dialogue.dialogue : dialogue.error)
              << " provider=" << dialogue.provider << (dialogue.error.empty() ? "" : " error=" + dialogue.error) << '\n';
    service.advanceMinutes(static_cast<std::int64_t>(days) * kMinutesPerDay);
    std::cout << "after " << days << " day(s): " << summarize(service.state()) << '\n';
    if (!replayPath.empty()) {
        service.writeReplay(replayPath);
        const auto replayed = WorldService::replay(replayPath);
        std::cout << "replay: " << summarize(replayed.state()) << '\n';
    }
    if (dumpEvents) {
        for (const auto& event : service.state().events) {
            std::cout << "event " << event.sequence << ' ' << event.at.toString() << ' '
                      << event.type << ' ' << event.actor << " -> " << event.target
                      << " | " << event.detail << '\n';
        }
    }
    return 0;
}
