#pragma once

#include "core/types.hpp"

#include <sqlite3.h>
#include <string>
#include <vector>

namespace tide_marks {

// Persistence is deliberately narrower than WorldService. It stores already
// committed representations; it never validates or mutates world facts.
class IWorldPersistence {
public:
    virtual ~IWorldPersistence() = default;
    virtual void saveSnapshot(const std::string& path, const Json& snapshot) const = 0;
    virtual void saveReplay(const std::string& path, const std::vector<Json>& lines) const = 0;
    // An explicit durable checkpoint. Implementations must make the supplied
    // committed Snapshot and its Replay artifact visible as one checkpoint.
    virtual void saveDurableCommit(const std::string& path, const Json& snapshot,
                                   const std::vector<Json>& replayLines) const = 0;
};

// Prototype file adapter. SQLite implements the same boundary without
// introducing a second mutable WorldState.
class JsonFilePersistence final : public IWorldPersistence {
public:
    void saveSnapshot(const std::string& path, const Json& snapshot) const override;
    void saveReplay(const std::string& path, const std::vector<Json>& lines) const override;
    void saveDurableCommit(const std::string& path, const Json& snapshot,
                           const std::vector<Json>& replayLines) const override;

private:
    static void replaceFile(const std::string& path, const std::string& bytes);
};

// Single-file adapter. It stores the same serialized artifacts as
// JsonFilePersistence, but each replacement is committed in one SQLite
// transaction and can be queried by run path. It is an adapter, not a second
// mutable WorldState or an alternative rule engine.
class SqlitePersistence final : public IWorldPersistence {
public:
    explicit SqlitePersistence(std::string databasePath);

    void saveSnapshot(const std::string& path, const Json& snapshot) const override;
    void saveReplay(const std::string& path, const std::vector<Json>& lines) const override;
    void saveDurableCommit(const std::string& path, const Json& snapshot,
                           const std::vector<Json>& replayLines) const override;

    [[nodiscard]] Json loadSnapshot(const std::string& path) const;
    [[nodiscard]] std::vector<Json> loadReplay(const std::string& path) const;
    [[nodiscard]] Json loadDurableCommit(const std::string& path) const;

private:
    std::string databasePath_;

    [[nodiscard]] sqlite3* open() const;
    static void initialize(sqlite3* database);
    static void close(sqlite3* database) noexcept;
    static void check(int result, sqlite3* database, const char* operation);
};
}  // namespace tide_marks
