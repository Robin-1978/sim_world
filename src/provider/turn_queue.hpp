#pragma once

#include "provider/provider.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tide_marks {

// A queued request is an immutable decision snapshot.  The worker may spend
// arbitrary time in the Provider; it never receives a WorldService reference
// and therefore cannot mutate world facts.
struct TurnRequest {
    std::string turnId;
    std::string npcId;
    std::string channelId;
    std::string trigger;
    std::string playerInput;
    std::string contentVersion;
    std::string promptVersion;
    std::uint64_t worldRevision{0};
    std::uint64_t npcStateVersion{0};
    GameTime createdAt;
    GameTime deadline;
    bool coalescePending{false};
    // Empty for the first call. A validator may attach one bounded repair
    // hint before the request is resubmitted; it is never persisted as world
    // state or treated as model authority.
    Json validationFeedback = Json::object();
    DecisionContext context;
};

struct TurnSubmitResult {
    bool accepted{false};
    std::vector<std::string> supersededTurnIds;
};

struct TurnResult {
    TurnRequest request;
    ProviderResponse response;
    std::string provider;
};

// A single worker is intentional for the prototype: it preserves provider
// ordering and avoids concurrent calls into local llama.cpp.  The queue is
// still independent from the World Tick, so a slow request cannot hold a
// WorldState transaction open.
class TurnQueue {
public:
    explicit TurnQueue(IProvider& provider);
    ~TurnQueue();

    TurnQueue(const TurnQueue&) = delete;
    TurnQueue& operator=(const TurnQueue&) = delete;

    [[nodiscard]] TurnSubmitResult submit(TurnRequest request);
    [[nodiscard]] std::vector<TurnResult> drain();
    [[nodiscard]] std::size_t pending() const;

private:
    void run();

    IProvider& provider_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<TurnRequest> pending_;
    std::deque<TurnResult> ready_;
    bool stopping_{false};
    std::thread worker_;
};

}  // namespace tide_marks
