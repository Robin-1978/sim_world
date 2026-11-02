#include "world/world_service.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace tide_marks {

namespace {

template <typename T>
T jsonValue(const Json& value, const char* key, T fallback) {
    if (!value.contains(key) || value.at(key).is_null()) return fallback;
    return value.at(key).get<T>();
}

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::ostringstream output;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) output << separator;
        output << values[i];
    }
    return output.str();
}

Json memoryJson(const Memory& memory) {
    return Json{{"kind", memory.kind}, {"text", memory.text}, {"minute", memory.at.minute}, {"confidence", memory.confidence}};
}
Memory memoryFromJson(const Json& json) {
    return Memory{jsonValue<std::string>(json, "kind", "fact"), jsonValue<std::string>(json, "text", ""),
                  GameTime{jsonValue<std::int64_t>(json, "minute", 0)}, jsonValue<double>(json, "confidence", 1.0)};
}
Json npcDefinitionJson(const NpcDefinition& definition) {
    return Json{{"id", definition.id}, {"name", definition.name}, {"role", definition.role}, {"home", definition.home},
                {"workplace", definition.workplace}, {"public_goal", definition.publicGoal}, {"hidden_goal", definition.hiddenGoal},
                {"boundary", definition.boundary}, {"voice", definition.voice}};
}
NpcDefinition npcDefinitionFromJson(const Json& json) {
    return NpcDefinition{jsonValue<std::string>(json, "id", ""), jsonValue<std::string>(json, "name", ""),
                         jsonValue<std::string>(json, "role", ""), jsonValue<std::string>(json, "home", ""),
                         jsonValue<std::string>(json, "workplace", ""), jsonValue<std::string>(json, "public_goal", ""),
                         jsonValue<std::string>(json, "hidden_goal", ""), jsonValue<std::string>(json, "boundary", ""),
                         jsonValue<std::string>(json, "voice", "")};
}
Json eventJson(const WorldEvent& event) {
    return Json{{"sequence", event.sequence}, {"minute", event.at.minute}, {"type", event.type}, {"actor", event.actor},
                {"target", event.target}, {"detail", event.detail}, {"world_revision", event.worldRevision}, {"data", event.data}};
}
WorldEvent eventFromJson(const Json& json) {
    return WorldEvent{jsonValue<std::uint64_t>(json, "sequence", 0), GameTime{jsonValue<std::int64_t>(json, "minute", 0)},
                      jsonValue<std::string>(json, "type", "unknown"), jsonValue<std::string>(json, "actor", ""),
                      jsonValue<std::string>(json, "target", ""), jsonValue<std::string>(json, "detail", ""),
                      jsonValue<std::uint64_t>(json, "world_revision", 0), json.value("data", Json::object())};
}

}  // namespace
WorldService::WorldService(std::unique_ptr<IProvider> provider)
    : provider_(provider ? std::move(provider) : std::make_unique<MockProvider>()) {}

WorldService WorldService::createVillage(std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.state_.contentVersion = "village-p0.1";
    service.state_.now = GameTime::fromDayHour(1, 6);
    service.state_.village = VillageState{};
    service.state_.npcs.emplace("mira", NpcState{
        NpcDefinition{"mira", "Mira", "village coordinator", "mira_home", "granary",
                       "Keep grain rationing fair", "Protect an undocumented family member",
                       "Never endanger a child to satisfy an official order", "measured and warm"},
        "granary", "morning ledger", 0, 100, 0.25, RelationshipAxes{},
        {Memory{"fact", "The village depends on the granary ledger.", service.state_.now, 1.0}}, {}, 0});
    service.state_.npcs.emplace("lorn", NpcState{
        NpcDefinition{"lorn", "Lorn", "ferryman", "lorn_house", "ferry_dock",
                       "Keep the crossing open", "Repay Mira while hiding a damaged skiff",
                       "Will not abandon passengers on the water", "dry, joking, practical"},
        "ferry_dock", "first crossing", 0, 72, 0.3, RelationshipAxes{},
        {Memory{"episode", "The east ferry is taking on water.", service.state_.now, 0.95}}, {}, 0});
    service.state_.npcs.emplace("eve", NpcState{
        NpcDefinition{"eve", "Eve", "archivist", "archive_room", "archive",
                       "Preserve the village records", "Find proof of the hidden family line",
                       "Will not burn a record to win an argument", "precise, guarded, curious"},
        "archive", "morning index", 0, 80, 0.35, RelationshipAxes{},
        {Memory{"fact", "Some names were removed from the old tide registry.", service.state_.now, 0.9}}, {}, 0});
    service.addEvent("village.initialized", "world", "village", "Three NPC village slice ready",
                     Json{{"npcs", Json::array({"mira", "lorn", "eve"})}});
    return service;
}

bool WorldService::hasNpc(const std::string& npcId) const {
    return state_.npcs.find(npcId) != state_.npcs.end();
}

DecisionContext WorldService::decisionContext(const std::string& npcId) const {
    const auto found = state_.npcs.find(npcId);
    if (found == state_.npcs.end()) return {};
    const auto& npc = found->second;
    DecisionContext context;
    context.npcId = npcId;
    context.now = state_.now;
    context.worldRevision = state_.revision;
    context.npcStateVersion = npc.stateVersion;
    context.perception = "At " + npc.location + ", it is " + state_.now.toString() + ". Grain=" +
                         std::to_string(state_.village.grain) + ", boat=" +
                         std::to_string(state_.village.boatIntegrity) + ", records=" +
                         std::to_string(state_.village.recordsIntegrity) + ".";
    std::vector<std::string> memories;
    for (const auto& memory : npc.memories) memories.push_back(memory.text);
    context.memories = join(memories, " | ");
    context.character = npc.definition.name + ": " + npc.definition.role + "; public goal=" +
                        npc.definition.publicGoal + "; hidden goal=" + npc.definition.hiddenGoal +
                        "; boundary=" + npc.definition.boundary + "; pressure=" +
                        std::to_string(npc.pressure);
    if (npcId == "mira") {
        context.candidates = {{"offer_ration", "ration", "offer a fair ration"},
                              {"protect_secret", "family", "protect the family secret"},
                              {"explain_ration", "ration", "explain the ledger",},
                              {"rest", "daily", "return home"}};
    } else if (npcId == "lorn") {
        context.candidates = {{"repair_ferry", "ferry", "repair the ferry"},
                              {"offer_crossing", "route", "offer a safe crossing"},
                              {"ask_debt", "debt", "ask Mira for help"},
                              {"rest", "daily", "return home"}};
    } else {
        context.candidates = {{"share_record", "archive", "share a verified record"},
                              {"withhold_record", "archive", "protect the archive"},
                              {"reveal_clue", "family", "reveal a cautious clue"},
                              {"rest", "daily", "return home"}};
    }
    // Utility ranking chooses the long-term intent deterministically. The
    // provider may still choose any authored candidate after seeing this list.
    const auto ranked = GoalSelector::rank(npc, state_.village, state_.now);
    std::vector<ActionCandidate> ordered;
    for (const auto& choice : ranked) {
        const auto candidate = std::find_if(context.candidates.begin(), context.candidates.end(),
                                            [&choice](const auto& item) { return item.actionId == choice.actionId; });
        if (candidate != context.candidates.end()) ordered.push_back(*candidate);
    }
    for (const auto& candidate : context.candidates) {
        if (std::none_of(ordered.begin(), ordered.end(), [&candidate](const auto& item) {
                return item.actionId == candidate.actionId;
            })) {
            ordered.push_back(candidate);
        }
    }
    context.candidates = std::move(ordered);
    return context;
}

void WorldService::advanceMinutes(std::int64_t minutes) {
    if (minutes <= 0) return;
    const auto target = state_.now.minute + minutes;
    while (state_.now.minute < target) {
        ++state_.now.minute;
        if (state_.now.minute % 60 == 0) processHourBoundary(state_.now.day(), state_.now.hour());
        if (state_.now.minute % 1440 == 0) processDayBoundary(state_.now.day());
        applyPromiseDeadline();
    }
}

void WorldService::processHourBoundary(int day, int hour) {
    auto& mira = state_.npcs.at("mira");
    auto& lorn = state_.npcs.at("lorn");
    auto& eve = state_.npcs.at("eve");
    mira.location = (hour >= 6 && hour < 16) ? (hour < 12 ? "granary" : "village_square") : "mira_home";
    lorn.location = (hour >= 6 && hour < 18) ? "ferry_dock" : "lorn_house";
    eve.location = (hour >= 8 && hour < 19) ? "archive" : "archive_room";
    mira.scheduleLabel = mira.location;
    lorn.scheduleLabel = lorn.location;
    eve.scheduleLabel = eve.location;
    for (auto* npc : {&mira, &lorn, &eve}) ++npc->stateVersion;
    if (hour == 8) {
        const int ration = std::min(5, state_.village.grain);
        state_.village.grain -= ration;
        mira.resource = state_.village.grain;
        mira.pressure = state_.village.grain < 70 ? 0.8 : 0.3;
        addEvent("life.ration", "mira", "village", "Mira records the morning ration",
                 Json{{"grain_used", ration}, {"grain_remaining", state_.village.grain}, {"day", day}});
    }
    if (hour == 10 && state_.village.boatIntegrity < 100) {
        if (state_.village.grain >= 5) state_.village.grain -= 5;
        state_.village.boatIntegrity = std::min(100, state_.village.boatIntegrity + 8);
        lorn.resource = state_.village.boatIntegrity;
        addEvent("life.repair", "lorn", "ferry", "Lorn patches the ferry before the next crossing",
                 Json{{"boat_integrity", state_.village.boatIntegrity}});
    }
    if (hour == 14) {
        state_.village.recordsIntegrity = std::min(100, state_.village.recordsIntegrity + 2);
        eve.resource = state_.village.recordsIntegrity;
        addEvent("life.archive", "eve", "archive", "Eve indexes the tide registry",
                 Json{{"records_integrity", state_.village.recordsIntegrity}});
    }
}

void WorldService::processDayBoundary(int day) {
    auto& mira = state_.npcs.at("mira");
    auto& lorn = state_.npcs.at("lorn");
    auto& eve = state_.npcs.at("eve");
    state_.village.boatIntegrity = std::max(0, state_.village.boatIntegrity - 3);
    mira.resource = state_.village.grain;
    lorn.resource = state_.village.boatIntegrity;
    eve.resource = state_.village.recordsIntegrity;
    if (state_.village.grain < 85) {
        mira.goalStage = std::max(mira.goalStage, 1);
        mira.pressure = std::min(1.0, mira.pressure + 0.12);
    }
    if (state_.village.boatIntegrity < 70) {
        lorn.goalStage = std::max(lorn.goalStage, 1);
        lorn.pressure = std::min(1.0, lorn.pressure + 0.1);
    }
    if (state_.village.recordsIntegrity > 80 || day >= 2) {
        eve.goalStage = std::max(eve.goalStage, 1);
        eve.pressure = std::min(1.0, eve.pressure + 0.08);
    }
    for (auto* npc : {&mira, &lorn, &eve}) {
        ++npc->stateVersion;
        npc->memories.push_back(Memory{"episode", "A day passed while the village kept working.", state_.now, 1.0});
    }
    addEvent("life.day", "world", "village", "The village completes another day",
             Json{{"day", day}, {"grain", state_.village.grain}, {"boat_integrity", state_.village.boatIntegrity},
                  {"records_integrity", state_.village.recordsIntegrity}});
}

void WorldService::applyPromiseDeadline() {
    for (auto& [id, promise] : state_.promises) {
        if ((promise.status == PromiseStatus::Accepted || promise.status == PromiseStatus::Renegotiated) &&
            state_.now.minute >= promise.due.minute) {
            promise.status = PromiseStatus::Broken;
            ++promise.version;
            auto from = state_.npcs.find(promise.from);
            auto to = state_.npcs.find(promise.to);
            if (from != state_.npcs.end()) {
                from->second.pressure = std::min(1.0, from->second.pressure + 0.2);
                from->second.playerRelation.trust = std::max(0.0, from->second.playerRelation.trust - 0.08);
                ++from->second.stateVersion;
            }
            if (to != state_.npcs.end()) {
                to->second.pressure = std::min(1.0, to->second.pressure + 0.2);
                to->second.playerRelation.respect = std::max(0.0, to->second.playerRelation.respect - 0.08);
                ++to->second.stateVersion;
            }
            addEvent("promise.broken", promise.from, promise.to, "A promise reached its deadline", Json{{"promise_id", id}});
        }
    }
}

CommandResult WorldService::reject(std::string code, std::string message) const {
    return CommandResult{false, std::move(code), std::move(message), std::nullopt};
}

CommandResult WorldService::submit(const Command& command) {
    if (command.expectedWorldRevision != 0 && command.expectedWorldRevision != state_.revision) {
        return reject("state_conflict", "The world changed before this command was committed.");
    }
    if (command.expectedNpcStateVersion != 0 && hasNpc(command.actor) &&
        command.expectedNpcStateVersion != state_.npcs.at(command.actor).stateVersion) {
        return reject("state_conflict", "The NPC changed before this command was committed.");
    }
    if (!command.turnId.empty() && state_.processedTurnIds.contains(command.turnId)) {
        return reject("duplicate_turn", "The turn was already applied.");
    }
    if (command.kind == "advance") {
        try {
            const auto minutes = std::stoll(command.detail);
            advanceMinutes(minutes);
        } catch (...) {
            return reject("invalid_duration", "Advance duration must be minutes.");
        }
        if (!command.turnId.empty()) state_.processedTurnIds.insert(command.turnId);
        return CommandResult{true, "ok", "World advanced.", std::nullopt};
    }
    if (command.kind == "promise_accept" || command.kind == "promise_refuse" ||
        command.kind == "promise_renegotiate" || command.kind == "promise_fulfill" || command.kind == "promise_break") {
        PromiseStatus status = PromiseStatus::Accepted;
        if (command.kind == "promise_refuse") status = PromiseStatus::Refused;
        if (command.kind == "promise_renegotiate") status = PromiseStatus::Renegotiated;
        if (command.kind == "promise_fulfill") status = PromiseStatus::Fulfilled;
        if (command.kind == "promise_break") status = PromiseStatus::Broken;
        return decidePromise(command.promiseId, status, command.actor, command.turnId, command.detail);
    }
    return reject("unknown_command", "The command is not part of the village action vocabulary.");
}

CommandResult WorldService::createPromise(const std::string& from, const std::string& to,
                                           const std::string& description, int dueInDays,
                                           const std::string& turnId) {
    const bool validFrom = from == "player" || hasNpc(from);
    const bool validTo = to == "player" || hasNpc(to);
    if (!validFrom || !validTo || from == to) return reject("unknown_actor", "Both promise parties must be distinct.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    const std::string id = "promise-" + std::to_string(state_.promises.size() + 1);
    Promise promise{id, from, to, description,
                    GameTime{state_.now.minute + static_cast<std::int64_t>(std::max(1, dueInDays)) * 1440},
                    PromiseStatus::Proposed, 0};
    state_.promises.emplace(id, promise);
    if (hasNpc(from)) state_.npcs.at(from).openPromises.push_back(id);
    if (hasNpc(to)) state_.npcs.at(to).openPromises.push_back(id);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    addEvent("promise.created", from, to, description, Json{{"promise_id", id}, {"due", promise.due.minute}});
    return CommandResult{true, "ok", id, state_.events.back()};
}

CommandResult WorldService::decidePromise(const std::string& promiseId, PromiseStatus decision,
                                          const std::string& actor, const std::string& turnId,
                                          const std::string& detail) {
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    auto found = state_.promises.find(promiseId);
    if (found == state_.promises.end()) return reject("unknown_promise", "That promise does not exist.");
    auto& promise = found->second;
    if (actor != "player" && actor != promise.from && actor != promise.to) {
        return reject("unauthorized", "Only a promise party can change it.");
    }
    if (promise.status == PromiseStatus::Broken || promise.status == PromiseStatus::Refused || promise.status == PromiseStatus::Fulfilled) {
        return reject("stale_promise", "That promise is already closed.");
    }
    if (decision == PromiseStatus::Renegotiated) {
        promise.due.minute += 1440;
        promise.status = PromiseStatus::Renegotiated;
    } else {
        promise.status = decision;
    }
    ++promise.version;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    if (auto npc = state_.npcs.find(actor); npc != state_.npcs.end()) {
        ++npc->second.stateVersion;
        npc->second.memories.push_back(Memory{"promise", detail.empty() ? "A promise changed status." : detail, state_.now, 1.0});
        if (decision == PromiseStatus::Refused || decision == PromiseStatus::Broken) {
            npc->second.playerRelation.trust = std::max(0.0, npc->second.playerRelation.trust - 0.04);
        } else {
            npc->second.playerRelation.trust = std::min(1.0, npc->second.playerRelation.trust + 0.03);
        }
    }
    addEvent("promise." + toString(decision), actor, promiseId, detail.empty() ? "Promise status changed" : detail,
             Json{{"promise_id", promiseId}, {"status", toString(decision)}, {"version", promise.version}});
    return CommandResult{true, "ok", toString(decision), state_.events.back()};
}

DialogueTurn WorldService::converse(const std::string& npcId, const std::string& playerInput,
                                    const std::string& turnId) {
    DialogueTurn result;
    result.npcId = npcId;
    result.provider = provider_->name();
    if (!hasNpc(npcId)) {
        result.error = "unknown_npc";
        return result;
    }
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        result.error = "duplicate_turn";
        return result;
    }
    const auto context = decisionContext(npcId);
    auto response = provider_->decide(context, playerInput);
    if (!response.ok) {
        // Rule fallback keeps the world playable when the local server is down.
        const std::string providerError = response.error;
        response = MockProvider{}.decide(context, playerInput);
        result.provider += "+fallback";
        result.error = "provider_fallback:" + providerError;
    }
    const auto validation = ActionValidator::validate(context, response.actionId);
    if (!validation.valid) {
        result.error = validation.code;
        return result;
    }
    if (response.actionId == "offer_ration") {
        if (state_.village.grain < 5) {
            result.error = "insufficient_grain";
            return result;
        }
        state_.village.grain -= 5;
        state_.village.reputation += 1;
    } else if (response.actionId == "repair_ferry") {
        state_.village.boatIntegrity = std::min(100, state_.village.boatIntegrity + 10);
    } else if (response.actionId == "share_record") {
        state_.village.recordsIntegrity = std::max(0, state_.village.recordsIntegrity - 1);
        state_.npcs.at(npcId).goalStage = std::max(1, state_.npcs.at(npcId).goalStage);
    } else if (response.actionId == "protect_secret" || response.actionId == "reveal_clue") {
        state_.npcs.at(npcId).goalStage = std::max(1, state_.npcs.at(npcId).goalStage);
    }
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    ++state_.npcs.at(npcId).stateVersion;
    state_.npcs.at(npcId).memories.push_back(Memory{"conversation", playerInput, state_.now, 1.0});
    addEvent("dialogue.turn", npcId, "player", response.dialogue,
             Json{{"turn_id", turnId}, {"action_id", response.actionId}, {"intent", response.intent},
                  {"motivation", response.motivation}, {"provider", result.provider}});
    result.accepted = true;
    result.actionId = response.actionId;
    result.intent = response.intent;
    result.motivation = response.motivation;
    result.dialogue = response.dialogue;
    result.event = state_.events.back();
    return result;
}

void WorldService::addEvent(std::string type, std::string actor, std::string target,
                            std::string detail, Json data) {
    ++state_.revision;
    state_.events.push_back(WorldEvent{state_.events.size() + 1, state_.now, std::move(type), std::move(actor),
                                       std::move(target), std::move(detail), state_.revision, std::move(data)});
}

Json toJson(const WorldState& state) {
    Json npcs = Json::object();
    for (const auto& [id, npc] : state.npcs) {
        Json memories = Json::array();
        for (const auto& memory : npc.memories) memories.push_back(memoryJson(memory));
        npcs[id] = Json{{"definition", npcDefinitionJson(npc.definition)}, {"location", npc.location},
                        {"schedule_label", npc.scheduleLabel}, {"goal_stage", npc.goalStage},
                        {"resource", npc.resource}, {"pressure", npc.pressure},
                        {"relation", {{"trust", npc.playerRelation.trust}, {"respect", npc.playerRelation.respect},
                                       {"fear", npc.playerRelation.fear}, {"owed", npc.playerRelation.owed}}},
                        {"memories", memories}, {"open_promises", npc.openPromises},
                        {"state_version", npc.stateVersion}};
    }
    Json promises = Json::object();
    for (const auto& [id, promise] : state.promises) {
        promises[id] = Json{{"id", promise.id}, {"from", promise.from}, {"to", promise.to},
                            {"description", promise.description}, {"due", promise.due.minute},
                            {"status", toString(promise.status)}, {"version", promise.version}};
    }
    Json events = Json::array();
    for (const auto& event : state.events) events.push_back(eventJson(event));
    return Json{{"content_version", state.contentVersion}, {"now", state.now.minute}, {"revision", state.revision},
                {"village", {{"grain", state.village.grain}, {"boat_integrity", state.village.boatIntegrity},
                              {"records_integrity", state.village.recordsIntegrity}, {"lantern_lit", state.village.lanternLit},
                              {"reputation", state.village.reputation}}},
                {"npcs", npcs}, {"promises", promises}, {"events", events},
                {"processed_turn_ids", state.processedTurnIds}};
}

WorldState worldStateFromJson(const Json& json) {
    WorldState state;
    state.contentVersion = jsonValue<std::string>(json, "content_version", "village-p0.1");
    state.now = GameTime{jsonValue<std::int64_t>(json, "now", 0)};
    state.revision = jsonValue<std::uint64_t>(json, "revision", 0);
    const auto village = json.value("village", Json::object());
    state.village.grain = jsonValue<int>(village, "grain", 100);
    state.village.boatIntegrity = jsonValue<int>(village, "boat_integrity", 72);
    state.village.recordsIntegrity = jsonValue<int>(village, "records_integrity", 80);
    state.village.lanternLit = jsonValue<bool>(village, "lantern_lit", true);
    state.village.reputation = jsonValue<int>(village, "reputation", 0);
    const auto npcs = json.value("npcs", Json::object());
    for (auto it = npcs.begin(); it != npcs.end(); ++it) {
        const auto value = it.value();
        NpcState npc;
        npc.definition = npcDefinitionFromJson(value.value("definition", Json::object()));
        npc.location = jsonValue<std::string>(value, "location", "");
        npc.scheduleLabel = jsonValue<std::string>(value, "schedule_label", "");
        npc.goalStage = jsonValue<int>(value, "goal_stage", 0);
        npc.resource = jsonValue<int>(value, "resource", 0);
        npc.pressure = jsonValue<double>(value, "pressure", 0.2);
        const auto relation = value.value("relation", Json::object());
        npc.playerRelation = RelationshipAxes{jsonValue<double>(relation, "trust", 0.35),
                                              jsonValue<double>(relation, "respect", 0.35),
                                              jsonValue<double>(relation, "fear", 0.0),
                                              jsonValue<double>(relation, "owed", 0.0)};
        for (const auto& memory : value.value("memories", Json::array())) npc.memories.push_back(memoryFromJson(memory));
        npc.openPromises = value.value("open_promises", std::vector<std::string>{});
        npc.stateVersion = jsonValue<std::uint64_t>(value, "state_version", 0);
        state.npcs[it.key()] = std::move(npc);
    }
    const auto promises = json.value("promises", Json::object());
    for (auto it = promises.begin(); it != promises.end(); ++it) {
        const auto value = it.value();
        state.promises[it.key()] = Promise{jsonValue<std::string>(value, "id", it.key()),
                                           jsonValue<std::string>(value, "from", ""),
                                           jsonValue<std::string>(value, "to", ""),
                                           jsonValue<std::string>(value, "description", ""),
                                           GameTime{jsonValue<std::int64_t>(value, "due", 0)},
                                           promiseStatusFromString(jsonValue<std::string>(value, "status", "proposed")),
                                           jsonValue<int>(value, "version", 0)};
    }
    for (const auto& event : json.value("events", Json::array())) state.events.push_back(eventFromJson(event));
    for (const auto& turn : json.value("processed_turn_ids", std::vector<std::string>{})) state.processedTurnIds.insert(turn);
    return state;
}

Json WorldService::snapshotJson() const { return toJson(state_); }

void WorldService::saveSnapshot(const std::string& path) const {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot write snapshot: " + path);
    output << snapshotJson().dump(2) << '\n';
}

void WorldService::writeReplay(const std::string& path) const {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot write replay: " + path);
    output << Json{{"kind", "snapshot"}, {"state", snapshotJson()}}.dump() << '\n';
    for (const auto& event : state_.events) output << Json{{"kind", "event"}, {"event", eventJson(event)}}.dump() << '\n';
}

WorldService WorldService::fromSnapshot(const Json& json, std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.state_ = worldStateFromJson(json.contains("state") ? json.at("state") : json);
    return service;
}

WorldService WorldService::replay(const std::string& path, std::unique_ptr<IProvider> provider) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read replay: " + path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto json = Json::parse(line);
        if (json.value("kind", "") == "snapshot") return fromSnapshot(json.at("state"), std::move(provider));
    }
    throw std::runtime_error("replay has no snapshot");
}

std::string summarize(const WorldState& state) {
    std::ostringstream output;
    output << state.now.toString() << "; grain=" << state.village.grain
           << "; boat=" << state.village.boatIntegrity << "; records=" << state.village.recordsIntegrity
           << "; revision=" << state.revision;
    for (const auto& [id, npc] : state.npcs) {
        output << "; " << id << "@" << npc.location << ":stage=" << npc.goalStage
               << ":pressure=" << npc.pressure;
    }
    return output.str();
}


}  // namespace tide_marks
