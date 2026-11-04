#include "npc/conversation.hpp"

#include <algorithm>

namespace tide_marks {

SpeechAct speechActFromString(const std::string& value) {
    if (value == "greeting") return SpeechAct::Greeting;
    if (value == "inquiry") return SpeechAct::Inquiry;
    if (value == "information_request") return SpeechAct::InformationRequest;
    if (value == "request") return SpeechAct::Request;
    if (value == "command") return SpeechAct::Command;
    if (value == "negotiation") return SpeechAct::Negotiation;
    if (value == "promise") return SpeechAct::Promise;
    if (value == "farewell") return SpeechAct::Farewell;
    if (value == "statement") return SpeechAct::Statement;
    return SpeechAct::Unknown;
}

std::string toString(SpeechAct act) {
    switch (act) {
        case SpeechAct::Greeting: return "greeting";
        case SpeechAct::Inquiry: return "inquiry";
        case SpeechAct::InformationRequest: return "information_request";
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

}  // namespace tide_marks
