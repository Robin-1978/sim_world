#include "world/persistence.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tide_marks {

void JsonFilePersistence::replaceFile(const std::string& path, const std::string& bytes) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError) {
            throw std::runtime_error("cannot create persistence directory: " + parent.string() + ": " +
                                     directoryError.message());
        }
    }
    const std::string temporaryPath = path + ".tmp";
    std::ofstream output(temporaryPath, std::ios::trunc | std::ios::binary);
    if (!output) throw std::runtime_error("cannot write persistence file: " + path);
    output << bytes;
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot flush persistence file: " + path);
    }
    output.close();
    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot replace persistence file: " + path + ": " + error.message());
    }
}

void JsonFilePersistence::saveSnapshot(const std::string& path, const Json& snapshot) const {
    replaceFile(path, snapshot.dump(2) + "\n");
}

void JsonFilePersistence::saveReplay(const std::string& path, const std::vector<Json>& lines) const {
    std::string bytes;
    for (const auto& line : lines) bytes += line.dump() + "\n";
    replaceFile(path, bytes);
}

void JsonFilePersistence::saveDurableCommit(const std::string& path, const Json& snapshot,
                                            const std::vector<Json>& replayLines) const {
    Json artifact{{"kind", "durable_commit"}, {"snapshot", snapshot}, {"replay", replayLines}};
    replaceFile(path, artifact.dump(2) + "\n");
}

SqlitePersistence::SqlitePersistence(std::string databasePath) : databasePath_(std::move(databasePath)) {
    if (databasePath_.empty()) throw std::invalid_argument("SQLite database path must not be empty");
    const auto parent = std::filesystem::path(databasePath_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    sqlite3* database = open();
    initialize(database);
    close(database);
}

sqlite3* SqlitePersistence::open() const {
    sqlite3* database = nullptr;
    const int result = sqlite3_open_v2(databasePath_.c_str(), &database,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
        const std::string message = database != nullptr ? sqlite3_errmsg(database) : "unknown SQLite error";
        close(database);
        throw std::runtime_error("cannot open SQLite persistence: " + message);
    }
    sqlite3_busy_timeout(database, 5000);
    return database;
}

void SqlitePersistence::initialize(sqlite3* database) {
    const char* schema =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS snapshots("
        "path TEXT PRIMARY KEY, payload TEXT NOT NULL, updated_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS replay_lines("
        "path TEXT NOT NULL, line_no INTEGER NOT NULL, payload TEXT NOT NULL,"
        "PRIMARY KEY(path, line_no));"
        "CREATE INDEX IF NOT EXISTS replay_lines_path_idx ON replay_lines(path, line_no);"
        "CREATE TABLE IF NOT EXISTS durable_commits("
        "path TEXT PRIMARY KEY, world_id TEXT NOT NULL, instance_id TEXT NOT NULL,"
        "content_version TEXT NOT NULL, world_revision INTEGER NOT NULL, event_sequence INTEGER NOT NULL,"
        "state_payload TEXT NOT NULL, event_log_payload TEXT NOT NULL, replay_payload TEXT NOT NULL,"
        "processed_turns_payload TEXT NOT NULL, overlays_payload TEXT NOT NULL,"
        "updated_at INTEGER NOT NULL DEFAULT 0);";
    check(sqlite3_exec(database, schema, nullptr, nullptr, nullptr), database, "initialize schema");
}

void SqlitePersistence::close(sqlite3* database) noexcept {
    if (database != nullptr) sqlite3_close(database);
}

void SqlitePersistence::check(int result, sqlite3* database, const char* operation) {
    if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW) return;
    throw std::runtime_error(std::string("SQLite ") + operation + " failed: " + sqlite3_errmsg(database));
}

void SqlitePersistence::saveSnapshot(const std::string& path, const Json& snapshot) const {
    sqlite3* database = open();
    try {
        initialize(database);
        check(sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), database, "begin snapshot transaction");
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(database,
                                 "INSERT OR REPLACE INTO snapshots(path,payload,updated_at) "
                                 "VALUES(?,?,CAST(strftime('%s','now') AS INTEGER));",
                                 -1, &statement, nullptr), database, "prepare snapshot");
        check(sqlite3_bind_text(statement, 1, path.c_str(), -1, SQLITE_TRANSIENT), database, "bind snapshot path");
        const auto payload = snapshot.dump();
        check(sqlite3_bind_text(statement, 2, payload.c_str(), -1, SQLITE_TRANSIENT), database, "bind snapshot payload");
        check(sqlite3_step(statement), database, "write snapshot");
        sqlite3_finalize(statement);
        check(sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr), database, "commit snapshot transaction");
    } catch (...) {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        close(database);
        throw;
    }
    close(database);
}

void SqlitePersistence::saveReplay(const std::string& path, const std::vector<Json>& lines) const {
    sqlite3* database = open();
    try {
        initialize(database);
        check(sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), database, "begin replay transaction");
        sqlite3_stmt* deleteStatement = nullptr;
        check(sqlite3_prepare_v2(database, "DELETE FROM replay_lines WHERE path=?;", -1, &deleteStatement, nullptr),
              database, "prepare replay delete");
        check(sqlite3_bind_text(deleteStatement, 1, path.c_str(), -1, SQLITE_TRANSIENT), database, "bind replay path");
        check(sqlite3_step(deleteStatement), database, "delete replay");
        sqlite3_finalize(deleteStatement);
        sqlite3_stmt* insertStatement = nullptr;
        check(sqlite3_prepare_v2(database,
                                 "INSERT INTO replay_lines(path,line_no,payload) VALUES(?,?,?);",
                                 -1, &insertStatement, nullptr), database, "prepare replay insert");
        for (std::size_t index = 0; index < lines.size(); ++index) {
            sqlite3_reset(insertStatement);
            sqlite3_clear_bindings(insertStatement);
            check(sqlite3_bind_text(insertStatement, 1, path.c_str(), -1, SQLITE_TRANSIENT), database, "bind replay path");
            check(sqlite3_bind_int64(insertStatement, 2, static_cast<sqlite3_int64>(index)), database, "bind replay line");
            const auto payload = lines[index].dump();
            check(sqlite3_bind_text(insertStatement, 3, payload.c_str(), -1, SQLITE_TRANSIENT), database, "bind replay payload");
            check(sqlite3_step(insertStatement), database, "write replay line");
        }
        sqlite3_finalize(insertStatement);
        check(sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr), database, "commit replay transaction");
    } catch (...) {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        close(database);
        throw;
    }
    close(database);
}

void SqlitePersistence::saveDurableCommit(const std::string& path, const Json& snapshot,
                                          const std::vector<Json>& replayLines) const {
    if (!snapshot.is_object() || !snapshot.contains("world_revision") ||
        !snapshot.contains("event_sequence") || !snapshot.contains("events")) {
        throw std::invalid_argument("durable commit snapshot is missing revision or event history");
    }
    Json state = snapshot;
    const Json events = state.at("events");
    state.erase("events");
    const auto serializedReplay = Json(replayLines).dump();
    const auto serializedState = state.dump();
    const auto serializedEvents = events.dump();
    const auto processedTurns = snapshot.value("processed_turn_ids", Json::array()).dump();
    const auto overlays = snapshot.value("active_overlays", Json::array()).dump();

    sqlite3* database = open();
    try {
        initialize(database);
        check(sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), database,
              "begin durable commit transaction");
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(
                  database,
                  "INSERT OR REPLACE INTO durable_commits "
                  "(path,world_id,instance_id,content_version,world_revision,event_sequence,state_payload,"
                  "event_log_payload,replay_payload,processed_turns_payload,overlays_payload,updated_at) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?,CAST(strftime('%s','now') AS INTEGER));",
                  -1, &statement, nullptr),
              database, "prepare durable commit");
        const auto bind = [database, statement](int index, const std::string& value) {
            return sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT);
        };
        check(bind(1, path), database, "bind durable commit path");
        check(bind(2, snapshot.value("world_id", "")), database, "bind durable commit world");
        check(bind(3, snapshot.value("instance_id", "")), database, "bind durable commit instance");
        check(bind(4, snapshot.value("content_version", "")), database, "bind durable commit content version");
        check(sqlite3_bind_int64(statement, 5, snapshot.at("world_revision").get<sqlite3_int64>()), database,
              "bind durable commit revision");
        check(sqlite3_bind_int64(statement, 6, snapshot.at("event_sequence").get<sqlite3_int64>()), database,
              "bind durable commit event sequence");
        check(bind(7, serializedState), database, "bind durable commit state");
        check(bind(8, serializedEvents), database, "bind durable commit events");
        check(bind(9, serializedReplay), database, "bind durable commit replay");
        check(bind(10, processedTurns), database, "bind durable commit idempotency");
        check(bind(11, overlays), database, "bind durable commit overlays");
        check(sqlite3_step(statement), database, "write durable commit");
        sqlite3_finalize(statement);
        check(sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr), database,
              "commit durable commit transaction");
    } catch (...) {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        close(database);
        throw;
    }
    close(database);
}

Json SqlitePersistence::loadSnapshot(const std::string& path) const {
    sqlite3* database = open();
    try {
        initialize(database);
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(database, "SELECT payload FROM snapshots WHERE path=?;", -1, &statement, nullptr),
              database, "prepare snapshot read");
        check(sqlite3_bind_text(statement, 1, path.c_str(), -1, SQLITE_TRANSIENT), database, "bind snapshot read path");
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) {
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite snapshot does not exist: " + path);
        }
        check(step, database, "read snapshot");
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        Json result = Json::parse(text == nullptr ? "{}" : text);
        sqlite3_finalize(statement);
        close(database);
        return result;
    } catch (...) {
        close(database);
        throw;
    }
}

std::vector<Json> SqlitePersistence::loadReplay(const std::string& path) const {
    sqlite3* database = open();
    try {
        initialize(database);
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(database,
                                 "SELECT payload FROM replay_lines WHERE path=? ORDER BY line_no;",
                                 -1, &statement, nullptr), database, "prepare replay read");
        check(sqlite3_bind_text(statement, 1, path.c_str(), -1, SQLITE_TRANSIENT), database, "bind replay read path");
        std::vector<Json> result;
        while (true) {
            const int step = sqlite3_step(statement);
            if (step == SQLITE_DONE) break;
            check(step, database, "read replay line");
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
            result.push_back(Json::parse(text == nullptr ? "{}" : text));
        }
        sqlite3_finalize(statement);
        close(database);
        return result;
    } catch (...) {
        close(database);
        throw;
    }
}

Json SqlitePersistence::loadDurableCommit(const std::string& path) const {
    sqlite3* database = open();
    try {
        initialize(database);
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(database,
                                 "SELECT world_id,instance_id,content_version,world_revision,event_sequence,"
                                 "state_payload,event_log_payload,replay_payload,processed_turns_payload,overlays_payload "
                                 "FROM durable_commits WHERE path=?;",
                                 -1, &statement, nullptr),
              database, "prepare durable commit read");
        check(sqlite3_bind_text(statement, 1, path.c_str(), -1, SQLITE_TRANSIENT), database,
              "bind durable commit read path");
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) {
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite durable commit does not exist: " + path);
        }
        check(step, database, "read durable commit");
        const auto text = [statement](int index) {
            const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, index));
            return std::string(value == nullptr ? "" : value);
        };
        Json snapshot = Json::parse(text(5));
        snapshot["world_id"] = text(0);
        snapshot["instance_id"] = text(1);
        snapshot["content_version"] = text(2);
        snapshot["world_revision"] = sqlite3_column_int64(statement, 3);
        snapshot["event_sequence"] = sqlite3_column_int64(statement, 4);
        snapshot["events"] = Json::parse(text(6));
        snapshot["processed_turn_ids"] = Json::parse(text(8));
        snapshot["active_overlays"] = Json::parse(text(9));
        const auto replay = Json::parse(text(7));
        snapshot["durable_replay"] = replay;
        sqlite3_finalize(statement);
        close(database);
        return snapshot;
    } catch (...) {
        close(database);
        throw;
    }
}

}  // namespace tide_marks
