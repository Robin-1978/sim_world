#include "world/world_service.hpp"

#include <exception>

namespace tide_marks {

WorldService::TransactionScope::TransactionScope(WorldService& service)
    : service_(service), uncaughtAtConstruction_(std::uncaught_exceptions()) {
    service_.beginTransaction();
}

WorldService::TransactionScope::~TransactionScope() {
    if (!active_) return;
    if (std::uncaught_exceptions() > uncaughtAtConstruction_) service_.rollbackTransaction();
    else service_.commitTransaction();
}

void WorldService::TransactionScope::rollback() {
    if (!active_) return;
    active_ = false;
    service_.rollbackTransaction();
}

void WorldService::TransactionScope::markChanged() { service_.markTransactionChanged(); }

void WorldService::beginTransaction() {
    transactionManager_.begin(state_);
}

void WorldService::commitTransaction() {
    transactionManager_.commit(state_, eventBus_, [this] { rebuildEntityRegistry(); });
}

void WorldService::markTransactionChanged() {
    transactionManager_.markChanged(state_);
}

void WorldService::rollbackTransaction() {
    transactionManager_.rollback(state_, [this] { rebuildEntityRegistry(); });
}

}  // namespace tide_marks
