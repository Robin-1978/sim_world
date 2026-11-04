#include "world/contract_system.hpp"

#include "world/ids.hpp"
#include "world/world_service.hpp"

#include <algorithm>

namespace tide_marks {
namespace {

void applyRelationshipDelta(RelationshipAxes& axes, const RelationshipDelta& delta) {
    axes.trust = std::clamp(axes.trust + delta.trust, 0.0, 1.0);
    axes.respect = std::clamp(axes.respect + delta.respect, 0.0, 1.0);
    axes.fear = std::clamp(axes.fear + delta.fear, 0.0, 1.0);
    axes.owed = std::clamp(axes.owed + delta.owed, 0.0, 1.0);
}

Json quantityMapJson(const std::map<std::string, int>& quantities) {
    Json result = Json::object();
    for (const auto& [resource, amount] : quantities) result[resource] = amount;
    return result;
}

std::string canonicalActorId(const WorldState& state, const std::string& value) {
    if (value == kWorldActor || value == kPlayerId) return value;
    if (state.npcs.contains(value)) return value;
    if (value.starts_with("npc:")) {
        const auto npcId = value.substr(4);
        if (state.npcs.contains(npcId)) return npcId;
    }
    return {};
}

bool actorIsActive(const WorldState& state, const std::string& actorId) {
    if (actorId == kWorldActor) return true;
    const auto entityId = actorId == kPlayerId ? actorId : npcEntityId(actorId);
    const auto entity = state.entities.find(entityId);
    return entity != state.entities.end() && entity->second.lifecycle == EntityLifecycle::Active;
}

}  // namespace

void ContractSystem::applyPromiseDeadline() {
    auto& state = world_.state_;
    for (auto& [id, promise] : state.promises) {
        if ((promise.status == PromiseStatus::Accepted || promise.status == PromiseStatus::Renegotiated) &&
            state.now.minute >= promise.due.minute) {
            promise.status = PromiseStatus::Broken;
            ++promise.version;
            auto from = state.npcs.find(promise.from);
            auto to = state.npcs.find(promise.to);
            if (from != state.npcs.end()) {
                applyRelationshipDelta(from->second.relationships[promise.to],
                                       world_.content_.promisePolicy.brokenPromiser);
                ++from->second.stateVersion;
            }
            if (to != state.npcs.end()) {
                applyRelationshipDelta(to->second.relationships[promise.from],
                                       world_.content_.promisePolicy.brokenPromisee);
                ++to->second.stateVersion;
            }
            world_.addEvent("promise.broken", promise.from, promise.to,
                            "A promise reached its deadline", Json{{"promise_id", id}});
        }
    }
}

void ContractSystem::applyExchangeDeadlines() {
    auto& state = world_.state_;
    for (auto& [id, contract] : state.exchangeContracts) {
        if ((contract.status != ExchangeContractStatus::Proposed &&
             contract.status != ExchangeContractStatus::Accepted) ||
            state.now.minute < contract.due.minute) {
            continue;
        }
        contract.status = ExchangeContractStatus::Expired;
        ++contract.version;
        contract.reason = "deadline_reached";
        world_.addEvent("exchange.expired", kWorldActor, id,
                        "An exchange contract reached its deadline before settlement",
                        Json{{"contract_id", id}, {"proposer", contract.proposer},
                             {"counterparty", contract.counterparty}, {"due", contract.due.minute}});
    }
}

CommandResult ContractSystem::createPromise(const std::string& from, const std::string& to,
                                            const std::string& description, int dueInDays,
                                            const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    auto& state = world_.state_;
    const bool validFrom = from == kPlayerId || world_.hasNpc(from);
    const bool validTo = to == kPlayerId || world_.hasNpc(to);
    if (!validFrom || !validTo || from == to) {
        return world_.reject("unknown_actor", "Both promise parties must be distinct.");
    }
    if (!turnId.empty() && state.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The turn was already applied.");
    }
    const std::string id = "promise-" + std::to_string(state.promises.size() + 1);
    Promise promise{id, from, to, description,
                    GameTime{state.now.minute + static_cast<std::int64_t>(std::max(1, dueInDays)) * kMinutesPerDay},
                    PromiseStatus::Proposed, 0};
    state.promises.emplace(id, promise);
    if (world_.hasNpc(from)) state.npcs.at(from).openPromises.push_back(id);
    if (world_.hasNpc(to)) state.npcs.at(to).openPromises.push_back(id);
    if (!turnId.empty()) state.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("promise.created", from, to, description,
                                      Json{{"promise_id", id}, {"due", promise.due.minute}});
    return CommandResult{true, "ok", id, event};
}

CommandResult ContractSystem::decidePromise(const std::string& promiseId, PromiseStatus decision,
                                            const std::string& actor, const std::string& turnId,
                                            const std::string& detail) {
    WorldService::TransactionScope transaction(world_);
    auto& state = world_.state_;
    if (!turnId.empty() && state.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The turn was already applied.");
    }
    auto found = state.promises.find(promiseId);
    if (found == state.promises.end()) return world_.reject("unknown_promise", "That promise does not exist.");
    auto& promise = found->second;
    if (actor != kPlayerId && actor != promise.from && actor != promise.to) {
        return world_.reject("unauthorized", "Only a promise party can change it.");
    }
    if (promise.status == PromiseStatus::Broken || promise.status == PromiseStatus::Refused ||
        promise.status == PromiseStatus::Fulfilled) {
        return world_.reject("stale_promise", "That promise is already closed.");
    }
    if (decision == PromiseStatus::Renegotiated) {
        promise.due.minute += world_.content_.promisePolicy.renegotiationExtensionMinutes;
        promise.status = PromiseStatus::Renegotiated;
    } else {
        promise.status = decision;
    }
    ++promise.version;
    if (!turnId.empty()) state.processedTurnIds.insert(turnId);
    if (auto npc = state.npcs.find(actor); npc != state.npcs.end()) {
        ++npc->second.stateVersion;
        npc->second.memories.push_back(Memory{"promise", detail.empty() ? "A promise changed status." : detail,
                                             state.now, 1.0});
        const auto counterparty = actor == promise.from ? promise.to : promise.from;
        if (decision == PromiseStatus::Refused || decision == PromiseStatus::Broken) {
            applyRelationshipDelta(npc->second.relationships[counterparty], world_.content_.promisePolicy.refusedActor);
        } else {
            applyRelationshipDelta(npc->second.relationships[counterparty], world_.content_.promisePolicy.acceptedActor);
        }
    }
    const auto event = world_.addEvent("promise." + toString(decision), actor, promiseId,
                                      detail.empty() ? "Promise status changed" : detail,
                                      Json{{"promise_id", promiseId}, {"status", toString(decision)},
                                           {"version", promise.version}});
    return CommandResult{true, "ok", toString(decision), event};
}

CommandResult ContractSystem::proposeExchange(const std::string& proposer,
                                              const std::string& counterparty,
                                              std::map<std::string, int> offered,
                                              std::map<std::string, int> requested,
                                              int dueInMinutes, const std::string& actor,
                                              const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    auto& state = world_.state_;
    if (!turnId.empty() && state.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The exchange proposal was already applied.");
    }
    const auto proposerId = canonicalActorId(state, proposer);
    const auto counterpartyId = canonicalActorId(state, counterparty);
    const auto actorId = canonicalActorId(state, actor);
    if (proposerId.empty() || counterpartyId.empty() || proposerId == counterpartyId) {
        return world_.reject("unknown_exchange_party", "An exchange needs two distinct active parties.");
    }
    if (actorId != kWorldActor && actorId != proposerId) {
        return world_.reject("unauthorized", "Only the proposer or world authority may create this exchange.");
    }
    if (!actorIsActive(state, proposerId) || !actorIsActive(state, counterpartyId)) {
        return world_.reject("inactive_exchange_party", "Both exchange parties must be active.");
    }
    if (dueInMinutes <= 0) return world_.reject("invalid_exchange_deadline", "An exchange deadline must be positive.");
    const auto validTerms = [this](const std::map<std::string, int>& terms) {
        return std::all_of(terms.begin(), terms.end(), [this](const auto& term) {
            return world_.content_.resources.contains(term.first) && term.second > 0;
        });
    };
    if (offered.empty() || requested.empty() || !validTerms(offered) || !validTerms(requested)) {
        return world_.reject("invalid_exchange_terms",
                             "An exchange needs positive terms on both sides using known resources.");
    }
    std::size_t sequence = state.exchangeContracts.size() + 1;
    std::string id = "exchange-" + std::to_string(sequence);
    while (state.exchangeContracts.contains(id)) id = "exchange-" + std::to_string(++sequence);
    ExchangeContract contract{id, proposerId, counterpartyId, std::move(offered), std::move(requested),
                              state.now, GameTime{state.now.minute + dueInMinutes}, GameTime{-1},
                              ExchangeContractStatus::Proposed, 0, "awaiting_counterparty"};
    state.exchangeContracts.emplace(id, contract);
    if (!turnId.empty()) state.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(
        "exchange.proposed", proposerId, counterpartyId, "An actor proposed an atomic two-party exchange",
        Json{{"contract_id", id}, {"offered", quantityMapJson(contract.offered)},
             {"requested", quantityMapJson(contract.requested)}, {"due", contract.due.minute}});
    return CommandResult{true, "ok", id, event};
}

CommandResult ContractSystem::decideExchange(const std::string& contractId,
                                             ExchangeContractStatus decision,
                                             const std::string& actor,
                                             const std::string& turnId,
                                             const std::string& reason) {
    WorldService::TransactionScope transaction(world_);
    auto& state = world_.state_;
    if (!turnId.empty() && state.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The exchange transition was already applied.");
    }
    const auto found = state.exchangeContracts.find(contractId);
    if (found == state.exchangeContracts.end()) return world_.reject("unknown_exchange", "That exchange does not exist.");
    auto& contract = found->second;
    if (state.now.minute >= contract.due.minute || contract.status == ExchangeContractStatus::Expired) {
        return world_.reject("exchange_expired", "The exchange deadline has passed.");
    }
    const auto actorId = canonicalActorId(state, actor);
    if (actorId.empty()) return world_.reject("unauthorized", "The actor is not a party to this exchange.");
    const bool authority = actorId == kWorldActor;
    const bool isProposer = actorId == contract.proposer;
    const bool isCounterparty = actorId == contract.counterparty;
    if (!authority && !isProposer && !isCounterparty) return world_.reject("unauthorized", "The actor is not a party to this exchange.");
    if ((decision == ExchangeContractStatus::Accepted || decision == ExchangeContractStatus::Rejected) &&
        !authority && !isCounterparty) return world_.reject("counterparty_required", "Only the counterparty may accept or reject an exchange.");
    if ((decision == ExchangeContractStatus::Accepted || decision == ExchangeContractStatus::Rejected) &&
        (!actorIsActive(state, contract.proposer) || !actorIsActive(state, contract.counterparty))) {
        return world_.reject("inactive_exchange_party", "Both exchange parties must be active to decide a proposal.");
    }
    if (decision == ExchangeContractStatus::Cancelled && !authority && !actorIsActive(state, actorId)) {
        return world_.reject("inactive_exchange_party", "An inactive exchange party cannot cancel a contract.");
    }
    if (decision == ExchangeContractStatus::Cancelled && !authority &&
        contract.status != ExchangeContractStatus::Proposed && contract.status != ExchangeContractStatus::Accepted) {
        return world_.reject("invalid_exchange_transition", "Only a live exchange can be cancelled.");
    }
    if (decision == ExchangeContractStatus::Accepted && contract.status == ExchangeContractStatus::Proposed) {
        contract.status = ExchangeContractStatus::Accepted;
    } else if (decision == ExchangeContractStatus::Rejected && contract.status == ExchangeContractStatus::Proposed) {
        contract.status = ExchangeContractStatus::Rejected;
    } else if (decision == ExchangeContractStatus::Cancelled &&
               (contract.status == ExchangeContractStatus::Proposed || contract.status == ExchangeContractStatus::Accepted)) {
        contract.status = ExchangeContractStatus::Cancelled;
    } else {
        return world_.reject("invalid_exchange_transition", "The requested exchange transition is not legal.");
    }
    contract.reason = reason;
    ++contract.version;
    if (!turnId.empty()) state.processedTurnIds.insert(turnId);
    const auto eventType = decision == ExchangeContractStatus::Accepted ? "exchange.accepted" :
                           decision == ExchangeContractStatus::Rejected ? "exchange.rejected" : "exchange.cancelled";
    const auto event = world_.addEvent(eventType, actorId, contractId,
                                       reason.empty() ? "An exchange lifecycle state changed" : reason,
                                       Json{{"contract_id", contractId}, {"status", toString(contract.status)}});
    return CommandResult{true, "ok", contractId, event};
}

CommandResult ContractSystem::settleExchange(const std::string& contractId,
                                             const std::string& actor,
                                             const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    auto& state = world_.state_;
    if (!turnId.empty() && state.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The exchange settlement was already applied.");
    }
    const auto found = state.exchangeContracts.find(contractId);
    if (found == state.exchangeContracts.end()) return world_.reject("unknown_exchange", "That exchange does not exist.");
    auto& contract = found->second;
    if (contract.status != ExchangeContractStatus::Accepted) return world_.reject("invalid_exchange_transition", "Only an accepted exchange can settle.");
    if (state.now.minute >= contract.due.minute) return world_.reject("exchange_expired", "The exchange deadline has passed.");
    const auto actorId = canonicalActorId(state, actor);
    if (actorId.empty()) return world_.reject("unauthorized", "The actor is not a party to this exchange.");
    if (actorId != kWorldActor && actorId != contract.proposer && actorId != contract.counterparty) {
        return world_.reject("unauthorized", "The actor is not a party to this exchange.");
    }
    if (!actorIsActive(state, contract.proposer) || !actorIsActive(state, contract.counterparty)) {
        return world_.reject("inactive_exchange_party", "Both exchange parties must be active to settle.");
    }
    for (const auto& [resource, amount] : contract.offered) {
        const auto transfer = world_.transferResource(contract.proposer, contract.counterparty, resource, amount, actorId);
        if (!transfer.accepted) {
            transaction.rollback();
            world_.addEvent("exchange.settlement_rejected", actorId, contractId,
                            "An accepted exchange could not deliver both sides",
                            Json{{"contract_id", contractId}, {"code", transfer.code},
                                 {"message", transfer.message}, {"proposer", contract.proposer},
                                 {"counterparty", contract.counterparty}});
            return world_.reject(transfer.code, transfer.message);
        }
    }
    for (const auto& [resource, amount] : contract.requested) {
        const auto transfer = world_.transferResource(contract.counterparty, contract.proposer, resource, amount, actorId);
        if (!transfer.accepted) {
            transaction.rollback();
            world_.addEvent("exchange.settlement_rejected", actorId, contractId,
                            "An accepted exchange could not deliver both sides",
                            Json{{"contract_id", contractId}, {"code", transfer.code},
                                 {"message", transfer.message}, {"proposer", contract.proposer},
                                 {"counterparty", contract.counterparty}});
            return world_.reject(transfer.code, transfer.message);
        }
    }
    contract.status = ExchangeContractStatus::Settled;
    contract.settledAt = state.now;
    contract.reason = "settled";
    ++contract.version;
    if (!turnId.empty()) state.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(
        "exchange.settled", actorId, contractId,
        "Both parties delivered their contractual resources atomically",
        Json{{"contract_id", contractId}, {"proposer", contract.proposer},
             {"counterparty", contract.counterparty}, {"offered", quantityMapJson(contract.offered)},
             {"requested", quantityMapJson(contract.requested)}});
    return CommandResult{true, "ok", contractId, event};
}

}  // namespace tide_marks
