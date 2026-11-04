#include "world/meta_action_executor.hpp"

#include "world/ids.hpp"
#include "world/resource_system.hpp"
#include "world/social_system.hpp"

#include <algorithm>

namespace tide_marks {

namespace {
Json mergedArgs(const MetaActionDefinition& definition, const BehaviorNodeContent& node) {
    Json result = definition.defaultArgs;
    for (auto it = node.args.begin(); it != node.args.end(); ++it) result[it.key()] = it.value();
    return result;
}

bool parseTerms(const Json& value, std::map<std::string, int>& terms) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it.value().is_number_integer() || it.value().get<int>() <= 0) return false;
        terms[it.key()] = it.value().get<int>();
    }
    return true;
}

}  // namespace

bool MetaActionExecutor::execute(const WorldContentPack& content, WorldState& state,
                                 const std::string& npcId, const std::string& actionId,
                                 const MetaActionDefinition& definition,
                                 const BehaviorNodeContent& node,
                                 const std::map<std::string, int>* reservedResources,
                                 const Json* parameters, const Callbacks& callbacks) {
    Json args = mergedArgs(definition, node);
    if (parameters != nullptr && parameters->is_object()) {
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("bind")) continue;
            const auto binding = it.value().at("bind");
            if (!binding.is_string() || !parameters->contains(binding.get<std::string>())) return false;
            it.value() = parameters->at(binding.get<std::string>());
        }
    }
    const auto& operation = definition.operation;
    const auto emit = [&callbacks](const std::string& type, const std::string& actor,
                                   const std::string& target, const std::string& detail, Json data) {
        if (callbacks.emit) callbacks.emit(type, actor, target, detail, std::move(data));
    };
    if (operation == "resource.consume") {
        const auto resource = args.value("resource", "");
        const int quantity = args.value("amount", 0);
        const bool reserved = reservedResources != nullptr && reservedResources->contains(resource);
        if (reserved && reservedResources->at(resource) < quantity) return false;
        const int before = ResourceSystem::amount(state, resource);
        if (!ResourceSystem::consume(state, content, resource, quantity, reserved)) return false;
        emit("action.resource", npcId, resource, "A behavior recipe consumed a resource",
             Json{{"action_id", actionId}, {"resource", resource}, {"before", before},
                  {"after", ResourceSystem::amount(state, resource)}, {"amount", quantity}});
        return true;
    }
    if (operation == "resource.produce") {
        const auto resource = args.value("resource", "");
        const int quantity = args.value("amount", 0);
        const int before = ResourceSystem::amount(state, resource);
        if (!ResourceSystem::produce(state, content, resource, quantity)) return false;
        emit("action.resource", npcId, resource, "A behavior recipe produced a resource",
             Json{{"action_id", actionId}, {"resource", resource}, {"before", before},
                  {"after", ResourceSystem::amount(state, resource)}, {"amount", quantity}});
        return true;
    }
    if (operation == "resource.reserve") {
        const auto resource = args.value("resource", "");
        const int quantity = args.value("amount", 0);
        if (!ResourceSystem::reserve(state, content, resource, quantity)) return false;
        emit("action.reservation", npcId, resource, "A behavior recipe reserved a resource",
             Json{{"action_id", actionId}, {"resource", resource}, {"amount", quantity}});
        return true;
    }
    if (operation == "resource.release") {
        const auto resource = args.value("resource", "");
        const int quantity = args.value("amount", 0);
        if (!ResourceSystem::release(state, content, resource, quantity)) return false;
        emit("action.reservation", npcId, resource, "A behavior recipe released a resource reservation",
             Json{{"action_id", actionId}, {"resource", resource}, {"amount", quantity}});
        return true;
    }
    if (operation == "resource.transfer") {
        if (!callbacks.transfer) return false;
        const auto source = args.value("source", "self") == "self" ? npcId : args.value("source", "self");
        const auto target = args.value("target", "world") == "self" ? npcId : args.value("target", "world");
        return callbacks.transfer(Json{{"source", source}, {"target", target},
                                       {"resource", args.value("resource", "")},
                                       {"amount", args.value("amount", 0)}});
    }
    if (operation == "movement.traverse" || operation == "movement.arrive") {
        if (!callbacks.move) return false;
        const auto destination = args.value("destination", args.value("location", ""));
        return !destination.empty() && callbacks.move(Json{{"destination", destination}});
    }
    if (MetaActionRegistry::securityOperatorKind(operation) != MetaActionRegistry::SecurityOperatorKind::Unknown) {
        if (!callbacks.security) return false;
        return callbacks.security(Json{{"operation", operation}, {"target", args.value("target", "self")},
                                       {"args", args}});
    }
    if (operation == "memory.append") {
        const auto npc = state.npcs.find(npcId);
        const auto text = args.value("text", "");
        if (npc == state.npcs.end() || text.empty()) return false;
        const auto sourceSequence = args.value("source_sequence", 0ULL);
        if (sourceSequence != 0 && std::none_of(state.events.begin(), state.events.end(),
                                                [sourceSequence](const auto& event) {
                                                    return event.sequence == sourceSequence;
                                                })) return false;
        npc->second.memories.push_back(Memory{args.value("kind", "episode"), text, state.now,
                                              args.value("confidence", 1.0)});
        emit("memory.appended", npcId, args.value("target", "self"), text,
             Json{{"action_id", actionId}, {"kind", args.value("kind", "episode")},
                  {"source_sequence", sourceSequence}});
        return true;
    }
    if (operation == "knowledge.assert") {
        return callbacks.knownFact && callbacks.knownFact(Json{{"subject", args.value("subject", "")},
                                                               {"predicate", args.value("predicate", "")},
                                                               {"value", args.value("value", "")},
                                                               {"source_sequence", args.value("source_sequence", 0ULL)},
                                                               {"scope", args.value("scope", "direct")},
                                                               {"confidence", args.value("confidence", 0.0)}});
    }
    if (operation == "experience.record") {
        return callbacks.experience && callbacks.experience(Json{{"domain", args.value("domain", "")},
                                                                 {"trigger", args.value("trigger", "")},
                                                                 {"action", args.value("action", actionId)},
                                                                 {"outcome", args.value("outcome", "")},
                                                                 {"source_sequence", args.value("source_sequence", 0ULL)},
                                                                 {"confidence", args.value("confidence", 0.0)},
                                                                 {"personal", args.value("personal", true)}});
    }
    if (operation == "promise.create") {
        if (!callbacks.promiseCreate) return false;
        const auto resolve = [&npcId](const std::string& value) { return value == "self" ? npcId : value; };
        return callbacks.promiseCreate(Json{{"from", resolve(args.value("from", "self"))},
                                           {"to", resolve(args.value("to", ""))},
                                           {"description", args.value("description", "")},
                                           {"due_days", args.value("due_days", 1)}});
    }
    if (operation == "promise.transition") {
        return callbacks.promiseTransition && callbacks.promiseTransition(Json{{"status", args.value("status", "proposed")},
                                                                                 {"promise_id", args.value("promise_id", "")},
                                                                                 {"detail", args.value("detail", "")} });
    }
    if (operation == "production.execute") {
        const auto inputs = args.value("inputs", Json::object());
        const auto outputs = args.value("outputs", Json::object());
        if (!inputs.is_object() || !outputs.is_object() || (inputs.empty() && outputs.empty())) return false;
        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            if (!it.value().is_number_integer() || it.value().get<int>() <= 0 ||
                ResourceSystem::available(state, it.key()) < it.value().get<int>()) return false;
        }
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (!it.value().is_number_integer() || it.value().get<int>() <= 0 ||
                !content.resources.contains(it.key()) ||
                ResourceSystem::amount(state, it.key()) + it.value().get<int>() > content.resources.at(it.key()).maximum) return false;
        }
        Json consumed = Json::object();
        Json produced = Json::object();
        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            const int quantity = it.value().get<int>();
            if (!ResourceSystem::consume(state, content, it.key(), quantity, false)) return false;
            consumed[it.key()] = quantity;
        }
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            const int quantity = it.value().get<int>();
            if (!ResourceSystem::produce(state, content, it.key(), quantity)) return false;
            produced[it.key()] = quantity;
        }
        emit("production.executed", npcId, "world", "A registered production process exchanged inputs and outputs",
             Json{{"action_id", actionId}, {"inputs", consumed}, {"outputs", produced}});
        return true;
    }
    if (operation == "trade.execute") {
        if (!callbacks.trade) return false;
        const auto seller = args.value("seller", "self") == "self" ? npcId : args.value("seller", npcId);
        return callbacks.trade(Json{{"seller", seller}, {"buyer", args.value("buyer", "")},
                                    {"seller_resource", args.value("seller_resource", "")},
                                    {"seller_amount", args.value("seller_amount", 0)},
                                    {"buyer_resource", args.value("buyer_resource", "")},
                                    {"buyer_amount", args.value("buyer_amount", 0)}});
    }
    if (operation == "exchange.create") {
        if (!callbacks.exchangeCreate) return false;
        std::map<std::string, int> offered;
        std::map<std::string, int> requested;
        if (!parseTerms(args.value("offered", Json::object()), offered) ||
            !parseTerms(args.value("requested", Json::object()), requested)) return false;
        const auto resolve = [&npcId](const std::string& value) { return value == "self" ? npcId : value; };
        return callbacks.exchangeCreate(Json{{"proposer", resolve(args.value("proposer", "self"))},
                                             {"counterparty", resolve(args.value("counterparty", ""))},
                                             {"offered", offered}, {"requested", requested},
                                             {"due_minutes", args.value("due_minutes", 0)}});
    }
    if (operation == "exchange.transition") {
        if (!callbacks.exchangeTransition) return false;
        const auto status = exchangeContractStatusFromString(args.value("status", ""));
        if (status == ExchangeContractStatus::Proposed || status == ExchangeContractStatus::Settled ||
            status == ExchangeContractStatus::Expired) return false;
        return callbacks.exchangeTransition(Json{{"contract_id", args.value("contract_id", "")},
                                                  {"status", args.value("status", "")},
                                                  {"reason", args.value("reason", "")}});
    }
    if (operation == "exchange.settle") {
        return callbacks.exchangeSettle && callbacks.exchangeSettle(Json{{"contract_id", args.value("contract_id", "")}});
    }
    if (operation == "group.metric_adjust") {
        return callbacks.groupMetric && callbacks.groupMetric(Json{{"group", args.value("group", "")},
                                                                    {"metric", args.value("metric", "")},
                                                                    {"delta", args.value("delta", 0.0)},
                                                                    {"reason", args.value("reason", "")}});
    }
    if (operation == "group.membership_adjust") {
        if (!callbacks.groupMembership) return false;
        const auto member = args.value("npc", "self") == "self" ? npcId : args.value("npc", npcId);
        return callbacks.groupMembership(Json{{"group", args.value("group", "")}, {"npc", member},
                                               {"role", args.value("role", "member")},
                                               {"active", args.value("active", true)}});
    }
    if (operation == "need.relieve") {
        if (!callbacks.needRelief) return false;
        callbacks.needRelief(args.value("action", actionId));
        return true;
    }
    if (operation == "link.set_state") {
        const auto link = args.value("link", "");
        if (!state.entities.contains(linkEntityId(link))) return false;
        const auto traversable = args.value("traversable", true);
        const auto locked = args.value("locked", false);
        state.links[link] = RuntimeLinkState{traversable, locked};
        emit("action.structure", npcId, link, "A behavior recipe changed a traversal link",
             Json{{"action_id", actionId}, {"link_id", link}, {"traversable", traversable}, {"locked", locked}});
        return true;
    }
    if (operation == "relationship.adjust") {
        const auto target = args.value("target", "player");
        RelationshipAxes delta{args.value("trust", 0.0), args.value("respect", 0.0),
                               args.value("fear", 0.0), args.value("owed", 0.0)};
        if (!SocialSystem::adjustRelationship(state, npcId, target, delta).accepted) return false;
        emit("action.relationship", npcId, target, "A behavior recipe changed a relationship",
             Json{{"action_id", actionId}, {"target", target}, {"trust", delta.trust},
                  {"respect", delta.respect}, {"fear", delta.fear}, {"owed", delta.owed}});
        return true;
    }
    return false;
}

}  // namespace tide_marks
