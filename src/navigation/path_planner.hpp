#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tide_marks {

struct GlobalEdge {
    std::string to;
    double cost{1.0};
};

// Region/room planning: stable authored graph, deterministic Dijkstra route.
class GlobalPlanner {
public:
    void addNode(std::string id);
    void addEdge(std::string from, std::string to, double cost = 1.0, bool bidirectional = true);
    [[nodiscard]] std::optional<std::vector<std::string>> plan(const std::string& from,
                                                                const std::string& goal) const;

private:
    std::map<std::string, std::vector<GlobalEdge>> graph_;
};

struct GridPoint {
    int x{0};
    int z{0};
    friend bool operator<(const GridPoint& left, const GridPoint& right) {
        return left.x == right.x ? left.z < right.z : left.x < right.x;
    }
    friend bool operator==(const GridPoint& left, const GridPoint& right) {
        return left.x == right.x && left.z == right.z;
    }
};

// Current-region planning: deterministic four-neighbor A* on a collision grid.
class LocalPlanner {
public:
    LocalPlanner(int width, int height);
    void setBlocked(GridPoint point, bool blocked = true);
    [[nodiscard]] std::optional<std::vector<GridPoint>> plan(GridPoint start, GridPoint goal) const;

private:
    int width_;
    int height_;
    std::set<GridPoint> blocked_;
    [[nodiscard]] bool inside(GridPoint point) const;
};

}  // namespace tide_marks
