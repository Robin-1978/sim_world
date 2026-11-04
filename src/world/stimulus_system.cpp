#include "world/stimulus_system.hpp"

#include "world/world_service.hpp"

#include <algorithm>
#include <cmath>

namespace tide_marks {

void StimulusSystem::project(const WorldEvent& event) {
    const WorldEvent source = event;
    const auto rule = std::find_if(world_.content_.stimulusRules.begin(), world_.content_.stimulusRules.end(),
                                   [&source](const auto& candidate) {
                                       return source.type.starts_with(candidate.eventPrefix);
                                   });
    if (rule == world_.content_.stimulusRules.end()) return;
    for (auto& [npcId, npc] : world_.state_.npcs) {
        const auto entity = world_.state_.entities.find(npcEntityId(npcId));
        if (entity != world_.state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        const bool participant = source.actor == npcId || source.target == npcId;
        const bool visible = rule->visibility == "world" ||
                             (rule->visibility == "participants" && participant) ||
                             (rule->visibility == "actor" && source.actor == npcId) ||
                             (rule->visibility == "target" && source.target == npcId);
        if (!visible) continue;
        auto sensitivity = npc.stimulusSensitivity.find(rule->category);
        if (sensitivity == npc.stimulusSensitivity.end()) {
            sensitivity = npc.stimulusSensitivity.emplace(rule->category, world_.content_.stimulusPolicy.maxSensitivity).first;
        }
        const std::int64_t windowStart = world_.state_.now.minute - world_.content_.stimulusPolicy.repeatWindowMinutes;
        const int repetitions = static_cast<int>(std::count_if(
            npc.stimuli.begin(), npc.stimuli.end(), [&](const auto& prior) {
                return prior.active && prior.category == rule->category && prior.actor == source.actor &&
                       prior.observedAt.minute >= windowStart;
            }));
        std::erase_if(npc.stimuli, [this](const auto& stimulus) {
            return !stimulus.active && world_.state_.now.minute - stimulus.expiresAt.minute > 0;
        });
        while (npc.stimuli.size() >= world_.content_.stimulusPolicy.maxActivePerNpc) {
            auto oldest = std::min_element(npc.stimuli.begin(), npc.stimuli.end(),
                                           [](const auto& left, const auto& right) {
                                               if (left.effectiveSalience == right.effectiveSalience) {
                                                   return left.observedAt.minute < right.observedAt.minute;
                                               }
                                               return left.effectiveSalience < right.effectiveSalience;
                                           });
            if (oldest == npc.stimuli.end()) break;
            npc.stimuli.erase(oldest);
        }
        const double novelty = std::pow(world_.content_.stimulusPolicy.noveltyDecay,
                                        std::max(0, repetitions));
        const double sensitivityBefore = sensitivity->second;
        if (repetitions > 0) {
            sensitivity->second = std::clamp(
                sensitivityBefore - world_.content_.stimulusPolicy.habituationStep * repetitions,
                world_.content_.stimulusPolicy.minSensitivity, world_.content_.stimulusPolicy.maxSensitivity);
            if (std::abs(sensitivity->second - sensitivityBefore) >= 0.000001) {
                world_.addEvent("stimulus.habituated", npcId, rule->category,
                         "Repeated stimuli reduced this character's sensitivity",
                         Json{{"category", rule->category}, {"source_type", source.type},
                              {"repetitions", repetitions}, {"before", sensitivityBefore},
                              {"after", sensitivity->second}, {"rule", rule->id}});
            }
        }
        const double salience = std::clamp(rule->relevance * rule->intensity * rule->urgency *
                                               novelty * rule->sourceConfidence, 0.0, 1.0);
        ExternalStimulus stimulus;
        stimulus.id = "stimulus:" + std::to_string(source.sequence) + ":" + npcId;
        stimulus.sourceEvent = source.type;
        stimulus.sourceSequence = source.sequence;
        stimulus.category = rule->category;
        stimulus.actor = source.actor;
        stimulus.target = source.target;
        stimulus.observedAt = world_.state_.now;
        stimulus.expiresAt = GameTime{world_.state_.now.minute + rule->ttlMinutes};
        stimulus.visibilityScope = rule->visibility;
        stimulus.sourceConfidence = rule->sourceConfidence;
        stimulus.relevance = rule->relevance;
        stimulus.intensity = rule->intensity;
        stimulus.urgency = rule->urgency;
        stimulus.novelty = novelty;
        stimulus.valence = rule->valence;
        stimulus.salience = salience;
        stimulus.effectiveSalience = salience * sensitivity->second;
        npc.stimuli.push_back(std::move(stimulus));
    }
}

void StimulusSystem::expire() {
    for (auto& [id, npc] : world_.state_.npcs) {
        (void)id;
        for (auto& stimulus : npc.stimuli) {
            if (stimulus.active && world_.state_.now.minute >= stimulus.expiresAt.minute) stimulus.active = false;
        }
    }
}


}  // namespace tide_marks
