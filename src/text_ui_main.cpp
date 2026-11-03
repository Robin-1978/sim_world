#include "tide_marks.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace tide_marks;

namespace {

std::unique_ptr<IProvider> providerFromEnvironment() {
    if (const char* selected = std::getenv("TIDE_MARKS_PROVIDER"); selected != nullptr && std::string(selected) == "llama") {
        LlamaCppConfig config;
        if (const char* model = std::getenv("TIDE_MARKS_MODEL"); model != nullptr) config.model = model;
        if (const char* base = std::getenv("TIDE_MARKS_BASE_URL"); base != nullptr) config.baseUrl = base;
        return std::make_unique<LlamaCppProvider>(config);
    }
    return std::make_unique<MockProvider>();
}

void printHelp() {
    std::cout << "Commands:\n"
              << "  look                         show time, resources, NPCs\n"
              << "  map                          show authored topology\n"
              << "  entities                     show runtime entity lifecycle\n"
              << "  entity-create <id> <archetype>\n"
              << "  entity-activate|entity-dormant|entity-destroy <id>\n"
              << "  talk <npc> <text>            talk to mira, lorn, or eve\n"
              << "  advance <minutes>            advance all NPC life\n"
              << "  day                          advance one day\n"
              << "  events [count]               show recent committed events\n"
              << "  promise <from> <to> <days> <text>\n"
              << "  accept|refuse|renegotiate|fulfill <id> <actor>\n"
              << "  save <path>                  write a JSON snapshot\n"
              << "  replay <path>                write a JSONL replay\n"
              << "  help                         show this help\n"
              << "  quit                         exit\n";
}

void printLook(const WorldService& service) {
    std::cout << summarize(service.state()) << "\n";
    for (const auto& [id, npc] : service.state().npcs) {
        std::cout << "  " << id << " mode=" << npc.mode << " goal_stage=" << npc.goalStage
                  << " memory=" << npc.memories.size() << " promises=" << npc.openPromises.size() << "\n";
        const auto context = service.decisionContext(id);
        for (const auto& drive : context.drives) {
            std::cout << "    drive " << drive.domain << "/" << drive.dimension << ":" << drive.id
                      << " deficit=" << drive.deficit << " pressure=" << drive.drive
                      << " sensitivity=" << drive.sensitivity << " confidence=" << drive.evidenceConfidence
                      << (drive.critical ? " CRITICAL" : "") << "\n";
        }
        const std::size_t visibleStimuli = std::min<std::size_t>(3, context.stimuli.size());
        for (std::size_t index = 0; index < visibleStimuli; ++index) {
            const auto& stimulus = context.stimuli[index];
            std::cout << "    stimulus " << stimulus.category << " from " << stimulus.sourceEvent
                      << " seq=" << stimulus.sourceSequence << " salience=" << stimulus.effectiveSalience
                      << " expires=" << stimulus.expiresAt.toString() << "\n";
        }
        for (const auto& constraint : context.constraints) {
            std::cout << "    constraint " << constraint.category << ":" << constraint.id
                      << " - " << constraint.reason << "\n";
        }
    }
}

void printMap(const WorldService& service) {
    std::cout << "World topology:\n";
    for (const auto& entity : service.entities().all()) {
        const auto* spatial = service.entities().get<SpatialComponent>(entity.id);
        if (spatial == nullptr) continue;
        std::cout << "  [" << entity.kind << "] " << entity.id << " - " << spatial->displayName
                  << " bounds=(" << spatial->minX << "," << spatial->minZ << ")..(" << spatial->maxX
                  << "," << spatial->maxZ << ")\n";
    }
    for (const auto& entity : service.entities().all()) {
        const auto* link = service.entities().get<LinkComponent>(entity.id);
        if (link == nullptr) continue;
        std::cout << "  [" << entity.kind << "] " << entity.id << " endpoints=";
        for (std::size_t i = 0; i < link->endpoints.size(); ++i) {
            if (i != 0) std::cout << " <-> ";
            std::cout << link->endpoints[i];
        }
        std::cout << " state=" << (link->traversable ? "traversable" : "blocked")
                  << (link->locked ? " locked" : "") << "\n";
    }
}

void printEntities(const WorldService& service) {
    for (const auto& [id, entity] : service.state().entities) {
        std::cout << "  [" << toString(entity.lifecycle) << "] " << id
                  << " kind=" << entity.kind << "\n";
    }
}

void printEvents(const WorldService& service, std::size_t count) {
    const auto& events = service.state().events;
    const std::size_t start = events.size() > count ? events.size() - count : 0;
    for (std::size_t i = start; i < events.size(); ++i) {
        const auto& event = events[i];
        std::cout << event.sequence << " " << event.at.toString() << " " << event.type << " "
                  << event.actor << " -> " << event.target << " | " << event.detail << "\n";
    }
}

bool execute(WorldService& service, const std::string& line) {
    std::istringstream input(line);
    std::string command;
    input >> command;
    if (command.empty() || command[0] == '#') return true;
    if (command == "help") {
        printHelp();
    } else if (command == "look") {
        printLook(service);
    } else if (command == "map") {
        printMap(service);
    } else if (command == "entities") {
        printEntities(service);
    } else if (command == "entity-create" || command == "entity-activate" ||
               command == "entity-dormant" || command == "entity-destroy") {
        std::string id;
        std::string detail;
        input >> id;
        if (command == "entity-create") input >> detail;
        const std::string kind = command == "entity-create" ? "entity_create"
                              : command == "entity-activate" ? "entity_activate"
                              : command == "entity-dormant" ? "entity_dormant" : "entity_destroy";
        const auto result = service.submit(Command{"text-" + kind + "-" + std::to_string(service.state().events.size() + 1),
                                                    kind, "player", id, {}, detail});
        std::cout << (result.accepted ? kind + " ok: " + id : result.code + ": " + result.message) << "\n";
    } else if (command == "talk") {
        std::string npc;
        input >> npc;
        std::string text;
        std::getline(input, text);
        if (!text.empty() && text.front() == ' ') text.erase(0, 1);
        const auto turn = service.converse(npc, text, "text-turn-" + std::to_string(service.state().events.size() + 1));
        std::cout << (turn.accepted ? turn.dialogue : turn.error) << " [provider=" << turn.provider << "]\n";
    } else if (command == "advance" || command == "wait") {
        std::int64_t minutes = 0;
        input >> minutes;
        service.advanceMinutes(command == "wait" ? minutes * kMinutesPerHour : minutes);
        printLook(service);
    } else if (command == "day") {
        service.advanceMinutes(kMinutesPerDay);
        printLook(service);
    } else if (command == "events") {
        std::size_t count = 12;
        input >> count;
        if (!input) count = 12;
        printEvents(service, count);
    } else if (command == "promise") {
        std::string from;
        std::string to;
        int days = 1;
        input >> from >> to >> days;
        std::string description;
        std::getline(input, description);
        if (!description.empty() && description.front() == ' ') description.erase(0, 1);
        const auto result = service.createPromise(from, to, description, days,
                                                  "text-promise-" + std::to_string(service.state().events.size() + 1));
        std::cout << (result.accepted ? "created " + result.message : result.code + ": " + result.message) << "\n";
    } else if (command == "accept" || command == "refuse" || command == "renegotiate" || command == "fulfill") {
        std::string id;
        std::string actor;
        input >> id >> actor;
        PromiseStatus status = PromiseStatus::Accepted;
        if (command == "refuse") status = PromiseStatus::Refused;
        if (command == "renegotiate") status = PromiseStatus::Renegotiated;
        if (command == "fulfill") status = PromiseStatus::Fulfilled;
        const auto result = service.decidePromise(id, status, actor,
                                                  "text-promise-decision-" + std::to_string(service.state().events.size() + 1));
        std::cout << (result.accepted ? result.message : result.code + ": " + result.message) << "\n";
    } else if (command == "save" || command == "replay") {
        std::string path;
        input >> path;
        if (command == "save") service.saveSnapshot(path);
        else service.writeReplay(path);
        std::cout << "written " << path << "\n";
    } else if (command == "quit" || command == "exit") {
        return false;
    } else {
        std::cout << "Unknown command. Type help.\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    auto service = WorldService::load(ContentPackLoader::defaultRoot(), providerFromEnvironment());
    std::string scriptPath;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--script") scriptPath = argv[index + 1];
    }
    printHelp();
    std::istream* input = &std::cin;
    std::ifstream script;
    if (!scriptPath.empty()) {
        script.open(scriptPath);
        input = &script;
    }
    std::string line;
    while (scriptPath.empty() ? (std::cout << "> " && static_cast<bool>(std::getline(*input, line)))
                              : static_cast<bool>(std::getline(*input, line))) {
        if (!execute(service, line)) break;
    }
    return 0;
}
