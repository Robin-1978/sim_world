#include "npc/behavior_tree.hpp"
#include <algorithm>

namespace tide_marks {
Sequence::Sequence(std::vector<std::unique_ptr<BehaviorNode>> children) : children_(std::move(children)) {}
NodeStatus Sequence::tick(TickContext& context) {
    while (index_ < children_.size()) {
        const auto status = children_[index_]->tick(context);
        if (status == NodeStatus::Running) return status;
        if (status == NodeStatus::Failure) {
            reset();
            return status;
        }
        ++index_;
    }
    reset();
    return NodeStatus::Success;
}
void Sequence::reset() {
    index_ = 0;
    for (auto& child : children_) child->reset();
}

Selector::Selector(std::vector<std::unique_ptr<BehaviorNode>> children) : children_(std::move(children)) {}
NodeStatus Selector::tick(TickContext& context) {
    while (index_ < children_.size()) {
        const auto status = children_[index_]->tick(context);
        if (status == NodeStatus::Running) return status;
        if (status == NodeStatus::Success) {
            reset();
            return status;
        }
        ++index_;
    }
    reset();
    return NodeStatus::Failure;
}
void Selector::reset() {
    index_ = 0;
    for (auto& child : children_) child->reset();
}

Condition::Condition(std::string name) : name_(std::move(name)) {}
NodeStatus Condition::tick(TickContext& context) {
    return context.condition && context.condition(name_) ? NodeStatus::Success : NodeStatus::Failure;
}
Action::Action(std::string name) : name_(std::move(name)) {}
NodeStatus Action::tick(TickContext& context) {
    return context.action ? context.action(name_) : NodeStatus::Failure;
}

Cooldown::Cooldown(std::unique_ptr<BehaviorNode> child, int ticks)
    : child_(std::move(child)), ticks_(std::max(0, ticks)) {}
NodeStatus Cooldown::tick(TickContext& context) {
    if (remaining_ > 0) {
        --remaining_;
        return NodeStatus::Failure;
    }
    const auto status = child_->tick(context);
    if (status == NodeStatus::Success) remaining_ = ticks_;
    return status;
}
void Cooldown::reset() { remaining_ = 0; child_->reset(); }

Timeout::Timeout(std::unique_ptr<BehaviorNode> child, int maxTicks)
    : child_(std::move(child)), maxTicks_(std::max(1, maxTicks)) {}
NodeStatus Timeout::tick(TickContext& context) {
    if (elapsed_++ >= maxTicks_) {
        reset();
        return NodeStatus::Failure;
    }
    const auto status = child_->tick(context);
    if (status != NodeStatus::Running) reset();
    return status;
}
void Timeout::reset() { elapsed_ = 0; child_->reset(); }

Interruptible::Interruptible(std::unique_ptr<BehaviorNode> child) : child_(std::move(child)) {}
NodeStatus Interruptible::tick(TickContext& context) {
    if (context.interrupted) {
        reset();
        return NodeStatus::Failure;
    }
    return child_->tick(context);
}
void Interruptible::reset() { child_->reset(); }


}  // namespace tide_marks
