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

// Evolution requests carry an immutable, already-projected context. The
// worker never receives WorldService and therefore cannot observe or mutate
// facts while the world continues to advance.
struct EvolutionRequest {
    std::string requestId;
    std::string key;
    std::string evolutionLevel;
    std::string targetNpcId;
    std::string trigger;
    std::string baseContentVersion;
    std::uint64_t baseWorldRevision{0};
    GameTime createdAt;
    GameTime deadline;
    int attempt{0};
    bool coalescePending{true};
    Json context = Json::object();
    Json validationFeedback = Json::object();
};

struct EvolutionSubmitResult {
    bool accepted{false};
    std::vector<std::string> supersededRequestIds;
};

struct EvolutionResult {
    EvolutionRequest request;
    ProviderResponse response;
    std::string provider;
};

// A single serial worker keeps local llama.cpp calls ordered with NPC turns
// while remaining independent from the simulation transaction and scheduler.
class EvolutionQueue {
public:
    explicit EvolutionQueue(IProvider& provider);
    ~EvolutionQueue();

    EvolutionQueue(const EvolutionQueue&) = delete;
    EvolutionQueue& operator=(const EvolutionQueue&) = delete;

    [[nodiscard]] EvolutionSubmitResult submit(EvolutionRequest request);
    [[nodiscard]] std::vector<EvolutionResult> drain();
    [[nodiscard]] std::size_t pending() const;

private:
    void run();

    IProvider& provider_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<EvolutionRequest> pending_;
    std::deque<EvolutionResult> ready_;
    std::size_t inFlight_{0};
    bool stopping_{false};
    std::thread worker_;
};

}  // namespace tide_marks
