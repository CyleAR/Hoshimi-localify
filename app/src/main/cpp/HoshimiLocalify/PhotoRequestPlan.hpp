#pragma once
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <functional>
#include <numeric>

namespace HoshimiLocal::PhotoRequests {
    struct OwnedActor {
        std::string liveCostume, liveHair, favoriteCostume, favoriteHair;
        bool operator==(const OwnedActor&) const = default;
    };
    struct Inventory {
        bool ready{};
        std::map<std::string, OwnedActor> actors;
        std::set<std::string> costumes, hairs, poses;
        std::map<std::string, std::map<std::string, bool>> unlocked;
    };
    struct ActorSelection {
        std::string character, costume, hair;
    };

    struct SceneOutfit {
        ActorSelection actor;
        std::map<int, std::vector<std::string>> poses;
    };
    struct ScenePosition {
        int number{}, poseType{}, capacity{};
    };
    struct SceneRules {
        std::string kind, id, stage, forcedCostumeType;
        std::vector<std::string> costumeTypes, forbiddenCharacters, allowedCharacters, forbiddenPoses;
        std::vector<int> characterAmounts;
        std::map<int, ScenePosition> positions;
        std::map<std::string, std::vector<SceneOutfit>> outfits;
        std::map<std::string, SceneOutfit> systemOutfits;
        int action{}, maxActors{};
        bool expression{}, hasCostumeRules{};
    };
    using SceneCatalog = std::map<std::string, SceneRules>;

    inline std::string SceneKey(const std::string& kind, const std::string& id) { return kind + "/" + id; }

    // Solve the complete roster rather than committing a greedy fallback for each actor.
    // A scarce candidate for a later slot can displace an earlier provisional assignment.
    inline std::vector<std::string> MatchRoster(const std::vector<std::string>& requested,
                                              const std::vector<std::vector<std::string>>& domains,
                                              bool preserveCharacters = false) {
        if (requested.empty() || requested.size() != domains.size()) return {};
        if (preserveCharacters) {
            if (std::set<std::string>(requested.begin(), requested.end()).size() != requested.size()) return {};
            for (size_t i = 0; i < requested.size(); ++i)
                if (requested[i].empty() || std::find(domains[i].begin(), domains[i].end(), requested[i]) == domains[i].end()) return {};
            return requested;
        }
        std::map<std::string, size_t> owner;
        std::vector<std::string> result(requested.size());
        std::vector<bool> fixed(requested.size());
        for (size_t i = 0; i < requested.size(); ++i) {
            if (!owner.contains(requested[i]) &&
                std::find(domains[i].begin(), domains[i].end(), requested[i]) != domains[i].end()) {
                owner[requested[i]] = i; result[i] = requested[i]; fixed[i] = true;
            }
        }
        std::function<bool(size_t, std::set<std::string>&)> assign = [&](size_t slot, auto& visited) {
            for (const auto& candidate : domains[slot]) {
                if (candidate.empty() || !visited.insert(candidate).second) continue;
                const auto found = owner.find(candidate);
                if (found != owner.end() && !assign(found->second, visited)) continue;
                owner[candidate] = slot; result[slot] = candidate;
                return true;
            }
            return false;
        };
        for (size_t i = 0; i < requested.size(); ++i) {
            if (fixed[i]) continue;
            std::set<std::string> visited;
            if (!assign(i, visited)) return {};
        }
        return result;
    }

    inline bool HasPoseType(const SceneOutfit& outfit, int type) {
        if (type) {
            const auto found = outfit.poses.find(type);
            return found != outfit.poses.end() && !found->second.empty();
        }
        return std::any_of(outfit.poses.begin(), outfit.poses.end(), [](const auto& entry) { return !entry.second.empty(); });
    }

    inline std::vector<size_t> ProjectRoster(size_t count, int capacity, const std::vector<int>& preferences) {
        std::vector<size_t> indexes(count);
        std::iota(indexes.begin(), indexes.end(), size_t{});
        if (capacity <= 0 || count <= static_cast<size_t>(capacity)) return indexes;
        std::stable_sort(indexes.begin(), indexes.end(), [&](size_t a, size_t b) {
            return (a < preferences.size() ? preferences[a] : 0) > (b < preferences.size() ? preferences[b] : 0);
        });
        indexes.resize(capacity);
        std::sort(indexes.begin(), indexes.end());
        return indexes;
    }

    inline bool IsKnownLocked(const Inventory& inventory, const std::string& kind, const std::string& id) {
        const auto group = inventory.unlocked.find(kind);
        if (group == inventory.unlocked.end()) return false;
        const auto item = group->second.find(id);
        return item != group->second.end() && !item->second;
    }

    // The original ID is preferred; fallback order stays stable across check/create.
    template<class Accept>
    std::string SelectOwnedId(const std::set<std::string>& owned, const std::string& requested,
                              Accept accept) {
        if (owned.contains(requested) && accept(requested)) return requested;
        for (const auto& id : owned) if (accept(id)) return id;
        return {};
    }

    inline bool Aligned(const std::vector<std::string>& characters,
                        const std::vector<std::string>& costumes,
                        const std::vector<std::string>& hairs) {
        return !characters.empty() && characters.size() == costumes.size() &&
               characters.size() == hairs.size() &&
               std::none_of(characters.begin(), characters.end(), [](const auto& id) { return id.empty(); });
    }

    template<class Accept>
    std::string SelectCharacter(const Inventory& inventory, const std::string& requested, Accept accept) {
        if (inventory.actors.contains(requested) && accept(requested)) return requested;
        for (const auto& [id, actor] : inventory.actors) if (accept(id)) return id;
        return {};
    }
}
