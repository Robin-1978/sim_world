#pragma once

#include "core/types.hpp"

#include <map>
#include <string>

namespace tide_marks {

class WorldService;
struct CommandResult;

// Coordinates promise and exchange lifecycles while leaving WorldState,
// transactions, revisions and EventLog ownership in WorldService.
class ContractSystem {
public:
    explicit ContractSystem(WorldService& world) : world_(world) {}

    CommandResult createPromise(const std::string& from, const std::string& to,
                                const std::string& description, int dueInDays,
                                const std::string& turnId);
    CommandResult decidePromise(const std::string& promiseId, PromiseStatus decision,
                                const std::string& actor, const std::string& turnId,
                                const std::string& detail);
    CommandResult proposeExchange(const std::string& proposer, const std::string& counterparty,
                                  std::map<std::string, int> offered,
                                  std::map<std::string, int> requested, int dueInMinutes,
                                  const std::string& actor, const std::string& turnId);
    CommandResult decideExchange(const std::string& contractId, ExchangeContractStatus decision,
                                 const std::string& actor, const std::string& turnId,
                                 const std::string& reason);
    CommandResult settleExchange(const std::string& contractId, const std::string& actor,
                                 const std::string& turnId);

    void applyPromiseDeadline();
    void applyExchangeDeadlines();

private:
    WorldService& world_;
};

}  // namespace tide_marks
