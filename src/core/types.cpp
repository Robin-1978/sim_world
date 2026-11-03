#include "core/types.hpp"
#include <algorithm>
#include <sstream>

namespace tide_marks {
int GameTime::day() const {
    return static_cast<int>(minute / 1440) + 1;
}

int GameTime::hour() const {
    return static_cast<int>((minute % 1440) / 60);
}

int GameTime::minuteOfHour() const {
    return static_cast<int>(minute % 60);
}

std::string GameTime::toString() const {
    std::ostringstream output;
    output << "day " << day() << ' ' << hour() << ':';
    if (minuteOfHour() < 10) {
        output << '0';
    }
    output << minuteOfHour();
    return output.str();
}

GameTime GameTime::fromDayHour(int dayValue, int hourValue, int minuteValue) {
    return GameTime{static_cast<std::int64_t>(std::max(1, dayValue) - 1) * 1440 +
                    static_cast<std::int64_t>(hourValue) * 60 + minuteValue};
}

std::string toString(PromiseStatus status) {
    switch (status) {
    case PromiseStatus::Proposed: return "proposed";
    case PromiseStatus::Accepted: return "accepted";
    case PromiseStatus::Renegotiated: return "renegotiated";
    case PromiseStatus::Refused: return "refused";
    case PromiseStatus::Fulfilled: return "fulfilled";
    case PromiseStatus::Broken: return "broken";
    }
    return "unknown";
}

std::string toString(EntityLifecycle lifecycle) {
    switch (lifecycle) {
        case EntityLifecycle::Active: return "active";
        case EntityLifecycle::Dormant: return "dormant";
        case EntityLifecycle::Destroyed: return "destroyed";
    }
    return "active";
}

EntityLifecycle entityLifecycleFromString(const std::string& value) {
    if (value == "dormant") return EntityLifecycle::Dormant;
    if (value == "destroyed") return EntityLifecycle::Destroyed;
    return EntityLifecycle::Active;
}

PromiseStatus promiseStatusFromString(const std::string& value) {
    if (value == "accepted") return PromiseStatus::Accepted;
    if (value == "renegotiated") return PromiseStatus::Renegotiated;
    if (value == "refused") return PromiseStatus::Refused;
    if (value == "fulfilled") return PromiseStatus::Fulfilled;
    if (value == "broken") return PromiseStatus::Broken;
    return PromiseStatus::Proposed;
}


}  // namespace tide_marks
