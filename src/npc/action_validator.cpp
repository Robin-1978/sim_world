#include "npc/action_validator.hpp"

#include <algorithm>

namespace tide_marks {

ValidationResult ActionValidator::validate(const DecisionContext& context, const std::string& actionId,
                                          const Json& parameters) {
    for (const auto& candidate : context.candidates) {
        if (candidate.actionId != actionId) continue;
        if (!candidate.parameterSchema.is_object()) {
            return {false, "invalid_action_parameter_schema", "The authored action parameter schema is not an object."};
        }
        // A recipe with no declared inputs is intentionally closed to model
        // parameters. This prevents a free-form target from becoming an
        // implicit authority channel.
        if (candidate.parameterSchema.empty()) {
            if (!parameters.is_object() || !parameters.empty()) {
                return {false, "unexpected_action_parameters", "This behavior plan declares no input parameters."};
            }
            return {true, "ok", "Candidate action is allowed."};
        }
        if (!parameters.is_object()) return {false, "invalid_action_parameters", "Action parameters must be an object."};
        const auto required = candidate.parameterSchema.value("required", Json::array());
        if (!required.is_array()) return {false, "invalid_action_parameter_schema", "Action parameter required must be an array."};
        for (const auto& item : required) {
            if (!item.is_string() || !parameters.contains(item.get<std::string>())) {
                return {false, "missing_action_parameter", "A required behavior parameter is missing: " +
                                                               (item.is_string() ? item.get<std::string>() : std::string("unknown"))};
            }
        }
        const auto properties = candidate.parameterSchema.value("properties", Json::object());
        if (!properties.is_object()) return {false, "invalid_action_parameter_schema", "Action parameter properties must be an object."};
        const auto typeMatches = [](const Json& value, const std::string& type) {
            if (type == "string") return value.is_string();
            if (type == "integer") return value.is_number_integer();
            if (type == "number") return value.is_number();
            if (type == "boolean") return value.is_boolean();
            if (type == "object") return value.is_object();
            if (type == "array") return value.is_array();
            return false;
        };
        for (auto item = parameters.begin(); item != parameters.end(); ++item) {
            const auto definition = properties.find(item.key());
            if (definition == properties.end()) {
                if (candidate.parameterSchema.value("additionalProperties", true) == false) {
                    return {false, "unknown_action_parameter", "The behavior parameter is not declared: " + item.key()};
                }
                continue;
            }
            const auto type = definition.value().value("type", "");
            if (!type.empty() && !typeMatches(item.value(), type)) {
                return {false, "invalid_action_parameter_type", "The behavior parameter has the wrong type: " + item.key()};
            }
            const auto& rules = definition.value();
            if (rules.contains("enum")) {
                const auto values = rules.at("enum");
                if (!values.is_array() || std::none_of(values.begin(), values.end(),
                                                       [&item](const auto& allowed) {
                                                           return allowed == item.value();
                                                       })) {
                    return {false, "invalid_action_parameter_value",
                            "The behavior parameter is outside its authored enum: " + item.key()};
                }
            }
            if (item.value().is_number()) {
                const double number = item.value().get<double>();
                if (rules.contains("minimum") && (!rules.at("minimum").is_number() ||
                                                    number < rules.at("minimum").get<double>())) {
                    return {false, "invalid_action_parameter_value",
                            "The behavior parameter is below its authored minimum: " + item.key()};
                }
                if (rules.contains("maximum") && (!rules.at("maximum").is_number() ||
                                                    number > rules.at("maximum").get<double>())) {
                    return {false, "invalid_action_parameter_value",
                            "The behavior parameter exceeds its authored maximum: " + item.key()};
                }
            }
            if (item.value().is_array() && rules.contains("items")) {
                const auto itemSchema = rules.at("items");
                const auto itemType = itemSchema.value("type", "");
                if (!itemSchema.is_object() || itemType.empty() ||
                    std::any_of(item.value().begin(), item.value().end(),
                                [&itemType, &typeMatches](const auto& entry) {
                                    return !typeMatches(entry, itemType);
                                })) {
                    return {false, "invalid_action_parameter_type",
                            "The behavior parameter array contains an item with the wrong type: " + item.key()};
                }
            }
        }
        return {true, "ok", "Candidate action and its parameter contract are allowed."};
    }
    return {false, "unknown_action_id", "The provider selected an action outside the authored candidate set."};
}

}  // namespace tide_marks
