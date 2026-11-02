#pragma once

#include "core/types.hpp"
#include <functional>
#include <memory>

namespace tide_marks {
enum class NodeStatus { Running, Success, Failure };

struct TickContext {
    bool interrupted{false};
    std::function<bool(const std::string&)> condition;
    std::function<NodeStatus(const std::string&)> action;
};

class BehaviorNode {
public:
    virtual ~BehaviorNode() = default;
    virtual NodeStatus tick(TickContext& context) = 0;
    virtual void reset() = 0;
};

class Sequence final : public BehaviorNode {
public:
    explicit Sequence(std::vector<std::unique_ptr<BehaviorNode>> children);
    NodeStatus tick(TickContext& context) override;
    void reset() override;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    std::size_t index_{0};
};

class Selector final : public BehaviorNode {
public:
    explicit Selector(std::vector<std::unique_ptr<BehaviorNode>> children);
    NodeStatus tick(TickContext& context) override;
    void reset() override;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    std::size_t index_{0};
};

class Condition final : public BehaviorNode {
public:
    explicit Condition(std::string name);
    NodeStatus tick(TickContext& context) override;
    void reset() override {}

private:
    std::string name_;
};

class Action final : public BehaviorNode {
public:
    explicit Action(std::string name);
    NodeStatus tick(TickContext& context) override;
    void reset() override {}

private:
    std::string name_;
};

class Cooldown final : public BehaviorNode {
public:
    Cooldown(std::unique_ptr<BehaviorNode> child, int ticks);
    NodeStatus tick(TickContext& context) override;
    void reset() override;

private:
    std::unique_ptr<BehaviorNode> child_;
    int ticks_{0};
    int remaining_{0};
};

class Timeout final : public BehaviorNode {
public:
    Timeout(std::unique_ptr<BehaviorNode> child, int maxTicks);
    NodeStatus tick(TickContext& context) override;
    void reset() override;

private:
    std::unique_ptr<BehaviorNode> child_;
    int maxTicks_{0};
    int elapsed_{0};
};

class Interruptible final : public BehaviorNode {
public:
    explicit Interruptible(std::unique_ptr<BehaviorNode> child);
    NodeStatus tick(TickContext& context) override;
    void reset() override;

private:
    std::unique_ptr<BehaviorNode> child_;
};


}  // namespace tide_marks
