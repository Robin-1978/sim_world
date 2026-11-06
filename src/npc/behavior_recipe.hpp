#pragma once

#include "core/types.hpp"
#include "npc/behavior_tree.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tide_marks {

// A named, authored capability. The operator is one of the Runtime's small
// set of generic fact-transition primitives; the id and parameters belong to
// content and may be added without a new C++ action branch.
struct MetaActionDefinition {
    std::string id;
    std::string category;
    std::string description;
    std::string operation;
    Json defaultArgs = Json::object();
};

// A reusable predicate over committed world state. Conditions never write
// facts. Content receives a named reusable predicate while Runtime only
// interprets the generic operator.
struct ConditionDefinition {
    std::string id;
    std::string operation;
    Json args = Json::object();
};

// A recipe is authored data. It may only compose registered conditions and
// meta-actions; it cannot reference arbitrary C++ or write state directly.
struct BehaviorNodeContent {
    std::string type;
    std::string id;
    std::string name;
    Json args = Json::object();
    int maxTicks{0};
    std::vector<BehaviorNodeContent> children;
};

struct BehaviorRecipeContent {
    std::string id;
    std::string planId;
    // A short content-authored explanation shown in decision catalogs. It is
    // descriptive only; Runtime still executes the registered BT below.
    std::string description;
    int durationMinutes{0};
    bool interruptible{true};
    double interruptSalienceThreshold{1.0};
    // Reservation is an authored process policy. Runtime supports the
    // generic modes "none" and "input_resources"; it never infers policy
    // from duration or resource names.
    std::string reservationMode{"none"};
    std::vector<std::string> allowedActors;
    std::map<std::string, int> reservations;
    // JSON Schema for decision parameters supplied by the LLM.  The schema
    // is a content contract; it never grants authority and is checked again
    // by Runtime before a recipe runs.
    Json parameterSchema = Json::object();
    BehaviorNodeContent root;
    // Resource arbitration is explicit content policy. A process with a
    // higher priority may suspend lower-priority reserving processes only
    // when preemption_policy is "lower_priority".
    int priority{0};
    std::string preemptionPolicy{"none"};
};

struct BehaviorRecipeRunResult {
    NodeStatus status{NodeStatus::Failure};
    std::string failedNode;
};

class BehaviorRecipeRuntime {
public:
    using ConditionHandler = std::function<bool(const BehaviorNodeContent&)>;
    using ActionHandler = std::function<bool(const BehaviorNodeContent&)>;

    // Compiles the recipe into a fresh tree for one evaluation. Long-running
    // waits belong to ProcessExecution; this method evaluates the recipe's
    // atomic effects at the process boundary.
    [[nodiscard]] static BehaviorRecipeRunResult run(const BehaviorRecipeContent& recipe,
                                                      ConditionHandler condition,
                                                      ActionHandler action);
    // Checks only the conditions that must hold before the first action leaf
    // can start. It is a read-only projection helper; Runtime still evaluates
    // the full tree again at process completion.
    [[nodiscard]] static bool canStart(const BehaviorRecipeContent& recipe,
                                       ConditionHandler condition);
    static void validate(const BehaviorRecipeContent& recipe);
};

}  // namespace tide_marks
