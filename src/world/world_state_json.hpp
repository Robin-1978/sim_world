#pragma once

#include "core/types.hpp"

namespace tide_marks {

[[nodiscard]] Json worldStateToJson(const WorldState& state);
[[nodiscard]] WorldState deserializeWorldState(const Json& json);
[[nodiscard]] Json worldEventToJson(const WorldEvent& event);

}  // namespace tide_marks
