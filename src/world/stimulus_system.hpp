#pragma once

#include "core/types.hpp"

namespace tide_marks {

class WorldService;

// Projects committed events into content-defined external stimuli and owns
// only the NPC-scoped stimulus lifecycle. The event journal and WorldService
// still own fact commits and audit identity.
class StimulusSystem {
public:
    explicit StimulusSystem(WorldService& world) : world_(world) {}

    void project(const WorldEvent& event);
    void expire();

private:
    WorldService& world_;
};

}  // namespace tide_marks
