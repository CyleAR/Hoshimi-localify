#include "PhotoRequestPlan.hpp"
#include <cassert>
#include <iostream>
#include <array>
using namespace HoshimiLocal::PhotoRequests;

int main() {
    Inventory owned;
    owned.ready = true;
    owned.actors["idol-a"] = {};
    owned.actors["idol-b"] = {};
    owned.costumes = {"costume-a", "costume-b"};
    owned.poses = {"pose-sitting", "pose-standing"};
    // An inlined/unobserved getter must not reject a normally selected unlocked scene.
    assert(!IsKnownLocked(owned, "PhotoExpression", "normal-scene"));
    owned.unlocked["PhotoExpression"]["normal-scene"] = true;
    owned.unlocked["PhotoExpression"]["locked-scene"] = false;
    assert(!IsKnownLocked(owned, "PhotoExpression", "normal-scene"));
    assert(IsKnownLocked(owned, "PhotoExpression", "locked-scene"));
    assert(!IsKnownLocked(owned, "PhotoExpression", "unobserved-scene"));

    // A displayed virtual item must never pass merely because the master accepts it.
    assert(SelectOwnedId(owned.costumes, "virtual-costume", [](auto&) { return true; }) == "costume-a");
    assert(SelectOwnedId(owned.costumes, "costume-b", [](auto&) { return true; }) == "costume-b");
    // Ownership alone is insufficient when a pose has no motion for the selected idol.
    assert(SelectOwnedId(owned.poses, "pose-sitting", [](const auto& id) { return id == "pose-standing"; }) == "pose-standing");
    assert(SelectOwnedId(owned.poses, "pose-standing", [](auto&) { return false; }).empty());
    // Replacing an unavailable idol must choose an owned, compatible actor.
    auto allowed = [](const auto& id) { return id == "idol-b"; };
    assert(SelectCharacter(owned, "unavailable-idol", allowed) == "idol-b");
    assert(SelectCharacter(owned, "idol-a", allowed) == "idol-b");
    assert(SelectCharacter(owned, "idol-b", allowed) == "idol-b");
    assert(SelectCharacter(Inventory{}, "idol-b", allowed).empty());
    // Malformed rosters cannot be silently truncated; a complete multi-idol roster is retained.
    assert(!Aligned({"idol-a", "idol-b"}, {"costume-a"}, {"hair-a", "hair-b"}));
    assert(!Aligned({"idol-a", ""}, {"costume-a", "costume-b"}, {"hair-a", "hair-b"}));
    assert(!Aligned({}, {}, {}));
    assert(Aligned({"idol-a", "idol-b"}, {"costume-a", "costume-b"}, {"hair-a", "hair-b"}));
    // Repeated check/create selection does not depend on insertion order.
    auto other = owned;
    other.actors.clear(); other.actors["idol-b"] = {}; other.actors["idol-a"] = {};
    assert(SelectCharacter(owned, "virtual", [](auto&) { return true; }) ==
           SelectCharacter(other, "virtual", [](auto&) { return true; }));
    // A later actor may need the only candidate provisionally assigned to an earlier actor.
    auto scarce = MatchRoster({"virtual-a", "virtual-b"}, {{"idol-a", "idol-b"}, {"idol-a"}});
    assert((scarce == std::vector<std::string>{"idol-b", "idol-a"}));
    assert(MatchRoster({"virtual-a", "virtual-b"}, {{"idol-a"}, {"idol-a"}}).empty());
    auto displaced = MatchRoster({"idol-a", "virtual-b"}, {{"idol-a", "idol-b"}, {"idol-a"}});
    assert((displaced == std::vector<std::string>{"idol-b", "idol-a"}));
    // Pool master data permits one actor even when the local UI contains three.
    assert((ProjectRoster(3, 1, {0, 0, 1}) == std::vector<size_t>{2}));
    assert((ProjectRoster(3, 1, {2, 0, 1}) == std::vector<size_t>{0}));
    assert((ProjectRoster(3, 2, {0, 1, 1}) == std::vector<size_t>{1, 2}));
    assert((ProjectRoster(3, 3, {0, 0, 0}) == std::vector<size_t>{0, 1, 2}));
    assert((ProjectRoster(3, 0, {}) == std::vector<size_t>{0, 1, 2}));
    // Starting a job must not silently substitute another idol and consume their stamina.
    assert((MatchRoster({"a", "b"}, {{"a", "b"}, {"a", "b"}}, true) == std::vector<std::string>{"a", "b"}));
    assert(MatchRoster({"a", "b"}, {{"b", "c"}, {"b"}}, true).empty());
    assert(MatchRoster({"a", "a"}, {{"a"}, {"a"}}, true).empty());
    assert(MatchRoster({"a"}, {{"b"}}, true).empty());
    // Exhaustively compare all nonempty three-actor domains to an independent permutation oracle.
    const std::array<std::string, 3> actors{"a", "b", "c"};
    int combinations = 0;
    for (int first = 1; first < 8; ++first) for (int second = 1; second < 8; ++second) for (int third = 1; third < 8; ++third) {
        const int masks[]{first, second, third};
        std::vector<std::vector<std::string>> domains(3);
        for (int slot = 0; slot < 3; ++slot) for (int actor = 0; actor < 3; ++actor)
            if (masks[slot] & (1 << actor)) domains[slot].push_back(actors[actor]);
        std::array<int, 3> permutation{0, 1, 2};
        bool possible = false;
        do {
            possible |= (masks[0] & (1 << permutation[0])) && (masks[1] & (1 << permutation[1])) &&
                        (masks[2] & (1 << permutation[2]));
        } while (std::next_permutation(permutation.begin(), permutation.end()));
        auto matched = MatchRoster({"a", "virtual", "c"}, domains);
        assert(!matched.empty() == possible);
        if (possible) {
            assert(std::set<std::string>(matched.begin(), matched.end()).size() == 3);
            for (int slot = 0; slot < 3; ++slot)
                assert(std::find(domains[slot].begin(), domains[slot].end(), matched[slot]) != domains[slot].end());
        }
        ++combinations;
    }
    std::cout << "photo request planning: 29 regressions and " << combinations << " roster domains passed\n";
}
