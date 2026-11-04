#include "world/cognition_system.hpp"

#include "world/world_service.hpp"

#include <algorithm>

namespace tide_marks {

CommandResult CognitionSystem::assertKnownFact(const std::string& npcId, const std::string& subject,
                                            const std::string& predicate, const std::string& value,
                                            std::uint64_t sourceSequence, const std::string& scope,
                                            double confidence, const std::string& actor,
                                            const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The knowledge fact was already applied.");
    }
    auto npc = world_.state_.npcs.find(npcId);
    if (npc == world_.state_.npcs.end()) return world_.reject("unknown_npc", "Knowledge must belong to an existing NPC.");
    if (actor != kWorldActor && actor != npcId && actor != kPlayerId) {
        return world_.reject("unauthorized", "The actor cannot write this NPC's knowledge.");
    }
    if (subject.empty() || predicate.empty() || value.empty()) {
        return world_.reject("invalid_knowledge", "Knowledge needs subject, predicate and value.");
    }
    if (confidence < 0.0 || confidence > 1.0) return world_.reject("invalid_knowledge", "Knowledge confidence must be between 0 and 1.");
    const auto source = std::find_if(world_.state_.events.begin(), world_.state_.events.end(), [sourceSequence](const auto& event) {
        return event.sequence == sourceSequence;
    });
    if (source == world_.state_.events.end()) return world_.reject("unknown_knowledge_source", "Knowledge must cite a committed event.");
    const bool participant = source->actor == npcId || source->target == npcId || source->target == npcEntityId(npcId);
    if (scope == "direct" && !participant) {
        return world_.reject("source_not_observed", "An NPC cannot claim direct knowledge of an unrelated event.");
    }
    if (scope != "direct" && scope != "witness" && scope != "testimony" && scope != "inference") {
        return world_.reject("invalid_knowledge_scope", "Knowledge scope is not registered.");
    }
    const auto existing = std::find_if(npc->second.knowledge.begin(), npc->second.knowledge.end(),
                                       [&subject, &predicate](const auto& fact) {
                                           return fact.subject == subject && fact.predicate == predicate;
                                       });
    const std::string id = "knowledge-" + std::to_string(world_.state_.events.size() + 1);
    KnowledgeFact fact{id, subject, predicate, value, sourceSequence, source->type, world_.state_.now,
                       confidence, scope, true};
    if (existing != npc->second.knowledge.end()) *existing = fact;
    else npc->second.knowledge.push_back(fact);
    ++npc->second.stateVersion;
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(
        "knowledge.asserted", actor, npcId, "An NPC recorded a sourced belief",
        Json{{"knowledge_id", id}, {"subject", subject}, {"predicate", predicate},
             {"value", value}, {"source_sequence", sourceSequence}, {"source_type", source->type},
             {"scope", scope}, {"confidence", confidence}});
    return CommandResult{true, "ok", id, event};
}

CommandResult CognitionSystem::recordExperience(const std::string& npcId, const std::string& domain,
                                             const std::string& trigger, const std::string& action,
                                             const std::string& outcome, std::uint64_t sourceSequence,
                                             double confidence, bool personal, const std::string& actor,
                                             const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The experience was already applied.");
    }
    auto npc = world_.state_.npcs.find(npcId);
    if (npc == world_.state_.npcs.end()) return world_.reject("unknown_npc", "Experience must belong to an existing NPC.");
    if (actor != kWorldActor && actor != npcId && actor != kPlayerId) {
        return world_.reject("unauthorized", "The actor cannot write this NPC's experience.");
    }
    if (domain.empty() || trigger.empty() || action.empty() || outcome.empty()) {
        return world_.reject("invalid_experience", "Experience needs domain, trigger, action and outcome.");
    }
    if (confidence < 0.0 || confidence > 1.0) return world_.reject("invalid_experience", "Experience confidence must be between 0 and 1.");
    const auto source = std::find_if(world_.state_.events.begin(), world_.state_.events.end(), [sourceSequence](const auto& event) {
        return event.sequence == sourceSequence;
    });
    if (source == world_.state_.events.end()) return world_.reject("unknown_experience_source", "Experience must cite a committed event.");
    const auto existing = std::find_if(npc->second.experiences.begin(), npc->second.experiences.end(),
                                       [&domain, &trigger, &action](const auto& experience) {
                                           return experience.domain == domain && experience.trigger == trigger &&
                                                  experience.action == action;
                                       });
    if (existing != npc->second.experiences.end()) {
        const double samples = static_cast<double>(existing->sourceSequences.size() + 1);
        existing->confidence = std::clamp(existing->confidence + (confidence - existing->confidence) / samples,
                                           0.0, 1.0);
        existing->outcome = outcome;
        existing->lastUsed = world_.state_.now;
        if (std::find(existing->sourceSequences.begin(), existing->sourceSequences.end(), sourceSequence) ==
            existing->sourceSequences.end()) {
            existing->sourceSequences.push_back(sourceSequence);
        }
    } else {
        Experience experience{"experience-" + std::to_string(world_.state_.events.size() + 1), domain, trigger,
                               action, outcome, {sourceSequence}, world_.state_.now, world_.state_.now, confidence, personal};
        npc->second.experiences.push_back(std::move(experience));
    }
    ++npc->second.stateVersion;
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(
        "experience.recorded", actor, npcId, "An outcome was abstracted into reusable experience",
        Json{{"domain", domain}, {"trigger", trigger}, {"action", action},
             {"outcome", outcome}, {"source_sequence", sourceSequence},
             {"confidence", confidence}, {"personal", personal}});
    return CommandResult{true, "ok", domain, event};
}


}  // namespace tide_marks
