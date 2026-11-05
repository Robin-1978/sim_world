#include "world/world_service.hpp"

#include "world/command_dispatcher.hpp"
#include "world/contract_system.hpp"
#include "world/world_command_system.hpp"

namespace tide_marks {

CommandResult WorldService::proposeExchange(const std::string& proposer,
                                            const std::string& counterparty,
                                            std::map<std::string, int> offered,
                                            std::map<std::string, int> requested,
                                            int dueInMinutes, const std::string& actor,
                                            const std::string& turnId) {
    return ContractSystem(*this).proposeExchange(proposer, counterparty, std::move(offered),
                                                 std::move(requested), dueInMinutes, actor, turnId);
}

CommandResult WorldService::decideExchange(const std::string& contractId,
                                           ExchangeContractStatus decision,
                                           const std::string& actor,
                                           const std::string& turnId,
                                           const std::string& reason) {
    return ContractSystem(*this).decideExchange(contractId, decision, actor, turnId, reason);
}

CommandResult WorldService::settleExchange(const std::string& contractId,
                                           const std::string& actor,
                                           const std::string& turnId) {
    return ContractSystem(*this).settleExchange(contractId, actor, turnId);
}

CommandResult WorldService::reject(std::string code, std::string message) const {
    return CommandResult{false, std::move(code), std::move(message), std::nullopt};
}

CommandResult WorldService::submit(const Command& command) {
    return CommandDispatcher(*this).submit(command);
}

CommandResult WorldService::createPromise(const std::string& from, const std::string& to,
                                           const std::string& description, int dueInDays,
                                           const std::string& turnId) {
    return ContractSystem(*this).createPromise(from, to, description, dueInDays, turnId);
}

CommandResult WorldService::decidePromise(const std::string& promiseId, PromiseStatus decision,
                                          const std::string& actor, const std::string& turnId,
                                          const std::string& detail) {
    return ContractSystem(*this).decidePromise(promiseId, decision, actor, turnId, detail);
}

CommandResult WorldService::createEntity(const std::string& id, const std::string& archetypeId,
                                         Json metadata, const std::string& turnId) {
    return WorldCommandSystem(*this).createEntity(id, archetypeId, std::move(metadata), turnId);
}

CommandResult WorldService::setEntityLifecycle(const std::string& id, EntityLifecycle lifecycle,
                                               const std::string& turnId) {
    return WorldCommandSystem(*this).setEntityLifecycle(id, lifecycle, turnId);
}

CommandResult WorldService::setLinkState(const std::string& linkId, bool traversable, bool locked,
                                         const std::string& turnId) {
    return WorldCommandSystem(*this).setLinkState(linkId, traversable, locked, turnId);
}

CommandResult WorldService::transferResource(const std::string& source, const std::string& target,
                                             const std::string& resource, int amount,
                                             const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).transferResource(source, target, resource, amount, actor, turnId);
}

CommandResult WorldService::tradeResources(const std::string& seller, const std::string& buyer,
                                           const std::string& sellerResource, int sellerAmount,
                                           const std::string& buyerResource, int buyerAmount,
                                           const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).tradeResources(seller, buyer, sellerResource, sellerAmount,
                                                    buyerResource, buyerAmount, actor, turnId);
}

CommandResult WorldService::adjustRelationship(const std::string& npcId, const std::string& target,
                                               RelationshipAxes delta, const std::string& actor,
                                               const std::string& turnId, const std::string& reason) {
    return WorldCommandSystem(*this).adjustRelationship(npcId, target, delta, actor, turnId, reason);
}

CommandResult WorldService::adjustGroupMetric(const std::string& groupId, const std::string& metric,
                                              double delta, const std::string& actor,
                                              const std::string& turnId, const std::string& reason) {
    return WorldCommandSystem(*this).adjustGroupMetric(groupId, metric, delta, actor, turnId, reason);
}

CommandResult WorldService::setGroupMembership(const std::string& groupId, const std::string& npcId,
                                               const std::string& role, bool active,
                                               const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).setGroupMembership(groupId, npcId, role, active, actor, turnId);
}

CommandResult WorldService::moveNpc(const std::string& npcId, const std::string& destination,
                                    const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).moveNpc(npcId, destination, actor, turnId);
}

}  // namespace tide_marks
