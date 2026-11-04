#pragma once

#include "provider/provider.hpp"

#include <memory>
#include <string>

namespace tide_marks {

// Intelligence Profile is the versioned configuration boundary for how a
// world uses a provider.  It selects the provider and the decision budgets;
// it never grants write access to WorldState.
struct IntelligenceProfile {
    std::string id;
    std::string providerType{"mock"};
    LlamaCppConfig llama;
};

[[nodiscard]] std::string defaultIntelligenceProfilePath();
[[nodiscard]] IntelligenceProfile loadIntelligenceProfile(const std::string& path);
[[nodiscard]] std::unique_ptr<IProvider> makeProviderFromProfile(
    const IntelligenceProfile& profile, const std::string& providerOverride = {});
// Product entry points use the versioned intelligence profile by default.
// Tests and offline tools must inject MockProvider explicitly.
[[nodiscard]] std::unique_ptr<IProvider> makeDefaultRuntimeProvider();

}  // namespace tide_marks
