#include "npc/action_validator.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>

namespace tide_marks {
namespace {

bool typeMatches(const Json& value, const std::string& type) {
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    return false;
}

ValidationResult invalidSchema(const std::string& message) {
    return {false, "invalid_action_parameter_schema", message};
}

ValidationResult invalidValue(const std::string& message) {
    return {false, "invalid_action_parameter_value", message};
}

ValidationResult validateValue(const Json& value, const Json& schema, const std::string& path) {
    if (!schema.is_object()) {
        return invalidSchema("The authored behavior parameter schema must be an object: " + path);
    }
    const auto type = schema.value("type", "");
    if (!type.empty() && !std::set<std::string>{"string", "integer", "number", "boolean", "object", "array"}.contains(type)) {
        return invalidSchema("The authored behavior parameter type is unsupported: " + path);
    }
    if (schema.contains("items") && type != "array") {
        return invalidSchema("The authored items rule is only valid for arrays: " + path);
    }
    if (schema.contains("uniqueItems") && type != "array") {
        return invalidSchema("The authored uniqueItems rule is only valid for arrays: " + path);
    }
    if (!type.empty() && !typeMatches(value, type)) {
        return {false, "invalid_action_parameter_type", "The behavior parameter has the wrong type: " + path};
    }
    if (schema.contains("enum")) {
        const auto& values = schema.at("enum");
        if (!values.is_array()) return invalidSchema("The authored enum must be an array: " + path);
        if (std::none_of(values.begin(), values.end(), [&value](const auto& allowed) { return allowed == value; })) {
            return invalidValue("The behavior parameter is outside its authored enum: " + path);
        }
    }
    if (schema.contains("const") && schema.at("const") != value) {
        return invalidValue("The behavior parameter does not match its authored const: " + path);
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") &&
            (!schema.at("minimum").is_number() || number < schema.at("minimum").get<double>())) {
            return invalidValue("The behavior parameter is below its authored minimum: " + path);
        }
        if (schema.contains("maximum") &&
            (!schema.at("maximum").is_number() || number > schema.at("maximum").get<double>())) {
            return invalidValue("The behavior parameter exceeds its authored maximum: " + path);
        }
    }
    if (value.is_string()) {
        for (const auto* bound : {"minLength", "maxLength"}) {
            if (!schema.contains(bound)) continue;
            if (!schema.at(bound).is_number_integer() || schema.at(bound).get<std::int64_t>() < 0 ||
                static_cast<std::uint64_t>(schema.at(bound).get<std::int64_t>()) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return invalidSchema(std::string("The authored ") + bound +
                                     " rule must be a non-negative integer: " + path);
            }
            const auto limit = static_cast<std::size_t>(schema.at(bound).get<std::int64_t>());
            if (std::string(bound) == "minLength" && value.get<std::string>().size() < limit) {
                return invalidValue("The behavior parameter is shorter than its authored minimum: " + path);
            }
            if (std::string(bound) == "maxLength" && value.get<std::string>().size() > limit) {
                return invalidValue("The behavior parameter exceeds its authored maximum: " + path);
            }
        }
    }
    if (value.is_array()) {
        for (const auto* bound : {"minItems", "maxItems"}) {
            if (!schema.contains(bound)) continue;
            if (!schema.at(bound).is_number_integer() || schema.at(bound).get<std::int64_t>() < 0 ||
                static_cast<std::uint64_t>(schema.at(bound).get<std::int64_t>()) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return invalidSchema(std::string("The authored ") + bound +
                                     " rule must be a non-negative integer: " + path);
            }
            const auto limit = static_cast<std::size_t>(schema.at(bound).get<std::int64_t>());
            if (std::string(bound) == "minItems" && value.size() < limit) {
                return invalidValue("The behavior parameter array is shorter than its authored minimum: " + path);
            }
            if (std::string(bound) == "maxItems" && value.size() > limit) {
                return invalidValue("The behavior parameter array exceeds its authored maximum: " + path);
            }
        }
        if (schema.contains("uniqueItems")) {
            if (!schema.at("uniqueItems").is_boolean()) {
                return invalidSchema("The authored uniqueItems rule must be boolean: " + path);
            }
            if (schema.at("uniqueItems").get<bool>()) {
                std::set<Json> seen;
                for (const auto& entry : value) {
                    if (!seen.insert(entry).second) {
                        return invalidValue("The behavior parameter array contains duplicate items: " + path);
                    }
                }
            }
        }
        if (schema.contains("items")) {
            const auto& itemSchema = schema.at("items");
            if (!itemSchema.is_object()) return invalidSchema("The authored array item schema must be an object: " + path);
            for (std::size_t index = 0; index < value.size(); ++index) {
                const auto result = validateValue(value.at(index), itemSchema,
                                                  path + "[" + std::to_string(index) + "]");
                if (!result.valid) return result;
            }
        }
    }
    if (value.is_object()) {
        const auto properties = schema.value("properties", Json::object());
        if (!properties.is_object()) return invalidSchema("The authored properties must be an object: " + path);
        if (schema.contains("additionalProperties") && !schema.at("additionalProperties").is_boolean()) {
            return invalidSchema("The authored additionalProperties rule must be boolean: " + path);
        }
        if (schema.contains("required")) {
            if (!schema.at("required").is_array()) return invalidSchema("The authored required must be an array: " + path);
            std::set<std::string> requiredNames;
            for (const auto& item : schema.at("required")) {
                if (!item.is_string() || !requiredNames.insert(item.get<std::string>()).second) {
                    return invalidSchema("The authored required names must be unique strings: " + path);
                }
                if (!value.contains(item.get<std::string>())) {
                    return {false, "missing_action_parameter",
                            "A required behavior parameter is missing: " + path + "." + item.get<std::string>()};
                }
            }
        }
        for (auto item = value.begin(); item != value.end(); ++item) {
            const auto definition = properties.find(item.key());
            if (definition == properties.end()) {
                if (schema.value("additionalProperties", true) == false) {
                    return {false, "unknown_action_parameter",
                            "The behavior parameter is not declared: " + path + "." + item.key()};
                }
                continue;
            }
            const auto result = validateValue(item.value(), definition.value(), path + "." + item.key());
            if (!result.valid) return result;
        }
    }
    return {true, "ok", "Action parameters satisfy the authored schema."};
}

}  // namespace

ValidationResult ActionValidator::validate(const DecisionContext& context, const std::string& actionId,
                                          const Json& parameters) {
    for (const auto& candidate : context.candidates) {
        if (candidate.actionId != actionId) continue;
        if (!candidate.parameterSchema.is_object()) {
            return invalidSchema("The authored action parameter schema is not an object.");
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
        return validateValue(parameters, candidate.parameterSchema, "parameters");
    }
    return {false, "unknown_action_id", "The provider selected an action outside the authored candidate set."};
}

}  // namespace tide_marks
