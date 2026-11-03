#pragma once

#include <string>
#include <vector>

namespace tide_marks {

enum class SpeechAct {
    Greeting,
    Inquiry,
    Request,
    Command,
    Negotiation,
    Promise,
    Farewell,
    Statement,
    Unknown,
};

struct SpeechActRule {
    SpeechAct act{SpeechAct::Unknown};
    bool worldActionAllowed{false};
    std::vector<std::string> patterns;
};

struct ConversationPolicy {
    bool defaultWorldActionAllowed{false};
    std::vector<SpeechActRule> rules;
};

[[nodiscard]] SpeechAct speechActFromString(const std::string& value);
[[nodiscard]] SpeechAct classifySpeechAct(const std::string& input,
                                          const ConversationPolicy& policy);
[[nodiscard]] std::string toString(SpeechAct act);
[[nodiscard]] bool permitsWorldAction(SpeechAct act, const ConversationPolicy& policy);
[[nodiscard]] bool isConversationOnly(SpeechAct act, const ConversationPolicy& policy);

}  // namespace tide_marks
