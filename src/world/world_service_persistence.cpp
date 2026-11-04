#include "world/world_service.hpp"
#include "world/world_state_json.hpp"
#include "evolution/evolution.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace tide_marks {

namespace {
Json replayFactJson(const WorldState& state, std::size_t eventSequence) {
    Json json = worldStateToJson(state);
    json.erase("events");
    json["event_sequence"] = eventSequence;
    return json;
}

std::string stateDigest(const Json& state) {
    std::uint64_t hash = 1469598103934665603ULL;
    const std::string canonical = state.dump();
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream digest;
    digest << "fnv1a64:" << std::hex << hash;
    return digest.str();
}

void validateEventHistory(const WorldState& state) {
    std::map<std::string, std::uint64_t> correlationRevisions;
    std::uint64_t previousRevision = 0;
    for (std::size_t index = 0; index < state.events.size(); ++index) {
        const auto& event = state.events[index];
        if (event.schemaVersion != kWorldEventSchemaVersion || event.sequence != index + 1 ||
            event.eventId != "event:" + std::to_string(event.sequence)) {
            throw std::runtime_error("snapshot contains an invalid event identity");
        }
        if (event.contentVersion.empty() ||
            event.simulationVersion != kSimulationVersion) {
            throw std::runtime_error("snapshot event has incomplete version provenance");
        }
        if (event.worldRevision == 0 || event.worldRevision > state.revision ||
            event.worldRevision < previousRevision) {
            throw std::runtime_error("snapshot event has an invalid world revision");
        }
        previousRevision = event.worldRevision;
        const std::string prefix = "correlation:";
        if (!event.correlationId.starts_with(prefix) || event.correlationId.size() == prefix.size()) {
            throw std::runtime_error("snapshot event has an invalid correlation id");
        }
        const auto [revision, inserted] =
            correlationRevisions.emplace(event.correlationId, event.worldRevision);
        if (!inserted && revision->second != event.worldRevision) {
            throw std::runtime_error("snapshot transaction combines events from different world revisions");
        }
        if (event.causationSequence == 0) continue;
        if (event.causationSequence >= event.sequence) {
            throw std::runtime_error("snapshot event causation does not precede its effect");
        }
        const auto& cause = state.events.at(static_cast<std::size_t>(event.causationSequence - 1));
        if (cause.correlationId != event.correlationId) {
            throw std::runtime_error("snapshot event causation crosses correlation chains");
        }
    }
}
}

Json WorldService::snapshotJson() const { return worldStateToJson(state_); }

void WorldService::saveSnapshot(const std::string& path) const {
    persistence_->saveSnapshot(path, snapshotJson());
}

void WorldService::writeReplay(const std::string& path) const {
    std::vector<Json> lines;
    Json baseline = replayBaseline_.is_object() && !replayBaseline_.empty() ? replayBaseline_ : snapshotJson();
    const auto baselineRevision = baseline.at("world_revision").get<std::uint64_t>();
    Json baselineFact = baseline;
    baselineFact.erase("events");
    const Json currentFact = replayFactJson(state_, state_.events.size());
    const auto patch = Json::diff(baselineFact, currentFact);
    const bool hasChanges = !patch.empty() || state_.events.size() > replayBaselineEvents_.size();
    lines.push_back(Json{{"kind", "replay_header"}, {"schema_version", 2}, {"state", baseline},
                         {"transaction_count", hasChanges ? 1U : 0U}});
    if (hasChanges) {
        Json events = Json::array();
        for (std::size_t index = replayBaselineEvents_.size(); index < state_.events.size(); ++index) {
            events.push_back(worldEventToJson(state_.events[index]));
        }
        lines.push_back(Json{{"kind", "transaction"}, {"base_world_revision", baselineRevision},
                             {"world_revision", state_.revision}, {"patch", patch}, {"events", events},
                             {"state_digest", stateDigest(currentFact)}});
    }
    persistence_->saveReplay(path, lines);
}

void WorldService::saveDurableCommit(const std::string& path) const {
    std::vector<Json> lines;
    Json baseline = replayBaseline_.is_object() && !replayBaseline_.empty() ? replayBaseline_ : snapshotJson();
    const auto baselineRevision = baseline.at("world_revision").get<std::uint64_t>();
    Json baselineFact = baseline;
    baselineFact.erase("events");
    const Json currentFact = replayFactJson(state_, state_.events.size());
    const auto patch = Json::diff(baselineFact, currentFact);
    const bool hasChanges = !patch.empty() || state_.events.size() > replayBaselineEvents_.size();
    lines.push_back(Json{{"kind", "replay_header"}, {"schema_version", 2}, {"state", baseline},
                         {"transaction_count", hasChanges ? 1U : 0U}});
    if (hasChanges) {
        Json events = Json::array();
        for (std::size_t index = replayBaselineEvents_.size(); index < state_.events.size(); ++index) {
            events.push_back(worldEventToJson(state_.events[index]));
        }
        lines.push_back(Json{{"kind", "transaction"}, {"base_world_revision", baselineRevision},
                             {"world_revision", state_.revision}, {"patch", patch}, {"events", events},
                             {"state_digest", stateDigest(currentFact)}});
    }
    persistence_->saveDurableCommit(path, snapshotJson(), lines);
}

void WorldService::configureDurableCheckpoint(DurableCheckpointPolicy policy) {
    if (policy.mode == DurableCheckpointMode::Disabled) {
        durableCheckpointPolicy_ = std::move(policy);
        nextDurableCheckpointMinute_ = 0;
        return;
    }
    if (policy.path.empty()) throw std::invalid_argument("durable checkpoint policy requires a path");
    if (policy.mode == DurableCheckpointMode::PeriodicMinutes && policy.intervalMinutes <= 0) {
        throw std::invalid_argument("periodic durable checkpoint interval must be positive");
    }
    durableCheckpointPolicy_ = std::move(policy);
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::PeriodicMinutes) {
        nextDurableCheckpointMinute_ = state_.now.minute + durableCheckpointPolicy_.intervalMinutes;
    } else {
        nextDurableCheckpointMinute_ = 0;
    }
}

void WorldService::maybeSaveDurableCheckpoint() {
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::Disabled ||
        durableCheckpointPolicy_.path.empty()) return;
    bool due = false;
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::PeriodicMinutes) {
        due = state_.now.minute >= nextDurableCheckpointMinute_;
    } else if (durableCheckpointPolicy_.mode == DurableCheckpointMode::DayBoundary) {
        due = state_.now.minute > 0 && state_.now.minute % kMinutesPerDay == 0 &&
              state_.revision != lastDurableCheckpointRevision_;
    }
    if (!due) return;
    saveDurableCommit(durableCheckpointPolicy_.path);
    lastDurableCheckpointRevision_ = state_.revision;
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::PeriodicMinutes) {
        do {
            nextDurableCheckpointMinute_ += durableCheckpointPolicy_.intervalMinutes;
        } while (nextDurableCheckpointMinute_ <= state_.now.minute);
    }
}

WorldService WorldService::fromContentSnapshot(const WorldContentPack& content, const Json& json,
                                                     std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.baseContent_ = content;
    service.content_ = content;
    const auto& snapshot = json.contains("state") ? json.at("state") : json;
    if (!snapshot.is_object() || !snapshot.contains("save_schema_version") ||
        !snapshot.at("save_schema_version").is_number_integer() ||
        snapshot.at("save_schema_version").get<int>() != kSnapshotSchemaVersion) {
        throw std::runtime_error("unsupported snapshot schema; expected save_schema_version=" +
                                 std::to_string(kSnapshotSchemaVersion));
    }
    if (!snapshot.contains("simulation_version") || !snapshot.at("simulation_version").is_string() ||
        snapshot.at("simulation_version") != kSimulationVersion) {
        throw std::runtime_error("unsupported snapshot simulation version; expected " +
                                 std::string{kSimulationVersion});
    }
    service.state_ = deserializeWorldState(snapshot);
    if (snapshot.value("event_sequence", static_cast<std::size_t>(0)) != service.state_.events.size()) {
        throw std::runtime_error("snapshot event_sequence does not match its event history");
    }
    validateEventHistory(service.state_);
    if (!service.state_.activeOverlays.empty()) {
        auto rebuilt = content;
        for (const auto& overlay : service.state_.activeOverlays) {
            EvolutionProposal replay{overlay.proposalId, "declarative_capability", "snapshot",
                                     overlay.baseContentVersion, 0, overlay.operations,
                                     EvolutionProposalStatus::Approved, {}};
            const auto applied = EvolutionManager::applyOverlay(rebuilt, replay);
            if (!applied.valid) {
                throw std::runtime_error("cannot restore overlay " + overlay.proposalId + ": " + applied.message);
            }
            if (rebuilt.contentVersion != overlay.overlayVersion) {
                throw std::runtime_error("restored overlay version does not match its activation record: " +
                                         overlay.proposalId);
            }
        }
        service.content_ = std::move(rebuilt);
    }
    for (const auto& [id, npc] : service.state_.npcs) {
        if (npc.definitionId.empty() || service.authoredNpc(npc.definitionId) == nullptr) {
            throw std::runtime_error("snapshot references unknown npc definition: " + npc.definitionId);
        }
    }
    for (const auto& [id, group] : service.state_.socialGroups) {
        if (std::none_of(content.socialGroups.begin(), content.socialGroups.end(),
                         [&id](const auto& defined) { return defined.id == id; })) {
            throw std::runtime_error("snapshot references unknown social group: " + id);
        }
        for (const auto& [npcId, role] : group.members) {
            (void)role;
            if (!service.state_.npcs.contains(npcId)) {
                throw std::runtime_error("social group snapshot references unknown NPC: " + id + "/" + npcId);
            }
        }
    }
    const std::string expectedInstanceId = service.content_.instance ? service.content_.instance->id : "default";
    if (service.state_.instanceId.empty() || service.state_.instanceId != expectedInstanceId) {
        throw std::runtime_error("snapshot instance " + service.state_.instanceId +
                                 " does not match loaded instance " + expectedInstanceId);
    }
    const std::string expectedWorldId = service.content_.instance && !service.content_.instance->worldId.empty()
                                            ? service.content_.instance->worldId
                                            : service.content_.worldId;
    if (service.state_.worldId.empty() || service.state_.worldId != expectedWorldId) {
        throw std::runtime_error("snapshot world " + service.state_.worldId +
                                 " does not match loaded world " + expectedWorldId);
    }
    if (service.state_.contentVersion.empty() || service.state_.contentVersion != service.content_.contentVersion) {
        throw std::runtime_error("snapshot content version " + service.state_.contentVersion +
                                 " does not match loaded content " + service.content_.contentVersion);
    }
    service.rebuildEntityRegistry();
    service.replayBaseline_ = service.snapshotJson();
    service.replayBaselineEvents_ = service.state_.events;
    return service;
}

WorldService WorldService::fromSnapshot(const Json& json, std::string contentRoot,
                                        std::unique_ptr<IProvider> provider) {
    const auto content = ContentPackLoader::load(contentRoot);
    return fromContentSnapshot(content, json, std::move(provider));
}

WorldService WorldService::replay(const std::string& path, std::string contentRoot,
                                  std::unique_ptr<IProvider> provider) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read replay: " + path);
    std::vector<Json> documents;
    std::ostringstream whole;
    whole << input.rdbuf();
    const auto bytes = whole.str();
    if (!bytes.empty()) {
        try {
            const auto artifact = Json::parse(bytes);
            if (artifact.is_object() && artifact.value("kind", "") == "durable_commit") {
                if (!artifact.contains("replay") || !artifact.at("replay").is_array()) {
                    throw std::runtime_error("durable commit has no replay package");
                }
                for (const auto& item : artifact.at("replay")) documents.push_back(item);
            }
        } catch (const nlohmann::json::parse_error&) {
            // A normal Replay Package is JSONL, so parsing the complete file
            // as one JSON document is expected to fail. Parse its records
            // below instead.
        }
    }
    if (documents.empty()) {
        std::istringstream lines(bytes);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            documents.push_back(Json::parse(line));
        }
    }
    Json current;
    std::size_t expectedTransactions = 0;
    std::size_t appliedTransactions = 0;
    for (const auto& json : documents) {
        const auto kind = json.value("kind", "");
        if (kind == "replay_header") {
            if (json.value("schema_version", 0) != 2 || !json.contains("state")) {
                throw std::runtime_error("unsupported replay header");
            }
            current = json.at("state");
            expectedTransactions = json.value("transaction_count", 0U);
            if (!current.is_object() || !current.contains("events")) {
                throw std::runtime_error("replay baseline must contain an event history");
            }
            continue;
        }
        if (kind != "transaction") continue;
        if (current.empty()) throw std::runtime_error("replay transaction precedes its header");
        if (!json.contains("patch") || !json.at("patch").is_array() ||
            !json.contains("events") || !json.at("events").is_array()) {
            throw std::runtime_error("replay transaction is incomplete");
        }
        const auto baseRevision = current.at("world_revision").get<std::uint64_t>();
        if (json.at("base_world_revision").get<std::uint64_t>() != baseRevision) {
            throw std::runtime_error("replay transaction base revision does not match reconstructed state");
        }
        Json fact = current;
        fact.erase("events");
        fact = fact.patch(json.at("patch"));
        if (fact.at("world_revision").get<std::uint64_t>() !=
            json.at("world_revision").get<std::uint64_t>()) {
            throw std::runtime_error("replay transaction patch produced the wrong world revision");
        }
        if (json.contains("state_digest") &&
            json.at("state_digest").get<std::string>() != stateDigest(fact)) {
            throw std::runtime_error("replay transaction state digest mismatch");
        }
        auto events = current.at("events");
        for (const auto& event : json.at("events")) events.push_back(event);
        fact["events"] = std::move(events);
        current = std::move(fact);
        ++appliedTransactions;
    }
    if (current.empty()) throw std::runtime_error("replay has no header");
    if (appliedTransactions != expectedTransactions) {
        throw std::runtime_error("replay transaction count mismatch");
    }
    return fromSnapshot(current, std::move(contentRoot), std::move(provider));
}

}  // namespace tide_marks
