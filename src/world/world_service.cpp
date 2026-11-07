#include "world/world_service.hpp"
#include "world/entity_projection_system.hpp"
#include "world/world_bootstrap.hpp"
#include "world/ids.hpp"
#include "provider/provider_profile.hpp"
#include "provider/decision_context_projector.hpp"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <limits>
#include <set>

namespace tide_marks {

WorldService::WorldService(std::unique_ptr<IProvider> provider,
                           std::unique_ptr<IWorldPersistence> persistence)
    : provider_(provider ? std::move(provider) : makeDefaultRuntimeProvider()),
      llm_(std::make_unique<LlmEngine>(*provider_)),
      intelligence_(std::make_unique<IntelligenceCoordinator>(*llm_)),
      evolution_(std::make_unique<EvolutionQueue>(*llm_)),
      persistence_(persistence ? std::move(persistence) : std::make_unique<JsonFilePersistence>()) {}

WorldService WorldService::load(std::string contentRoot, std::unique_ptr<IProvider> provider) {
    return fromContent(ContentPackLoader::load(contentRoot), std::move(provider));
}

WorldService WorldService::fromContent(const WorldContentPack& content,
                                       std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.baseContent_ = content;
    service.content_ = content;
    service.state_ = WorldBootstrap::createState(content);
    const auto* instance = content.instance ? &*content.instance : nullptr;
    service.rebuildEntityRegistry();
    Json ids = Json::array();
    for (const auto& authored : content.npcs) ids.push_back(authored.definition.id);
    service.addEvent("world.initialized", kWorldActor, content.id, "World content pack loaded",
                     Json{{"content_pack", content.id}, {"content_version", content.version},
                          {"instance", instance != nullptr ? instance->id : "default"}, {"npcs", ids}});
    service.replayBaseline_ = service.snapshotJson();
    service.replayBaselineEvents_ = service.state_.events;
    return service;
}

void WorldService::rebuildEntityRegistry() {
    EntityProjectionSystem(*this).rebuild();
}

void WorldService::syncNpcEntityState() {
    EntityProjectionSystem(*this).syncNpc();
}

bool WorldService::hasNpc(const std::string& npcId) const {
    if (state_.npcs.find(npcId) == state_.npcs.end()) return false;
    const auto entity = state_.entities.find(npcEntityId(npcId));
    return entity == state_.entities.end() || entity->second.lifecycle == EntityLifecycle::Active;
}

DecisionContext WorldService::decisionContext(const std::string& npcId) const {
    const auto promptVersion = content_.contentVersion + ":" + content_.id;
    const auto worldRevision = state_.revision + (transactionManager_.active() ? 1 : 0);
    return DecisionContextProjector::build(state_, content_, npcId, worldRevision, promptVersion);
}

std::string summarize(const WorldState& state) {
    std::ostringstream output;
    output << state.now.toString();
    for (const auto& [id, value] : state.resources) output << "; " << id << '=' << value.amount;
    output << "; revision=" << state.revision;
    for (const auto& [id, npc] : state.npcs) {
        output << "; " << id << "@" << npc.location << ":stage=" << npc.goalStage;
        const auto highest = std::max_element(npc.needs.begin(), npc.needs.end(),
                                              [](const auto& left, const auto& right) {
                                                  if (left.second.deficit == right.second.deficit) {
                                                      return left.first > right.first;
                                                  }
                                                  return left.second.deficit < right.second.deficit;
                                              });
        if (highest != npc.needs.end()) output << ":need=" << highest->first << '=' << highest->second.deficit;
    }
    return output.str();
}


}  // namespace tide_marks
