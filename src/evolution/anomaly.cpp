#include "evolution/anomaly.hpp"

#include <algorithm>

namespace tide_marks {
namespace {
struct Evidence {
    std::uint64_t sequence{0};
    std::string type;
};

Evidence evidenceFor(const WorldState& state, const std::string& subject, const std::string& target) {
    for (auto event = state.events.rbegin(); event != state.events.rend(); ++event) {
        const bool dataMatch = event->data.is_object() &&
                               (event->data.value("resource", "") == subject ||
                                event->data.value("group_id", "") == subject ||
                                event->data.value("promise_id", "") == subject);
        if (dataMatch || event->actor == subject || event->target == target || event->actor == target) {
            return Evidence{event->sequence, event->type};
        }
    }
    return {};
}
}  // namespace

Json anomalyFindingToJson(const AnomalyFinding& finding) {
    return Json{{"rule_id", finding.ruleId},
                {"operation", finding.operation},
                {"subject", finding.subject},
                {"metric", finding.metric},
                {"observed", finding.observed},
                {"threshold", finding.threshold},
                {"severity", finding.severity},
                {"description", finding.description},
                {"source_sequence", finding.sourceSequence},
                {"source_event_type", finding.sourceEventType}};
}

std::vector<AnomalyFinding> AnomalyDetector::scan(const WorldContentPack& content, const WorldState& state) {
    std::vector<AnomalyFinding> findings;
    for (const auto& rule : content.anomalyRules) {
        double observed = 0.0;
        bool triggered = false;
        std::string target = rule.subject;
        if (rule.operation == "resource.below" || rule.operation == "resource.above") {
            const auto resource = state.resources.find(rule.subject);
            if (resource == state.resources.end()) continue;
            observed = static_cast<double>(resource->second.amount);
            triggered = rule.operation == "resource.below" ? observed < rule.threshold
                                                            : observed > rule.threshold;
        } else if (rule.operation == "need.above") {
            const auto npc = state.npcs.find(rule.subject);
            if (npc == state.npcs.end()) continue;
            const auto need = npc->second.needs.find(rule.metric);
            if (need == npc->second.needs.end()) continue;
            observed = need->second.deficit;
            triggered = observed > rule.threshold;
            target = rule.metric;
        } else if (rule.operation == "group_metric.below" || rule.operation == "group_metric.above") {
            const auto group = state.socialGroups.find(rule.subject);
            if (group == state.socialGroups.end()) continue;
            const auto metric = group->second.metrics.find(rule.metric);
            if (metric == group->second.metrics.end()) continue;
            observed = metric->second;
            triggered = rule.operation == "group_metric.below" ? observed < rule.threshold
                                                                : observed > rule.threshold;
            target = rule.subject + ":" + rule.metric;
        } else if (rule.operation == "promise.overdue") {
            std::size_t overdue = 0;
            for (const auto& [id, promise] : state.promises) {
                (void)id;
                if ((promise.status == PromiseStatus::Accepted ||
                     promise.status == PromiseStatus::Renegotiated) &&
                    state.now.minute >= promise.due.minute) {
                    ++overdue;
                }
            }
            observed = static_cast<double>(overdue);
            triggered = static_cast<double>(overdue) > rule.threshold;
            target = "promises";
        }
        if (!triggered) continue;
        const auto evidence = evidenceFor(state, rule.subject, target);
        findings.push_back(AnomalyFinding{rule.id, rule.operation, rule.subject, rule.metric, observed,
                                          rule.threshold, rule.severity, rule.description,
                                          evidence.sequence, evidence.type});
    }
    std::sort(findings.begin(), findings.end(), [](const auto& left, const auto& right) {
        if (left.severity == right.severity) return left.ruleId < right.ruleId;
        const auto rank = [](const std::string& value) {
            return value == "critical" ? 0 : value == "warning" ? 1 : 2;
        };
        return rank(left.severity) < rank(right.severity);
    });
    return findings;
}

}  // namespace tide_marks
