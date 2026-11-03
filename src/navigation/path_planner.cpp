#include "navigation/path_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace tide_marks {

void GlobalPlanner::addNode(std::string id) {
    graph_.try_emplace(std::move(id));
}

void GlobalPlanner::addEdge(std::string from, std::string to, double cost, bool bidirectional) {
    addNode(from);
    addNode(to);
    graph_[from].push_back(GlobalEdge{to, std::max(0.0, cost)});
    if (bidirectional) graph_[to].push_back(GlobalEdge{from, std::max(0.0, cost)});
}

std::optional<std::vector<std::string>> GlobalPlanner::plan(const std::string& from,
                                                             const std::string& goal) const {
    if (!graph_.contains(from) || !graph_.contains(goal)) return std::nullopt;
    std::map<std::string, double> distance;
    std::map<std::string, std::string> previous;
    for (const auto& [id, edges] : graph_) {
        (void)edges;
        distance[id] = std::numeric_limits<double>::infinity();
    }
    using QueueItem = std::pair<double, std::string>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> queue;
    distance[from] = 0.0;
    queue.emplace(0.0, from);
    while (!queue.empty()) {
        const auto [cost, node] = queue.top();
        queue.pop();
        if (cost > distance[node]) continue;
        if (node == goal) break;
        for (const auto& edge : graph_.at(node)) {
            const double nextCost = cost + edge.cost;
            if (nextCost < distance[edge.to]) {
                distance[edge.to] = nextCost;
                previous[edge.to] = node;
                queue.emplace(nextCost, edge.to);
            }
        }
    }
    if (!std::isfinite(distance[goal])) return std::nullopt;
    std::vector<std::string> path;
    for (std::string node = goal;; node = previous.at(node)) {
        path.push_back(node);
        if (node == from) break;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

LocalPlanner::LocalPlanner(int width, int height) : width_(width), height_(height) {}
bool LocalPlanner::inside(GridPoint point) const {
    return point.x >= 0 && point.z >= 0 && point.x < width_ && point.z < height_;
}
void LocalPlanner::setBlocked(GridPoint point, bool blocked) {
    if (!inside(point)) return;
    if (blocked) blocked_.insert(point);
    else blocked_.erase(point);
}

std::optional<std::vector<GridPoint>> LocalPlanner::plan(GridPoint start, GridPoint goal) const {
    if (!inside(start) || !inside(goal) || blocked_.contains(start) || blocked_.contains(goal)) return std::nullopt;
    const auto heuristic = [goal](GridPoint point) {
        return std::abs(point.x - goal.x) + std::abs(point.z - goal.z);
    };
    struct QueueItem {
        int score;
        GridPoint point;
        bool operator>(const QueueItem& other) const { return score > other.score; }
    };
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> open;
    std::map<GridPoint, int> cost;
    std::map<GridPoint, GridPoint> previous;
    cost[start] = 0;
    open.push({heuristic(start), start});
    constexpr GridPoint directions[]{{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    while (!open.empty()) {
        const auto current = open.top().point;
        open.pop();
        if (current == goal) break;
        for (const auto direction : directions) {
            const GridPoint next{current.x + direction.x, current.z + direction.z};
            if (!inside(next) || blocked_.contains(next)) continue;
            const int nextCost = cost[current] + 1;
            if (!cost.contains(next) || nextCost < cost[next]) {
                cost[next] = nextCost;
                previous[next] = current;
                open.push({nextCost + heuristic(next), next});
            }
        }
    }
    if (!cost.contains(goal)) return std::nullopt;
    std::vector<GridPoint> path;
    for (auto point = goal;; point = previous.at(point)) {
        path.push_back(point);
        if (point == start) break;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

}  // namespace tide_marks
