#pragma once

#include "core/types.hpp"

namespace tide_marks {

// Keep simulation arithmetic at full precision while making JSON wire,
// diagnostic, snapshot and replay projections stable and compact.
[[nodiscard]] Json compactJsonNumbers(const Json& value, int decimalPlaces = 6);

}  // namespace tide_marks
