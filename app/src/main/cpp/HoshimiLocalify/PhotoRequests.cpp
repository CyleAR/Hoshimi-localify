#include "PhotoRequests.hpp"
#include "PhotoRequestPlan.hpp"
#include "Il2cppUtils.hpp"
#include "config/Config.hpp"
#include <mutex>
#include <utility>
#include <memory>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "Local.h"
#include <tuple>
#include <chrono>

namespace HoshimiLocal::PhotoRequests {
namespace {
    using String = UnityResolve::UnityType::String;
    using Method = Il2cppUtils::MethodInfo;
    std::mutex inventoryMutex;
    Inventory inventory;
    CostumeValidator validateCostume{};
    HairValidator validateHair{};
    uint64_t inventoryRevision{};
    std::mutex catalogMutex;
    std::shared_ptr<const SceneCatalog> catalog;
    uint64_t catalogRevision = ~uint64_t{};
    SceneCatalog dynamicScenes;
    struct StoredPlan {
        uint64_t revision;
        std::map<std::string, ActorSelection> selections;
    };
    std::map<std::string, StoredPlan> storedPlans;

    Method* MethodOf(void* object, const char* name, int arguments = 0) {
        if (!object) return nullptr;
        auto klass = Il2cppUtils::get_class_from_instance(object);
        return klass ? Il2cppUtils::il2cpp_class_get_method_from_name(klass, name, arguments) : nullptr;
    }

    template<class T = void*>
    T Get(void* object, const char* name) {
        auto method = MethodOf(object, name);
        return method ? reinterpret_cast<T (*)(void*, const Method*)>(method->methodPointer)(object, method) : T{};
    }

    template<class T>
    bool Set(void* object, const char* name, T value) {
        auto method = MethodOf(object, name, 1);
        if (!method) return false;
        reinterpret_cast<void (*)(void*, T, const Method*)>(method->methodPointer)(object, value, method);
        return true;
    }

    std::string Text(void* object, const char* getter) {
        auto value = Get<String*>(object, getter);
        return value ? value->ToString() : "";
    }
    bool SetText(void* object, const char* setter, const std::string& value) {
        return Set(object, setter, String::New(value));
    }

    // Read protobuf backing fields so UI hooks cannot contaminate ownership or restrictions.
    template<class T = void*>
    T Field(void* object, const char* name) {
        T value{};
        if (!object) return value;
        auto klass = Il2cppUtils::get_class_from_instance(object);
        auto field = UnityResolve::Invoke<void*>("il2cpp_class_get_field_from_name", klass, name);
        if (field) UnityResolve::Invoke<void>("il2cpp_field_get_value", object, field, &value);
        return value;
    }

    template<class T = void*>
    std::vector<T> Items(void* list) {
        std::vector<T> result;
        if (!list) return result;
        Il2cppUtils::Tools::CSListEditor<T> editor(list);
        const auto count = editor.get_Count();
        if (count < 0 || count > 100000) return result;
        for (int i = 0; i < count; ++i) result.push_back(editor.get_Item(i));
        return result;
    }

    std::vector<std::string> Strings(void* list) {
        std::vector<std::string> result;
        for (auto value : Items<String*>(list)) result.push_back(value ? value->ToString() : "");
        return result;
    }

    bool Replace(void* list, const std::vector<std::string>& values) {
        auto clear = MethodOf(list, "Clear");
        if (!clear || !MethodOf(list, "Add", 1)) return false;
        reinterpret_cast<void (*)(void*, const Method*)>(clear->methodPointer)(list, clear);
        Il2cppUtils::Tools::CSListEditor<String*> editor(list);
        for (const auto& value : values) editor.Add(String::New(value));
        return Strings(list) == values;
    }

    bool ReplaceNumbers(void* list, const std::vector<int>& values) {
        auto clear = MethodOf(list, "Clear");
        if (!clear || !MethodOf(list, "Add", 1)) return false;
        reinterpret_cast<void (*)(void*, const Method*)>(clear->methodPointer)(list, clear);
        Il2cppUtils::Tools::CSListEditor<int> editor(list);
        for (const int value : values) editor.Add(value);
        return Items<int>(list) == values;
    }

    bool Contains(void* list, const std::string& value) {
        auto strings = Strings(list);
        return std::find(strings.begin(), strings.end(), value) != strings.end();
    }

    void* Master(const char* getter) {
        auto method = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master", "MasterManager", getter);
        if (!method) return nullptr;
        return reinterpret_cast<void* (*)(void*)>(method->function)(method->address);
    }

    void* Find(const char* getter, const std::string& id) {
        if (id.empty()) return nullptr;
        auto master = Master(getter);
        auto method = MethodOf(master, "FindByKey", 1);
        if (!method) return nullptr;
        return reinterpret_cast<void* (*)(void*, String*, const Method*)>(method->methodPointer)(
            master, String::New(id), method);
    }

    bool MotionExists(const std::string& pose, const std::string& character) {
        auto master = Master("get_PhotoPoseMotionMaster");
        static auto resolved = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
            "PhotoPoseMotionMaster", "FindByKey", {"System.String", "System.String"});
        auto method = resolved ? static_cast<Method*>(resolved->address) : nullptr;
        if (!master || !method) return false;
        return reinterpret_cast<void* (*)(void*, String*, String*, const Method*)>(method->methodPointer)(
            master, String::New(pose), String::New(character), method) != nullptr;
    }

    bool ReadInventory(void* source, Inventory& target, bool initial) {
        auto characters = Field(source, "characters_");
        auto costumes = Field(source, "costumes_");
        auto hairs = Field(source, "hairs_");
        auto poses = Field(source, "photoPoses_");
        if (!characters || !costumes || !hairs || !poses) return false;
        if (initial) target = {};
        for (auto actor : Items(characters)) {
            auto id = Text(actor, "get_CharacterId");
            if (!id.empty()) target.actors[id] = {Text(actor, "get_LiveCostumeId"), Text(actor, "get_LiveHairId"),
                                                Text(actor, "get_FavoriteCostumeId"), Text(actor, "get_FavoriteHairId")};
        }
        for (auto item : Items(costumes)) target.costumes.insert(Text(item, "get_CostumeId"));
        for (auto item : Items(hairs)) target.hairs.insert(Text(item, "get_HairId"));
        for (auto item : Items(poses)) target.poses.insert(Text(item, "get_PhotoPoseId"));
        target.costumes.erase(""); target.hairs.erase(""); target.poses.erase("");
        target.ready = !target.actors.empty() && !target.costumes.empty() && !target.hairs.empty();
        return true;
    }

    bool CostumeAllowed(void* costume, void* expression, const std::string& character) {
        if (!costume || Text(costume, "get_CharacterId") != character ||
            Field<bool>(costume, "isDisableAdmin_") || Field<bool>(costume, "isUserUnavailable_")) return false;
        if (!expression) return true;
        // Use the unhooked game predicate. ForceCostumeTypeId is not an additional AND
        // constraint in IsAppropriateCostume (verified against the 6.1.1 ARM64 code).
        return validateCostume && validateCostume(expression, costume, nullptr);
    }

    bool HairAllowed(void* hair, const std::string& character, const std::string& costume) {
        if (!hair || Text(hair, "get_CharacterId") != character || Field<bool>(hair, "isDisableAdmin_")) return false;
        auto costumeMaster = Find("get_CostumeMaster", costume);
        return costumeMaster && validateHair && validateHair(hair, costumeMaster, nullptr);
    }

    std::string AvailabilityId(void* item) {
        for (const auto getter : {"get_Id", "get_PhotoExpressionId", "get_MusicId", "get_StageId"}) {
            auto id = Text(item, getter);
            if (!id.empty()) return id;
        }
        return {};
    }

    std::vector<void*> AllMasterRows(const char* getter) {
        auto master = Master(getter);
        auto method = MethodOf(master, "GetAllWithSortByKey", 1);
        if (!method) return {};
        auto rows = reinterpret_cast<void* (*)(void*, int, const Method*)>(method->methodPointer)(master, 0, method);
        return Items(rows);
    }

    SceneRules ReadScene(void* specifier, const std::string& kind, const std::string& id) {
        SceneRules scene;
        scene.kind = kind; scene.id = id;
        scene.expression = kind == "PhotoExpression";
        scene.hasCostumeRules = MethodOf(specifier, "get_CostumeTypes") != nullptr;
        scene.stage = Text(specifier, "get_StageId");
        if (scene.stage.empty()) scene.stage = Text(specifier, "get_PhotoStageId");
        scene.forcedCostumeType = Text(specifier, "get_ForceCostumeTypeId");
        if (scene.forcedCostumeType.empty()) scene.forcedCostumeType = Text(Get(specifier, "get_ForceCostumeType"), "get_Id");
        // This is the expanded list consumed by the original IsAppropriateCostume predicate.
        for (auto type : Items(Get(specifier, "get_CostumeTypes"))) scene.costumeTypes.push_back(Text(type, "get_Id"));
        scene.forbiddenCharacters = Strings(Get(specifier, "get_ImpossibleShootingCharacterIds"));
        scene.allowedCharacters = Strings(Get(specifier, "get_CharacterIds"));
        scene.forbiddenPoses = Strings(Get(specifier, "get_ImpossiblePhotoPoseIds"));
        scene.action = MethodOf(specifier, "get_ActionType") ? Get<int>(specifier, "get_ActionType") : Get<int>(specifier, "get_Type");
        scene.maxActors = Get<int>(specifier, "get_MaxCapacity");
        scene.characterAmounts = Items<int>(Get(specifier, "get_CharacterAmountArray"));
        if (scene.expression) {
            scene.maxActors = Get<int>(specifier, "GetMaxCharacterAmount");
            for (auto position : Items(Get(specifier, "GetStagePositionList"))) {
                ScenePosition slot{Get<int>(position, "get_Number"), Get<int>(position, "get_PossiblePhotoPoseType"),
                                   static_cast<int>(Items<float>(Get(position, "get_CharacterPositions")).size() / 3)};
                scene.positions[slot.number] = slot;
            }
        }
        // ActivityCostumeChanger.GetForceActorCostume first uses GetForceCostume, even for
        // owned idols, then constructs ActorCostume(costume, null). These job uniforms are
        // supplied by the activity and do not appear in the user's purchased costume list.
        if ((kind == "Promotion" || kind == "FanEvent" || kind == "Refresh") && !scene.forcedCostumeType.empty()) {
            static auto force = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Data",
                "CostumeSpecifierExtensions", "GetForceCostume");
            static auto actorClass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Data", "ActorCostume");
            std::vector<std::string> characters;
            { std::lock_guard lock(inventoryMutex); for (const auto& [id, actor] : inventory.actors) characters.push_back(id); }
            for (const auto& id : characters) {
                auto character = Find("get_CharacterMaster", id);
                if (!force || !actorClass || !character) continue;
                auto costume = reinterpret_cast<void* (*)(void*, void*, void*)>(force->function)(specifier, character, force->address);
                if (!costume) continue;
                auto actor = actorClass->New<void*>();
                auto ctor = MethodOf(actor, ".ctor", 2);
                if (!ctor) continue;
                reinterpret_cast<void (*)(void*, void*, void*, const Method*)>(ctor->methodPointer)(actor, costume, nullptr, ctor);
                const auto costumeId = Text(actor, "get_CostumeId");
                if (!costumeId.empty()) scene.systemOutfits[id] = {{id, costumeId, Text(actor, "get_HairId")}, {}};
            }
            Log::InfoFmt("PhotoRequests: activity uniform %s/%s type=%s provided=%zu", kind.c_str(), id.c_str(),
                         scene.forcedCostumeType.c_str(), scene.systemOutfits.size());
        }
        return scene;
    }

    void RefreshActivityRules() {
        const auto info = [](const char* getter) -> void* {
            auto method = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.User", "UserDataManager", getter);
            return method ? reinterpret_cast<void* (*)(void*)>(method->function)(method->address) : nullptr;
        };
        SceneCatalog fresh;
        for (const auto& [getter, rowsGetter, kind] : {
                std::tuple{"get_PromotionInfo", "get_Promotions", "Promotion"},
                std::tuple{"get_FanEventInfo", "get_FanEvents", "FanEvent"}}) {
            for (auto item : Items(Get(info(getter), rowsGetter))) {
                const auto id = Text(item, "get_Id");
                if (!id.empty()) fresh[SceneKey(kind, id)] = ReadScene(item, kind, id);
            }
        }
        for (auto refresh : Items(Get(info("get_RefreshInfo"), "get_Refreshes"))) {
            const auto id = Text(refresh, "get_Id");
            for (auto level : Items(Get(refresh, "get_Levels"))) {
                const auto key = id + "/" + std::to_string(Get<int>(level, "get_Level"));
                fresh[SceneKey("Refresh", key)] = ReadScene(level, "Refresh", key);
            }
        }
        std::lock_guard lock(catalogMutex);
        bool changed = false;
        for (auto& [key, scene] : fresh) {
            const auto existing = dynamicScenes.find(key);
            if (existing != dynamicScenes.end() && existing->second.costumeTypes == scene.costumeTypes &&
                existing->second.forcedCostumeType == scene.forcedCostumeType &&
                existing->second.allowedCharacters == scene.allowedCharacters &&
                existing->second.forbiddenCharacters == scene.forbiddenCharacters) continue;
            dynamicScenes[key] = std::move(scene);
            changed = true;
        }
        if (changed) {
            catalogRevision = ~uint64_t{};
            Log::InfoFmt("PhotoRequests: live activity rules captured=%zu", fresh.size());
        }
    }

    struct BaseOutfit {
        SceneOutfit option;
        std::string costumeType;
    };

    std::vector<BaseOutfit> BuildOwnedOutfits(const Inventory& owned) {
        std::vector<BaseOutfit> result;
        std::map<std::string, void*> costumes, hairs, poses;
        for (const auto& id : owned.costumes) costumes[id] = Find("get_CostumeMaster", id);
        for (const auto& id : owned.hairs) hairs[id] = Find("get_HairMaster", id);
        for (const auto& id : owned.poses) poses[id] = Find("get_PhotoPoseMaster", id);
        std::map<std::string, std::map<int, std::vector<std::string>>> actorPoses;
        for (const auto& [character, data] : owned.actors) {
            for (const auto& [id, pose] : poses) {
                if (pose && MotionExists(id, character)) actorPoses[character][Get<int>(pose, "get_PoseType")].push_back(id);
            }
        }
        for (const auto& [costumeId, costume] : costumes) {
            const auto character = Text(costume, "get_CharacterId");
            if (!owned.actors.contains(character) || !CostumeAllowed(costume, nullptr, character)) continue;
            for (const auto& [hairId, hair] : hairs) {
                if (Text(hair, "get_CharacterId") != character || !HairAllowed(hair, character, costumeId)) continue;
                BaseOutfit outfit{{{character, costumeId, hairId}, actorPoses[character]}, Text(costume, "get_CostumeTypeId")};
                for (auto master : {costume, hair}) {
                    for (const int type : Items<int>(Field(master, "impossiblePhotoPoseTypes_"))) outfit.option.poses.erase(type);
                }
                result.push_back(std::move(outfit));
            }
        }
        return result;
    }

    void PopulateScene(SceneRules& scene, const std::vector<BaseOutfit>& outfits) {
        scene.outfits.clear();
        for (const auto& base : outfits) {
            const auto& actor = base.option.actor;
            if (actor.character == "char-mna" || actor.character == "char-mng") continue;
            const auto has = [](const auto& list, const auto& id) { return std::find(list.begin(), list.end(), id) != list.end(); };
            if (has(scene.forbiddenCharacters, actor.character) ||
                (!scene.allowedCharacters.empty() && !has(scene.allowedCharacters, actor.character))) continue;
            // Every costume-specifier scene supplies an expanded allowed-type list.
            // Quest/music stages have no costume specifier and intentionally leave this list empty.
            if (scene.hasCostumeRules && !has(scene.costumeTypes, base.costumeType)) continue;
            auto option = base.option;
            for (auto& [type, poses] : option.poses)
                std::erase_if(poses, [&](const auto& id) { return has(scene.forbiddenPoses, id); });
            scene.outfits[actor.character].push_back(std::move(option));
        }
        for (const auto& [character, uniform] : scene.systemOutfits) scene.outfits[character].push_back(uniform);
    }

    void ExportCatalog(const SceneCatalog& scenes, uint64_t revision) {
        using Json = nlohmann::json;
        Json report{{"revision", revision}, {"scenes", Json::array()}};
        for (const auto& [key, scene] : scenes) {
            Json row{{"key", key}, {"stage", scene.stage}, {"action", scene.action}, {"maxActors", scene.maxActors},
                     {"costumeTypes", scene.costumeTypes}, {"forcedCostumeType", scene.forcedCostumeType},
                     {"forbiddenCharacters", scene.forbiddenCharacters}, {"allowedCharacters", scene.allowedCharacters},
                     {"forbiddenPoses", scene.forbiddenPoses}, {"characterAmounts", scene.characterAmounts},
                     {"positions", Json::array()}, {"outfits", Json::array()}};
            for (const auto& [number, position] : scene.positions)
                row["positions"].push_back({{"number", number}, {"poseType", position.poseType}, {"capacity", position.capacity}});
            for (const auto& [character, choices] : scene.outfits) {
                for (const auto& choice : choices) {
                    Json poses = Json::object();
                    for (const auto& [type, ids] : choice.poses) poses[std::to_string(type)] = ids;
                    row["outfits"].push_back({{"character", character}, {"costume", choice.actor.costume},
                                              {"hair", choice.actor.hair}, {"poses", std::move(poses)}});
                }
            }
            report["scenes"].push_back(std::move(row));
        }
        std::ofstream output(Local::GetBasePath() / "photo-scene-catalog.json", std::ios::binary | std::ios::trunc);
        if (output) output << report.dump(2);
    }

    std::shared_ptr<const SceneCatalog> EnsureCatalog() {
        std::lock_guard buildLock(catalogMutex);
        Inventory owned;
        uint64_t revision;
        { std::lock_guard lock(inventoryMutex); owned = inventory; revision = inventoryRevision; }
        if (catalog && catalogRevision == revision) return catalog;
        if (!owned.ready || !validateCostume || !validateHair) return {};
        auto scenes = std::make_shared<SceneCatalog>();
        const auto started = std::chrono::steady_clock::now();
        const auto outfits = BuildOwnedOutfits(owned);
        SceneRules unrestricted;
        unrestricted.kind = "Owned"; unrestricted.id = "default";
        PopulateScene(unrestricted, outfits);
        (*scenes)["Owned/default"] = std::move(unrestricted);
        for (const auto kind : {"PhotoExpression", "PhotoActivity", "PhotoContestActivity", "PhotoQuestStage", "PhotoContestQuestStage"}) {
            const auto getter = std::string("get_") + kind + "Master";
            for (auto row : AllMasterRows(getter.c_str())) {
                auto scene = ReadScene(row, kind, Text(row, "get_Id"));
                if (scene.id.empty()) continue;
                PopulateScene(scene, outfits);
                (*scenes)[SceneKey(scene.kind, scene.id)] = std::move(scene);
            }
        }
        for (const auto& [key, rules] : dynamicScenes) {
            auto scene = rules;
            PopulateScene(scene, outfits);
            (*scenes)[key] = std::move(scene);
        }
        ExportCatalog(*scenes, revision);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        Log::InfoFmt("PhotoRequests: catalog precomputed scenes=%zu owned-outfits=%zu revision=%llu elapsed-ms=%lld",
                     scenes->size(), outfits.size(), static_cast<unsigned long long>(revision), static_cast<long long>(elapsed));
        if (const auto pool = scenes->find("PhotoExpression/photo_expression-pool"); pool != scenes->end()) {
            size_t choices = 0;
            for (const auto& [character, options] : pool->second.outfits) choices += options.size();
            Log::InfoFmt("PhotoRequests: pool catalog eligible-actors=%zu outfits=%zu positions=%zu max-actors=%d",
                         pool->second.outfits.size(), choices, pool->second.positions.size(), pool->second.maxActors);
        }
        catalog = scenes; catalogRevision = revision;
        return catalog;
    }

    struct Context {
        Inventory owned;
        std::shared_ptr<const SceneCatalog> scenes;
        const SceneRules* rules{};
        std::set<int> requiredPoseTypes;
        void* expression{};
        std::string expressionId;
        const char* step = "request shape";
        std::map<std::string, ActorSelection> selections;
        std::set<std::string> droppedSources;
        ActorSelection primary;
        int capacityLimit{};
        bool preserveCharacters{};

        int MaxActors() const {
            const int sceneLimit = rules ? rules->maxActors : 0;
            return capacityLimit > 0 && (sceneLimit <= 0 || capacityLimit < sceneLimit) ? capacityLimit : sceneLimit;
        }

        bool Pair(const std::string& character, const std::string& requestedCostume,
                  const std::string& requestedHair, ActorSelection& result) {
            if (!owned.actors.contains(character)) return false;
            if (rules) {
                const auto found = rules->outfits.find(character);
                if (found == rules->outfits.end()) return false;
                const SceneOutfit* best = nullptr;
                int bestScore = -1;
                for (const auto& option : found->second) {
                    if (rules->expression && !HasPoseType(option, 0)) continue;
                    if (std::any_of(requiredPoseTypes.begin(), requiredPoseTypes.end(),
                                    [&](int type) { return !HasPoseType(option, type); })) continue;
                    const auto& equipped = owned.actors.at(character);
                    const int score = (option.actor.costume == requestedCostume ? 8 : 0) +
                                      (option.actor.hair == requestedHair ? 4 : 0) +
                                      (option.actor.costume == equipped.liveCostume ? 2 : 0) +
                                      (option.actor.hair == equipped.liveHair ? 1 : 0);
                    if (score > bestScore) { best = &option; bestScore = score; }
                }
                if (!best) return false;
                result = best->actor;
                return true;
            }
            auto pickHair = [&](const std::string& costume) {
                return SelectOwnedId(owned.hairs, requestedHair, [&](const auto& id) {
                    return HairAllowed(Find("get_HairMaster", id), character, costume);
                });
            };
            std::string hair;
            auto costume = SelectOwnedId(owned.costumes, requestedCostume, [&](const auto& id) {
                if (!CostumeAllowed(Find("get_CostumeMaster", id), expression, character)) return false;
                hair = pickHair(id);
                return !hair.empty();
            });
            if (costume.empty()) return false;
            result = {character, costume, hair};
            return true;
        }

        bool Actor(const std::string& original, const std::string& costume, const std::string& hair,
                   ActorSelection& result) {
            if (auto it = selections.find(original); it != selections.end()) { result = it->second; return true; }
            if (droppedSources.contains(original) && !primary.character.empty()) { result = primary; return true; }
            ActorSelection candidate;
            auto chosen = SelectCharacter(owned, original, [&](const auto& id) {
                if (Config::unlockAllPhotoPose && (id == "char-mna" || id == "char-mng")) return false;
                if (expression && Contains(Field(expression, "impossibleShootingCharacterIds_"), id)) return false;
                for (const auto& [source, selected] : selections)
                    if (source != original && selected.character == id) return false;
                return Pair(id, costume, hair, candidate);
            });
            if (chosen.empty()) {
                step = "owned costume/hair candidates";
                Log::ErrorFmt("PhotoRequests: no actor candidate for %s expression=%s forced=%s",
                             original.c_str(), expressionId.c_str(), Text(expression, "get_ForceCostumeTypeId").c_str());
                return false;
            }
            result = candidate;
            selections[original] = result;
            return true;
        }

        bool Actors(void* object, const char* charactersGetter, const char* costumesGetter,
                    const char* hairsGetter) {
            auto characterField = Get(object, charactersGetter);
            auto costumeField = Get(object, costumesGetter);
            auto hairField = Get(object, hairsGetter);
            auto characters = Strings(characterField), costumes = Strings(costumeField), hairs = Strings(hairField);
            if (characters.empty() && costumes.empty() && hairs.empty()) return true;
            if (!Aligned(characters, costumes, hairs)) {
                step = "actor roster lengths";
                Log::ErrorFmt("PhotoRequests: roster sizes characters=%zu costumes=%zu hairs=%zu",
                             characters.size(), costumes.size(), hairs.size());
                return false;
            }
            if (rules && MaxActors() > 0 && characters.size() > static_cast<size_t>(MaxActors())) {
                std::vector<int> preference;
                for (const auto& id : characters)
                    preference.push_back(selections.contains(id) ? 2 : rules->outfits.contains(id) ? 1 : 0);
                const auto keep = ProjectRoster(characters.size(), MaxActors(), preference);
                std::vector<std::string> projectedCharacters, projectedCostumes, projectedHairs;
                for (size_t i = 0; i < characters.size(); ++i) {
                    if (std::find(keep.begin(), keep.end(), i) == keep.end()) droppedSources.insert(characters[i]);
                    else {
                        projectedCharacters.push_back(characters[i]); projectedCostumes.push_back(costumes[i]); projectedHairs.push_back(hairs[i]);
                    }
                }
                Log::InfoFmt("PhotoRequests: catalog roster projected %zu -> %zu for %s/%s",
                             characters.size(), projectedCharacters.size(), rules->kind.c_str(), rules->id.c_str());
                characters = std::move(projectedCharacters); costumes = std::move(projectedCostumes); hairs = std::move(projectedHairs);
            }
            if (!selections.empty() && std::all_of(characters.begin(), characters.end(),
                                                 [&](const auto& id) { return selections.contains(id); })) {
                for (size_t i = 0; i < characters.size(); ++i) {
                    const auto saved = selections.at(characters[i]);
                    if (i == 0) primary = saved;
                    ActorSelection verified;
                    if (!Pair(saved.character, saved.costume, saved.hair, verified) ||
                        verified.costume != saved.costume || verified.hair != saved.hair) {
                        step = "stored check plan incompatible with capture"; return false;
                    }
                    characters[i] = saved.character; costumes[i] = saved.costume; hairs[i] = saved.hair;
                }
                return Replace(characterField, characters) && Replace(costumeField, costumes) && Replace(hairField, hairs);
            }
            if (rules) {
                if (MaxActors() > 0 && characters.size() > static_cast<size_t>(MaxActors())) {
                    step = "catalog actor capacity"; return false;
                }
                std::vector<std::set<int>> poseRequirements{requiredPoseTypes};
                if (rules->expression && requiredPoseTypes.empty() && !rules->positions.empty()) {
                    poseRequirements.clear();
                    for (const auto& [number, position] : rules->positions) {
                        if (position.capacity > 0 && static_cast<size_t>(position.capacity) < characters.size()) continue;
                        poseRequirements.push_back(position.poseType ? std::set<int>{position.poseType} : std::set<int>{});
                    }
                }
                std::vector<std::string> assignment;
                for (const auto& requirement : poseRequirements) {
                    requiredPoseTypes = requirement;
                    std::vector<std::vector<std::string>> domains(characters.size());
                    for (size_t i = 0; i < characters.size(); ++i) {
                        for (const auto& [id, options] : rules->outfits) {
                            ActorSelection selected;
                            if (Pair(id, costumes[i], hairs[i], selected)) domains[i].push_back(id);
                        }
                    }
                    assignment = MatchRoster(characters, domains, preserveCharacters);
                    if (!assignment.empty()) break;
                }
                if (assignment.empty()) {
                    step = "catalog has no complete roster";
                    Log::ErrorFmt("PhotoRequests: catalog %s/%s eligible-actors=%zu requested=%zu",
                                 rules->kind.c_str(), rules->id.c_str(), rules->outfits.size(), characters.size());
                    return false;
                }
                for (size_t i = 0; i < characters.size(); ++i) {
                    ActorSelection actor;
                    if (!Pair(assignment[i], costumes[i], hairs[i], actor)) return false;
                    selections[characters[i]] = actor;
                    if (i == 0) primary = actor;
                    characters[i] = actor.character; costumes[i] = actor.costume; hairs[i] = actor.hair;
                }
                step = "catalog roster write";
                return Replace(characterField, characters) && Replace(costumeField, costumes) && Replace(hairField, hairs);
            }
            for (size_t i = 0; i < characters.size(); ++i) {
                ActorSelection actor;
                if (!Actor(characters[i], costumes[i], hairs[i], actor)) return false;
                characters[i] = actor.character; costumes[i] = actor.costume; hairs[i] = actor.hair;
            }
            step = "actor list write";
            return Replace(characterField, characters) && Replace(costumeField, costumes) && Replace(hairField, hairs);
        }

        bool MapCharacter(void* object, const char* getter, const char* setter) {
            if (!MethodOf(object, getter)) return true;
            const auto original = Text(object, getter);
            if (original.empty()) return true;
            auto it = selections.find(original);
            if (it == selections.end()) {
                return droppedSources.contains(original) && !primary.character.empty() && SetText(object, setter, primary.character);
            }
            return SetText(object, setter, it->second.character);
        }

        std::string Pose(const std::string& requested, const ActorSelection& actor, int allowedType) {
            if (rules) {
                const auto candidates = rules->outfits.find(actor.character);
                if (candidates == rules->outfits.end()) return {};
                for (const auto& outfit : candidates->second) {
                    if (outfit.actor.costume != actor.costume || outfit.actor.hair != actor.hair) continue;
                    std::set<std::string> poses;
                    for (const auto& [type, ids] : outfit.poses)
                        if (!allowedType || type == allowedType) poses.insert(ids.begin(), ids.end());
                    return SelectOwnedId(poses, requested, [](const auto&) { return true; });
                }
                return {};
            }
            return SelectOwnedId(owned.poses, requested, [&](const auto& id) {
                if (expression && Contains(Field(expression, "impossiblePhotoPoseIds_"), id)) return false;
                auto pose = Find("get_PhotoPoseMaster", id);
                if (!pose || !MotionExists(id, actor.character)) return false;
                const int type = Get<int>(pose, "get_PoseType");
                if (allowedType && type != allowedType) return false;
                for (auto master : {Find("get_CostumeMaster", actor.costume), Find("get_HairMaster", actor.hair)}) {
                    auto types = Items<int>(Field(master, "impossiblePhotoPoseTypes_"));
                    if (std::find(types.begin(), types.end(), type) != types.end()) return false;
                }
                return true;
            });
        }

        bool ExpressionParams(void* request) {
            // get_CreateShootingParams() creates an empty temporary collection in 6.1.1.
            auto parameters = Get(request, "get_CreateExpressionShootingParams");
            for (auto param : Items(parameters)) {
                if (!MapCharacter(param, "get_MainCharacterId", "set_MainCharacterId")) return false;
                const auto number = Get<int>(param, "get_StagePositionNumber");
                // Resolve the position by its real composite key, not a guessed list index.
                auto positions = Master("get_PhotoExpressionStagePositionMaster");
                static auto resolved = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                    "PhotoExpressionStagePositionMaster", "FindByKey", {"System.String", "System.Int32"});
                auto findPosition = resolved ? static_cast<Method*>(resolved->address) : nullptr;
                if (!positions || !findPosition) return false;
                auto position = reinterpret_cast<void* (*)(void*, String*, int, const Method*)>(findPosition->methodPointer)(
                    positions, String::New(Text(expression, "get_StageId")), number, findPosition);
                if (!position) return false;
                const int type = Get<int>(position, "get_PossiblePhotoPoseType");
                auto idsField = Get(param, "get_CharacterIds");
                auto posesField = Get(param, "get_PhotoPoseIds");
                auto ids = Strings(idsField), poses = Strings(posesField);
                if (ids.size() != poses.size()) return false;
                auto facesField = Get(param, "get_PhotoFacialIds");
                auto looksField = Get(param, "get_PhotoLookingTypes");
                auto faces = Strings(facesField);
                auto looks = Items<int>(looksField);
                if ((!faces.empty() && faces.size() != ids.size()) || (!looks.empty() && looks.size() != ids.size())) return false;
                std::vector<size_t> keep;
                for (size_t i = 0; i < ids.size(); ++i) if (selections.contains(ids[i])) keep.push_back(i);
                // A capture whose original subject was removed still needs a valid canonical subject.
                if (keep.empty() && !ids.empty() && !primary.character.empty()) keep.push_back(0);
                std::vector<std::string> projectedIds, projectedPoses, projectedFaces;
                std::vector<int> projectedLooks;
                for (const size_t i : keep) {
                    const auto selected = selections.find(ids[i]);
                    const auto& actor = selected != selections.end() ? selected->second : primary;
                    const auto pose = Pose(poses[i], actor, type);
                    if (pose.empty()) return false;
                    projectedIds.push_back(actor.character); projectedPoses.push_back(pose);
                    if (!faces.empty()) projectedFaces.push_back(faces[i]);
                    if (!looks.empty()) projectedLooks.push_back(looks[i]);
                }
                if (!Replace(idsField, projectedIds) || !Replace(posesField, projectedPoses) ||
                    (!faces.empty() && !Replace(facesField, projectedFaces)) ||
                    (!looks.empty() && !ReplaceNumbers(looksField, projectedLooks))) return false;
                if (!projectedIds.empty() && !Contains(idsField, Text(param, "get_MainCharacterId")) &&
                    !SetText(param, "set_MainCharacterId", projectedIds.front())) return false;
                const auto main = Text(param, "get_MainCharacterId");
                if (!main.empty()) {
                    auto actor = std::find_if(selections.begin(), selections.end(), [&](const auto& pair) {
                        return pair.second.character == main;
                    });
                    if (actor == selections.end()) return false;
                    auto pose = Pose(Text(param, "get_PhotoPoseId"), actor->second, type);
                    if (pose.empty() || !SetText(param, "set_PhotoPoseId", pose)) return false;
                    const auto mainIndex = std::find(projectedIds.begin(), projectedIds.end(), main);
                    if (mainIndex != projectedIds.end()) {
                        const size_t index = mainIndex - projectedIds.begin();
                        if (!projectedFaces.empty() && !SetText(param, "set_PhotoFacialId", projectedFaces[index])) return false;
                        if (!projectedLooks.empty() && !Set(param, "set_PhotoLookingType", projectedLooks[index])) return false;
                    }
                }
            }
            return parameters != nullptr;
        }
    };

    std::string RequestFingerprint(void* request) {
        std::string result;
        for (const auto field : {"characterId_", "costumeId_", "hairId_", "photoExpressionId_", "startCharacterId_"}) {
            auto value = Field<String*>(request, field);
            result += value ? value->ToString() : "";
            result += '\n';
        }
        for (const auto field : {"characterIds_", "costumeIds_", "hairIds_", "selectedCharacterIds_",
                                 "selectedCostumeIds_", "selectedHairIds_"}) {
            for (const auto& value : Strings(Field(request, field))) result += value + '\n';
            result += '|';
        }
        for (auto param : Items(Field(request, "createExpressionShootingParams_"))) {
            result += Text(param, "get_MainCharacterId") + '\n' + Text(param, "get_PhotoPoseId") + '\n';
            for (const auto field : {"characterIds_", "photoPoseIds_"})
                for (const auto& value : Strings(Field(param, field))) result += value + '\n';
        }
        return result;
    }

    std::string PlanKey(void* request, const std::string& name) {
        std::string key;
        const auto append = [&](std::initializer_list<const char*> getters) {
            for (const auto getter : getters) key += '\n' + Text(request, getter);
        };
        // Only include real wire identity fields, never the creation-only convenience properties.
        if (name.find("Expression") != std::string::npos) {
            key = "expression"; append({"get_PhotoExpressionId"});
        } else if (name.starts_with("PhotoContest")) {
            key = "contest"; append({"get_PhotoContestId", "get_SectionId", "get_PhotoContestActivityId",
                                    "get_PhotoContestQuestMusicId", "get_PhotoContestQuestStageId"});
        } else if (name.starts_with("PhotoPanorama")) {
            key = "panorama"; append({"get_PhotoMusicId"});
        } else if (name.find("Special") != std::string::npos) {
            key = "special"; append({"get_SpecialPhotoShootingId", "get_MusicId", "get_StageId"});
        } else {
            key = "photo"; append({"get_PhotoActivityId", "get_PhotoMusicId", "get_PhotoStageId"});
        }
        if (name.find("Expression") == std::string::npos && !name.starts_with("PhotoPanorama"))
            key += '\n' + std::to_string(Get<int>(request, "get_ActionType"));
        auto ids = Strings(Get(request, "get_CharacterIds"));
        auto costumes = Strings(Get(request, "get_CostumeIds"));
        auto hairs = Strings(Get(request, "get_HairIds"));
        if (ids.empty()) {
            ids = Strings(Get(request, "get_SelectedCharacterIds"));
            costumes = Strings(Get(request, "get_SelectedCostumeIds"));
            hairs = Strings(Get(request, "get_SelectedHairIds"));
        }
        for (const auto& values : {ids, costumes, hairs}) {
            key += '|'; for (const auto& value : values) key += '\n' + value;
        }
        // Expression APIs also support the legacy singular representation.
        if (ids.empty()) for (const auto getter : {"get_CharacterId", "get_CostumeId", "get_HairId"}) key += '\n' + Text(request, getter);
        return key;
    }

    std::string ActivityIdentity(void* request) {
        std::string result;
        for (const auto getter : {"get_ActivityFanEventId", "get_ActivityPromotionId", "get_ActivityRefreshId"})
            result += '\n' + Text(request, getter);
        result += '\n' + std::to_string(Get<int>(request, "get_ActivityRefreshLevel"));
        for (const auto getter : {"get_CharacterIds", "get_ItemIds"}) {
            result += '|'; for (const auto& id : Strings(Get(request, getter))) result += '\n' + id;
        }
        return result;
    }

    bool Sanitize(void* request, Context& context, const std::string& name) {
        const auto useScene = [&](const std::string& kind, const std::string& id) {
            if (!context.scenes) return false;
            const auto scene = context.scenes->find(SceneKey(kind, id));
            if (scene == context.scenes->end()) return false;
            context.rules = &scene->second;
            return true;
        };
        useScene("Owned", "default");
        if (name.starts_with("ActivityStart")) {
            context.preserveCharacters = true;
            context.step = "activity costume conditions";
            bool found = false;
            if (name == "ActivityStartPromotionRequest")
                found = useScene("Promotion", Text(request, "get_ActivityPromotionId"));
            else if (name == "ActivityStartFanEventRequest")
                found = useScene("FanEvent", Text(request, "get_ActivityFanEventId"));
            else if (name == "ActivityStartRefreshRequest")
                found = useScene("Refresh", Text(request, "get_ActivityRefreshId") + "/" +
                                             std::to_string(Get<int>(request, "get_ActivityRefreshLevel")));
            if (!found) return false;
            return context.Actors(request, "get_CharacterIds", "get_CostumeIds", "get_HairIds");
        }
        if (name == "CostumeSetRequest" || name == "CostumeLiveSetRequest") {
            auto costume = Find("get_CostumeMaster", Text(request, "get_CostumeId"));
            auto hair = Find("get_HairMaster", Text(request, "get_HairId"));
            auto character = costume ? Text(costume, "get_CharacterId") : Text(hair, "get_CharacterId");
            ActorSelection actor;
            return context.Pair(character, Text(request, "get_CostumeId"), Text(request, "get_HairId"), actor) &&
                   SetText(request, "set_CostumeId", actor.costume) && SetText(request, "set_HairId", actor.hair);
        }
        if (name == "CostumeCheckBulkRequest") {
            auto costumes = Strings(Get(request, "get_CostumeIds")), hairs = Strings(Get(request, "get_HairIds"));
            for (auto& id : costumes) {
                ActorSelection actor;
                auto master = Find("get_CostumeMaster", id);
                if (!context.Pair(Text(master, "get_CharacterId"), id, "", actor)) return false;
                id = actor.costume;
            }
            for (auto& id : hairs) {
                ActorSelection actor;
                auto master = Find("get_HairMaster", id);
                if (!context.Pair(Text(master, "get_CharacterId"), "", id, actor)) return false;
                id = actor.hair;
            }
            return Replace(Get(request, "get_CostumeIds"), costumes) && Replace(Get(request, "get_HairIds"), hairs);
        }

        const bool expression = name == "PhotoCheckExpressionShootingRequest" || name == "PhotoCreateExpressionShootingsRequest";
        if (expression) {
            context.expressionId = Text(request, "get_PhotoExpressionId");
            context.step = "expression master";
            auto known = context.owned.unlocked.find("PhotoExpression");
            // IsUnlocked getters can be inlined by IL2CPP. An unobserved value is not a locked scene.
            // With scene unlocking off, keep the scene selected by the game's normal UI.
            if (Config::unlockAllLive && IsKnownLocked(context.owned, "PhotoExpression", context.expressionId)) {
                context.expressionId.clear();
                for (const auto& [id, unlocked] : known->second) {
                    if (unlocked) { context.expressionId = id; break; }
                }
                if (context.expressionId.empty() || !SetText(request, "set_PhotoExpressionId", context.expressionId)) return false;
            }
            context.expression = Find("get_PhotoExpressionMaster", context.expressionId);
            if (!context.expression || !useScene("PhotoExpression", context.expressionId)) return false;
            if (name == "PhotoCreateExpressionShootingsRequest") {
                for (auto param : Items(Get(request, "get_CreateExpressionShootingParams"))) {
                    const int number = Get<int>(param, "get_StagePositionNumber");
                    const auto position = context.rules->positions.find(number);
                    if (position == context.rules->positions.end()) { context.step = "unknown stage position"; return false; }
                    if (position->second.poseType) context.requiredPoseTypes.insert(position->second.poseType);
                }
            }
        }
        if (Config::unlockAllLive && (name == "PhotoCheckShootingRequest" || name == "PhotoCreateShootingsRequest")) {
            const auto pick = [&](const char* kind, const char* getter, const char* setter) {
                const auto known = context.owned.unlocked.find(kind);
                if (known == context.owned.unlocked.end()) return false;
                const auto current = Text(request, getter);
                if (known->second.contains(current) && known->second.at(current)) return true;
                for (const auto& [id, unlocked] : known->second) if (unlocked) return SetText(request, setter, id);
                return false;
            };
            const int action = Get<int>(request, "get_ActionType");
            if (action == 1 || action == 2) {
                if (!pick("PhotoActivity", "get_PhotoActivityId", "set_PhotoActivityId")) return false;
                if (!SetText(request, "set_PhotoMusicId", "") || !SetText(request, "set_PhotoStageId", "")) return false;
            } else if (action == 3) {
                if (!pick("PhotoMusic", "get_PhotoMusicId", "set_PhotoMusicId") ||
                    !pick("PhotoStage", "get_PhotoStageId", "set_PhotoStageId") ||
                    !SetText(request, "set_PhotoActivityId", "")) return false;
            }
        }
        if (!expression) {
            const int action = Get<int>(request, "get_ActionType");
            if (name.starts_with("PhotoContest")) {
                if (action == 1 || action == 2)
                    useScene("PhotoContestActivity", Text(request, "get_PhotoContestActivityId"));
                else if (action == 3) useScene("PhotoContestQuestStage", Text(request, "get_PhotoContestQuestStageId"));
            } else if (name.find("Special") != std::string::npos) {
                if (action == 1 || action == 2) useScene("SpecialPhotoActivity", std::to_string(action));
                else if (action == 3) {
                    useScene("SpecialPhotoQuestMusic", Text(request, "get_MusicId"));
                    if (context.scenes) {
                        const auto stage = context.scenes->find(SceneKey("SpecialPhotoQuestStage", Text(request, "get_StageId")));
                        if (stage != context.scenes->end()) context.capacityLimit = stage->second.maxActors;
                    }
                }
            } else if (action == 1 || action == 2) useScene("PhotoActivity", Text(request, "get_PhotoActivityId"));
            else if (action == 3) useScene("PhotoQuestStage", Text(request, "get_PhotoStageId"));
        }
        context.step = "actor roster";
        if (MethodOf(request, "get_CharacterIds") &&
            !context.Actors(request, "get_CharacterIds", "get_CostumeIds", "get_HairIds")) return false;
        if (MethodOf(request, "get_SelectedCharacterIds") &&
            !context.Actors(request, "get_SelectedCharacterIds", "get_SelectedCostumeIds", "get_SelectedHairIds")) return false;
        if (expression && !Text(request, "get_CharacterId").empty()) {
            context.step = "single actor costume/hair";
            ActorSelection actor;
            if (!context.Actor(Text(request, "get_CharacterId"), Text(request, "get_CostumeId"), Text(request, "get_HairId"), actor) ||
                !SetText(request, "set_CharacterId", actor.character) || !SetText(request, "set_CostumeId", actor.costume) ||
                !SetText(request, "set_HairId", actor.hair)) return false;
        }
        if (context.expression) {
            context.step = "expression actor count";
            auto count = Strings(Get(request, "get_CharacterIds")).size();
            if (count == 0) count = 1;
            const int maximum = Get<int>(context.expression, "GetMaxCharacterAmount");
            if (maximum > 0 && count > static_cast<size_t>(maximum)) return false;
        }
        context.step = "main/start actor mapping";
        if (!context.MapCharacter(request, "get_MainCharacterId", "set_MainCharacterId") ||
            !context.MapCharacter(request, "get_StartCharacterId", "set_StartCharacterId")) return false;
        context.step = "nested shooting parameters";
        if (name == "PhotoCreateExpressionShootingsRequest") return context.ExpressionParams(request);
        // These collections are actual protobuf fields. The contest convenience property is not serialized.
        if (name != "PhotoContestSubmitShootingRequest") {
            for (auto param : Items(Get(request, "get_CreateShootingParams")))
                if (!context.MapCharacter(param, "get_MainCharacterId", "set_MainCharacterId")) return false;
        }
        return !context.selections.empty();
    }


}

    void CaptureInitial(void* response) {
        if (!response || std::string(Il2cppUtils::get_class_from_instance(response)->name) != "UserGetResponse") return;
        Inventory fresh;
        if (!ReadInventory(response, fresh, true)) return;
        std::lock_guard lock(inventoryMutex);
        // Initial responses can be read repeatedly; preserve unlock observations for the same account data.
        if (inventory.actors == fresh.actors) fresh.unlocked = std::move(inventory.unlocked);
        inventory = std::move(fresh);
        ++inventoryRevision;
        storedPlans.clear();
        Log::InfoFmt("PhotoRequests: server inventory ready=%d actors=%zu costumes=%zu hairs=%zu poses=%zu",
                     inventory.ready, inventory.actors.size(), inventory.costumes.size(), inventory.hairs.size(), inventory.poses.size());
    }

    void CaptureUpdate(void* data) {
        if (!data || std::string(Il2cppUtils::get_class_from_instance(data)->name) != "UpdatedData") return;
        std::lock_guard lock(inventoryMutex);
        if (inventory.ready) {
            const auto before = std::tuple{inventory.actors, inventory.costumes.size(), inventory.hairs.size(), inventory.poses.size()};
            ReadInventory(data, inventory, false);
            if (before != std::tuple{inventory.actors, inventory.costumes.size(), inventory.hairs.size(), inventory.poses.size()})
                ++inventoryRevision;
        }
    }

    void CaptureAvailability(void* response) {
        if (!response) return;
        const std::string name = Il2cppUtils::get_class_from_instance(response)->name;
        auto source = name == "PhotoContestTopResponse" ? Get(response, "get_PhotoContestInfo") : response;
        if (!source) return;
        std::map<std::string, std::map<std::string, bool>> observations;
        SceneCatalog additional;
        const auto collect = [&](const char* getter, const char* kind) {
            for (auto item : Items(Get(source, getter))) {
                auto id = AvailabilityId(item);
                if (std::string_view(kind) == "SpecialPhotoActivity") id = std::to_string(Get<int>(item, "get_ActionType"));
                if (id.empty()) continue;
                observations[kind][id] = Field<bool>(item, "isUnlocked_");
                if (std::string_view(kind).starts_with("SpecialPhoto")) {
                    auto scene = ReadScene(item, kind, id);
                    additional[SceneKey(kind, id)] = std::move(scene);
                    for (auto stage : Items(Get(item, "get_PhotoStages"))) {
                        const auto stageId = Text(stage, "get_StageId");
                        auto stageRules = ReadScene(stage, "SpecialPhotoQuestStage", stageId);
                        additional[SceneKey("SpecialPhotoQuestStage", stageId)] = std::move(stageRules);
                    }
                }
            }
        };
        if (name == "PhotoListShootingResponse") {
            collect("get_Expressions", "PhotoExpression"); collect("get_Activities", "PhotoActivity");
            collect("get_Musics", "PhotoMusic"); collect("get_Stages", "PhotoStage");
        } else if (name == "PhotoListSpecialShootingResponse") {
            collect("get_Activities", "SpecialPhotoActivity"); collect("get_Musics", "SpecialPhotoQuestMusic");
        } else if (name == "PhotoContestTopResponse") {
            collect("get_Activities", "PhotoContestActivity"); collect("get_Musics", "PhotoContestQuestMusic");
            collect("get_Stages", "PhotoContestQuestStage");
        } else return;
        bool changed = !additional.empty();
        {
            std::lock_guard lock(inventoryMutex);
            for (const auto& [kind, rows] : observations)
                for (const auto& [id, unlocked] : rows) {
                    auto& known = inventory.unlocked[kind];
                    changed |= !known.contains(id) || known[id] != unlocked;
                    known[id] = unlocked;
                }
            if (changed) ++inventoryRevision;
        }
        if (!additional.empty()) {
            std::lock_guard lock(catalogMutex);
            for (auto& [key, scene] : additional) dynamicScenes[key] = std::move(scene);
        }
        // Response parsing must remain short: build the catalog only when a request needs it.
    }

    void ObserveUnlock(void* item, bool unlocked) {
        if (!item) return;
        const std::string kind = Il2cppUtils::get_class_from_instance(item)->name;
        auto id = AvailabilityId(item);
        if (id.empty()) return;
        std::lock_guard lock(inventoryMutex);
        inventory.unlocked[kind][id] = unlocked;
    }

    void SetValidators(CostumeValidator costume, HairValidator hair) {
        validateCostume = costume;
        validateHair = hair;
        Log::Info("PhotoRequests: native revision=activity-rules-v5-login-recovery");
    }

    PreparedRequest::PreparedRequest(void* original) : request_(original) {}
    PreparedRequest::PreparedRequest(void* request, uint32_t handle) : request_(request), handle_(handle) {}
    PreparedRequest::PreparedRequest(PreparedRequest&& other) noexcept
        : request_(other.request_), handle_(std::exchange(other.handle_, 0)) {}
    PreparedRequest::~PreparedRequest() {
        if (handle_) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle_);
    }
    void* PreparedRequest::get() const {
        return handle_ ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", handle_) : request_;
    }

    PreparedRequest Prepare(void* request) {
        if (!request || (!Config::unlockAllLiveCostume && !Config::unlockAllPhotoPose && !Config::unlockAllLive)) return PreparedRequest(request);
        const std::string name = Il2cppUtils::get_class_from_instance(request)->name;
        if (name.starts_with("Costume") && !Config::unlockAllLiveCostume) return PreparedRequest(request);
        const bool activity = name.starts_with("ActivityStart");
        if (activity && !Config::unlockAllLiveCostume) return PreparedRequest(request);
        Context context;
        { std::lock_guard lock(inventoryMutex); context.owned = inventory; }
        if (!context.owned.ready) {
            Log::ErrorFmt("PhotoRequests: %s unchanged, server inventory unavailable", name.c_str());
            return PreparedRequest(request);
        }
        if (activity) RefreshActivityRules();
        context.scenes = EnsureCatalog();
        const auto planKey = PlanKey(request, name);
        uint64_t revision;
        {
            std::lock_guard lock(inventoryMutex);
            revision = inventoryRevision;
            const auto saved = storedPlans.find(planKey);
            if (!activity && saved != storedPlans.end() && saved->second.revision == revision) context.selections = saved->second.selections;
        }
        const auto originalFingerprint = RequestFingerprint(request);
        auto clone = Get(request, "Clone");
        if (!clone) return PreparedRequest(request);
        const auto handle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new", clone, false);
        if (!handle) return PreparedRequest(request);
        PreparedRequest candidate(clone, handle);
        if (!Sanitize(clone, context, name)) {
            const auto details = Text(request, "ToString");
            Log::ErrorFmt("PhotoRequests: %s unchanged, failed at %s; original=%s",
                         name.c_str(), context.step, details.c_str());
            return PreparedRequest(request);
        }
        if (RequestFingerprint(request) != originalFingerprint) {
            Log::ErrorFmt("PhotoRequests: %s clone isolation failed", name.c_str());
            return PreparedRequest(request);
        }
        if (activity && ActivityIdentity(clone) != ActivityIdentity(request)) {
            Log::ErrorFmt("PhotoRequests: %s activity identity changed; original retained", name.c_str());
            return PreparedRequest(request);
        }
        Log::InfoFmt("PhotoRequests: %s prepared on clone, actors=%zu original-preserved=1 bytes=%d",
                     name.c_str(), context.selections.size(), Get<int>(clone, "CalculateSize"));
        if (Config::dbgMode && name.find("Check") != std::string::npos)
            Log::DebugFmt("PhotoRequests: prepared check=%s", Text(clone, "ToString").c_str());
        if (Config::dbgMode && activity)
            Log::DebugFmt("PhotoRequests: prepared activity=%s", Text(clone, "ToString").c_str());
        if (!activity && !context.selections.empty()) {
            std::lock_guard lock(inventoryMutex);
            if (revision == inventoryRevision) storedPlans[planKey] = {revision, context.selections};
        }
        return candidate;
    }
}
