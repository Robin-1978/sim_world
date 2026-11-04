#include "world/command_dispatcher.hpp"

#include "world/world_service.hpp"

#include <map>
#include <stdexcept>

namespace tide_marks {

CommandResult CommandDispatcher::submit(const Command& command) {
    if (command.expectedWorldRevision != 0 && command.expectedWorldRevision != world_.state_.revision) {
        return world_.reject("state_conflict", "The world changed before this command was committed.");
    }
    if (command.expectedNpcStateVersion != 0 && world_.hasNpc(command.actor) &&
        command.expectedNpcStateVersion != world_.state_.npcs.at(command.actor).stateVersion) {
        return world_.reject("state_conflict", "The NPC changed before this command was committed.");
    }
    if (!command.turnId.empty() && world_.state_.processedTurnIds.contains(command.turnId)) {
        return world_.reject("duplicate_turn", "The turn was already applied.");
    }
    if (command.kind == "advance") {
        std::int64_t minutes = 0;
        try {
            minutes = std::stoll(command.detail);
        } catch (...) {
            return world_.reject("invalid_duration", "Advance duration must be minutes.");
        }
        if (minutes <= 0) return world_.reject("invalid_duration", "Advance duration must be positive minutes.");
        world_.advanceMinutes(minutes);
        if (!command.turnId.empty()) {
            WorldService::TransactionScope transaction(world_);
            world_.state_.processedTurnIds.insert(command.turnId);
        }
        return CommandResult{true, "ok", "World advanced.", std::nullopt};
    }
    if (command.kind == "promise_accept" || command.kind == "promise_refuse" ||
        command.kind == "promise_renegotiate" || command.kind == "promise_fulfill" || command.kind == "promise_break") {
        PromiseStatus status = PromiseStatus::Accepted;
        if (command.kind == "promise_refuse") status = PromiseStatus::Refused;
        if (command.kind == "promise_renegotiate") status = PromiseStatus::Renegotiated;
        if (command.kind == "promise_fulfill") status = PromiseStatus::Fulfilled;
        if (command.kind == "promise_break") status = PromiseStatus::Broken;
        return world_.decidePromise(command.promiseId, status, command.actor, command.turnId, command.detail);
    }
    if (command.kind == "entity_create") {
        return world_.createEntity(command.target, command.detail, Json::object(), command.turnId);
    }
    if (command.kind == "entity_activate" || command.kind == "entity_dormant" || command.kind == "entity_destroy") {
        const auto lifecycle = command.kind == "entity_activate" ? EntityLifecycle::Active
                              : command.kind == "entity_dormant" ? EntityLifecycle::Dormant
                                                                  : EntityLifecycle::Destroyed;
        return world_.setEntityLifecycle(command.target, lifecycle, command.turnId);
    }
    if (command.kind == "link_open" || command.kind == "link_close" ||
        command.kind == "link_lock" || command.kind == "link_unlock") {
        const auto state = world_.state_.links.find(command.target);
        if (state == world_.state_.links.end()) return world_.reject("unknown_link", "That link does not exist.");
        const bool traversable = command.kind == "link_open" ? true
                              : command.kind == "link_close" ? false : state->second.traversable;
        const bool locked = command.kind == "link_lock" ? true
                          : command.kind == "link_unlock" ? false : state->second.locked;
        return world_.setLinkState(command.target, traversable, locked, command.turnId);
    }
    if (command.kind == "resource_transfer") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto source = args.value("source", command.actor);
            const auto target = args.value("target", command.target);
            const auto resource = args.value("resource", std::string{});
            const auto amount = args.value("amount", 0);
            return world_.transferResource(source, target, resource, amount, command.actor, command.turnId);
        } catch (...) {
            return world_.reject("invalid_resource_transfer", "Resource transfer detail must be valid JSON.");
        }
    }
    if (command.kind == "npc_move") {
        return world_.moveNpc(command.actor, command.target, command.actor, command.turnId);
    }
    if (command.kind == "security") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto operation = args.value("operation", std::string{});
            const auto target = args.value("target", command.target);
            if (operation.empty()) return world_.reject("invalid_security_operator", "Security command needs an operation.");
            return world_.applySecurityOperator(operation, command.actor, target, args, command.turnId);
        } catch (...) {
            return world_.reject("invalid_security_operator", "Security command detail must be valid JSON.");
        }
    }
    if (command.kind == "knowledge_assert" || command.kind == "experience_record") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto npcId = args.value("npc", command.target);
            const auto sourceSequence = args.value("source_sequence", 0ULL);
            const auto confidence = args.value("confidence", 0.0);
            if (command.kind == "knowledge_assert") {
                return world_.assertKnownFact(npcId, args.value("subject", std::string{}),
                                       args.value("predicate", std::string{}),
                                       args.value("value", std::string{}), sourceSequence,
                                       args.value("scope", "direct"), confidence,
                                       command.actor, command.turnId);
            }
            return world_.recordExperience(npcId, args.value("domain", std::string{}),
                                    args.value("trigger", std::string{}),
                                    args.value("action", std::string{}),
                                    args.value("outcome", std::string{}), sourceSequence,
                                    confidence, args.value("personal", true),
                                    command.actor, command.turnId);
        } catch (...) {
            return world_.reject("invalid_knowledge_action", "Knowledge detail must be valid JSON.");
        }
    }
    if (command.kind == "relationship_adjust") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto delta = RelationshipAxes{args.value("trust", 0.0), args.value("respect", 0.0),
                                                args.value("fear", 0.0), args.value("owed", 0.0)};
            return world_.adjustRelationship(args.value("npc", command.actor), args.value("target", command.target),
                                      delta, command.actor, command.turnId, args.value("reason", ""));
        } catch (...) {
            return world_.reject("invalid_relationship", "Relationship detail must be valid JSON.");
        }
    }
    if (command.kind == "group_metric") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            return world_.adjustGroupMetric(args.value("group", command.target), args.value("metric", ""),
                                     args.value("delta", 0.0), command.actor, command.turnId,
                                     args.value("reason", ""));
        } catch (...) {
            return world_.reject("invalid_group_metric", "Group metric detail must be valid JSON.");
        }
    }
    if (command.kind == "group_membership") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            return world_.setGroupMembership(args.value("group", command.target), args.value("npc", command.actor),
                                      args.value("role", "member"), args.value("active", true),
                                      command.actor, command.turnId);
        } catch (...) {
            return world_.reject("invalid_group_membership", "Group membership detail must be valid JSON.");
        }
    }
    if (command.kind == "trade") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            return world_.tradeResources(args.value("seller", command.actor), args.value("buyer", command.target),
                                  args.value("seller_resource", ""), args.value("seller_amount", 0),
                                  args.value("buyer_resource", ""), args.value("buyer_amount", 0),
                                  command.actor, command.turnId);
        } catch (...) {
            return world_.reject("invalid_trade", "Trade detail must be valid JSON.");
        }
    }
    if (command.kind == "exchange_propose") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto parseTerms = [](const Json& value) {
                std::map<std::string, int> terms;
                if (!value.is_object()) throw std::runtime_error("exchange terms must be objects");
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (!it.value().is_number_integer()) throw std::runtime_error("exchange quantities must be integers");
                    terms[it.key()] = it.value().get<int>();
                }
                return terms;
            };
            const auto proposer = args.value("proposer", command.actor);
            return world_.proposeExchange(proposer, args.value("counterparty", command.target),
                                   parseTerms(args.value("offered", Json::object())),
                                   parseTerms(args.value("requested", Json::object())),
                                   args.value("due_minutes", 0), command.actor, command.turnId);
        } catch (...) {
            return world_.reject("invalid_exchange", "Exchange proposal detail must contain valid JSON terms.");
        }
    }
    if (command.kind == "exchange_transition") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto status = exchangeContractStatusFromString(args.value("status", ""));
            if (status == ExchangeContractStatus::Proposed || status == ExchangeContractStatus::Settled ||
                status == ExchangeContractStatus::Expired) {
                return world_.reject("invalid_exchange_transition", "Use accept, reject or cancel for a transition.");
            }
            return world_.decideExchange(args.value("contract_id", command.target), status, command.actor,
                                  command.turnId, args.value("reason", ""));
        } catch (...) {
            return world_.reject("invalid_exchange", "Exchange transition detail must be valid JSON.");
        }
    }
    if (command.kind == "exchange_settle") {
        return world_.settleExchange(command.target, command.actor, command.turnId);
    }
    return world_.reject("unknown_command", "The command is not part of the registered action vocabulary.");
}

}  // namespace tide_marks
