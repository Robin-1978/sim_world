#pragma once

#include "content/content_pack.hpp"

namespace tide_marks {

// Builds the initial mutable WorldState from immutable content and an
// optional Game Instance opening configuration.  The bootstrapper has no
// access to transactions, providers, events, or runtime overlays; the
// WorldService remains the sole owner of the resulting facts.
class WorldBootstrap {
public:
    [[nodiscard]] static WorldState createState(const WorldContentPack& content);
};

}  // namespace tide_marks
