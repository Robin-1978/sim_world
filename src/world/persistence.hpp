#pragma once

#include "core/types.hpp"

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
};

// Prototype file adapter. SQLite can implement the same boundary later
// without introducing a second mutable WorldState.
class JsonFilePersistence final : public IWorldPersistence {
public:
    void saveSnapshot(const std::string& path, const Json& snapshot) const override;
    void saveReplay(const std::string& path, const std::vector<Json>& lines) const override;

private:
    static void replaceFile(const std::string& path, const std::string& bytes);
};

}  // namespace tide_marks
