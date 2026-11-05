#include "npc/behavior_recipe.hpp"

#include <stdexcept>

namespace tide_marks {
namespace {

bool canStartNode(const BehaviorNodeContent& node,
                  const BehaviorRecipeRuntime::ConditionHandler& condition,
                  bool& actionReached) {
    if (node.type == "condition") return condition && condition(node);
    if (node.type == "meta_action") {
        actionReached = true;
        return true;
    }
    if (node.type == "interruptible" || node.type == "timeout") {
        return !node.children.empty() && canStartNode(node.children.front(), condition, actionReached);
    }
    if (node.type == "sequence") {
        for (const auto& child : node.children) {
            if (!canStartNode(child, condition, actionReached)) return false;
            // The first action leaf is the process start boundary. Conditions
            // after it may depend on effects produced at completion.
            if (actionReached) return true;
        }
        return actionReached;
    }
    if (node.type == "selector") {
        for (const auto& child : node.children) {
            bool childActionReached = false;
            if (canStartNode(child, condition, childActionReached) && childActionReached) {
                actionReached = true;
                return true;
            }
        }
        return false;
    }
    return false;
}

struct CompiledTree {
    std::unique_ptr<BehaviorNode> root;
    std::map<std::string, const BehaviorNodeContent*> nodes;
};

std::unique_ptr<BehaviorNode> compileNode(const BehaviorNodeContent& node, CompiledTree& tree) {
    if (node.id.empty()) throw std::runtime_error("behavior node has no id");
    if (!tree.nodes.emplace(node.id, &node).second) {
        throw std::runtime_error("duplicate behavior node id: " + node.id);
    }
    if (node.type == "sequence" || node.type == "selector") {
        if (node.children.empty()) {
            throw std::runtime_error("composite behavior node requires at least one child: " + node.id);
        }
        std::vector<std::unique_ptr<BehaviorNode>> children;
        children.reserve(node.children.size());
        for (const auto& child : node.children) children.push_back(compileNode(child, tree));
        if (node.type == "sequence") return std::make_unique<Sequence>(std::move(children));
        return std::make_unique<Selector>(std::move(children));
    }
    if (node.type == "condition") {
        if (node.name.empty()) throw std::runtime_error("behavior condition has no name: " + node.id);
        return std::make_unique<Condition>(node.id);
    }
    if (node.type == "meta_action") {
        if (node.name.empty()) throw std::runtime_error("behavior meta-action has no name: " + node.id);
        return std::make_unique<Action>(node.id);
    }
    if (node.type == "interruptible") {
        if (node.children.size() != 1) throw std::runtime_error("interruptible requires one child: " + node.id);
        return std::make_unique<Interruptible>(compileNode(node.children.front(), tree));
    }
    if (node.type == "timeout") {
        if (node.children.size() != 1 || node.maxTicks <= 0) {
            throw std::runtime_error("timeout requires one child and positive max_ticks: " + node.id);
        }
        return std::make_unique<Timeout>(compileNode(node.children.front(), tree), node.maxTicks);
    }
    throw std::runtime_error("unsupported behavior node type: " + node.type);
}

}  // namespace

BehaviorRecipeRunResult BehaviorRecipeRuntime::run(const BehaviorRecipeContent& recipe,
                                                   ConditionHandler condition,
                                                   ActionHandler action) {
    CompiledTree tree;
    tree.root = compileNode(recipe.root, tree);
    std::string failedNode;
    TickContext context;
    context.condition = [&tree, &condition, &failedNode](const std::string& id) {
        const auto found = tree.nodes.find(id);
        if (found == tree.nodes.end() || !condition) {
            failedNode = id;
            return false;
        }
        const bool result = condition(*found->second);
        if (!result) failedNode = id;
        return result;
    };
    context.action = [&tree, &action, &failedNode](const std::string& id) {
        const auto found = tree.nodes.find(id);
        if (found == tree.nodes.end() || !action) {
            failedNode = id;
            return NodeStatus::Failure;
        }
        const bool result = action(*found->second);
        if (!result) failedNode = id;
        return result ? NodeStatus::Success : NodeStatus::Failure;
    };
    const auto status = tree.root->tick(context);
    return BehaviorRecipeRunResult{status, status == NodeStatus::Success ? "" : failedNode};
}

bool BehaviorRecipeRuntime::canStart(const BehaviorRecipeContent& recipe,
                                     ConditionHandler condition) {
    bool actionReached = false;
    return canStartNode(recipe.root, condition, actionReached) && actionReached;
}

void BehaviorRecipeRuntime::validate(const BehaviorRecipeContent& recipe) {
    CompiledTree tree;
    tree.root = compileNode(recipe.root, tree);
}

}  // namespace tide_marks
