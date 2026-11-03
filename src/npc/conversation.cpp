#include "npc/conversation.hpp"

#include <algorithm>
#include <cctype>

namespace tide_marks {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool containsAny(const std::string& value, const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(), [&value](const auto& pattern) {
        return !pattern.empty() && value.find(lower(pattern)) != std::string::npos;
    });
}

}  // namespace

SpeechAct speechActFromString(const std::string& value) {
    if (value == "greeting") return SpeechAct::Greeting;
    if (value == "inquiry") return SpeechAct::Inquiry;
    if (value == "request") return SpeechAct::Request;
    if (value == "command") return SpeechAct::Command;
    if (value == "negotiation") return SpeechAct::Negotiation;
    if (value == "promise") return SpeechAct::Promise;
    if (value == "farewell") return SpeechAct::Farewell;
    if (value == "statement") return SpeechAct::Statement;
    return SpeechAct::Unknown;
}

SpeechAct classifySpeechAct(const std::string& input, const ConversationPolicy& policy) {
    const auto value = lower(input);
    if (value.empty()) return SpeechAct::Unknown;
    for (const auto& rule : policy.rules) {
        if (containsAny(value, rule.patterns)) return rule.act;
    }
    return SpeechAct::Statement;
}

std::string toString(SpeechAct act) {
    switch (act) {
        case SpeechAct::Greeting: return "greeting";
        case SpeechAct::Inquiry: return "inquiry";
        case SpeechAct::Request: return "request";
        case SpeechAct::Command: return "command";
        case SpeechAct::Negotiation: return "negotiation";
        case SpeechAct::Promise: return "promise";
        case SpeechAct::Farewell: return "farewell";
        case SpeechAct::Statement: return "statement";
        case SpeechAct::Unknown: return "unknown";
    }
    return "unknown";
}

bool permitsWorldAction(SpeechAct act, const ConversationPolicy& policy) {
    const auto found = std::find_if(policy.rules.begin(), policy.rules.end(),
                                    [act](const auto& rule) { return rule.act == act; });
    return found == policy.rules.end() ? policy.defaultWorldActionAllowed : found->worldActionAllowed;
}

bool isConversationOnly(SpeechAct act, const ConversationPolicy& policy) {
    return !permitsWorldAction(act, policy);
}

}  // namespace tide_marks
