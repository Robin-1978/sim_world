#include "core/json_utils.hpp"

#include <algorithm>
#include <cmath>

namespace tide_marks {
namespace {

double roundedFinite(const double value, const int decimalPlaces) {
    if (!std::isfinite(value)) return value;
    const int places = std::clamp(decimalPlaces, 0, 12);
    const double scale = std::pow(10.0, static_cast<double>(places));
    const double rounded = std::round(value * scale) / scale;
    return rounded == 0.0 ? 0.0 : rounded;
}

Json compact(const Json& value, const int decimalPlaces) {
    if (value.is_number_float()) return roundedFinite(value.get<double>(), decimalPlaces);
    if (value.is_array()) {
        Json result = Json::array();
        for (const auto& item : value) result.push_back(compact(item, decimalPlaces));
        return result;
    }
    if (value.is_object()) {
        Json result = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) result[it.key()] = compact(it.value(), decimalPlaces);
        return result;
    }
    return value;
}

}  // namespace

Json compactJsonNumbers(const Json& value, const int decimalPlaces) {
    return compact(value, decimalPlaces);
}

}  // namespace tide_marks
