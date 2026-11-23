#include "PhotoRequests.hpp"
#include "PhotoRequestPlan.hpp"
#include "Il2cppUtils.hpp"
#include "config/Config.hpp"
#include "Local.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace HoshimiLocal::PhotoRequests {
namespace {
    using String = UnityResolve::UnityType::String;
    using Method = Il2cppUtils::MethodInfo;

    std::mutex inventoryMutex;
    Inventory inventory;
    CostumeValidator validateCostume{};
    HairValidator validateHair{};

    std::mutex specialSceneMutex;
    SceneCatalog specialScenes;

    std::mutex costumeRoomMutex;
    std::string activeCostumeRoomCharacterId;

    struct ActivityVisualRecord {
        std::string sceneId;
        std::map<std::string, std::pair<std::string, std::string>> outfits;
    };
    std::mutex activityVisualMutex;
    bool activityVisualLoaded = false;
    std::map<std::string, ActivityVisualRecord> activityVisuals;

    struct DeckVisualSlot {
        std::string cardId;
        std::string characterId;
        std::string costumeId;
        std::string hairId;
    };
    std::mutex deckVisualMutex;
    bool deckVisualLoaded = false;
    std::map<std::string, std::map<int, DeckVisualSlot>> deckVisuals;
    std::string activeLiveDeckKey;
    std::string selfPublicUserId;

    void LoadDeckVisualsLocked() {
        if (deckVisualLoaded) return;
        deckVisualLoaded = true;
        try {
            const auto path = Local::GetBasePath() / "deck-visual-outfits.json";
            std::ifstream input(path, std::ios::binary);
            if (!input) return;
            auto root = nlohmann::json::parse(input, nullptr, false);
            if (!root.is_object()) return;
            for (const auto& [deckKey, slots] : root.items()) {
                if (!slots.is_object()) continue;
                std::map<int, DeckVisualSlot> deckSlots;
                for (const auto& [posStr, entry] : slots.items()) {
                    if (!entry.is_object()) continue;
                    int pos = 0;
                    try { pos = std::stoi(posStr); } catch (...) { continue; }
                    DeckVisualSlot slot{
                        entry.value("cardId", ""),
                        entry.value("characterId", ""),
                        entry.value("costume", ""),
                        entry.value("hair", "")
                    };
                    if (!slot.cardId.empty() && (!slot.costumeId.empty() || !slot.hairId.empty())) {
                        deckSlots[pos] = std::move(slot);
                    }
                }
                if (!deckSlots.empty()) deckVisuals[deckKey] = std::move(deckSlots);
            }
        } catch (...) {
        }
    }

    void SaveDeckVisualsLocked() {
        try {
            nlohmann::json root = nlohmann::json::object();
            for (const auto& [deckKey, slots] : deckVisuals) {
                if (slots.empty()) continue;
                nlohmann::json slotsJson = nlohmann::json::object();
                for (const auto& [pos, slot] : slots) {
                    slotsJson[std::to_string(pos)] = {
                        {"cardId", slot.cardId},
                        {"characterId", slot.characterId},
                        {"costume", slot.costumeId},
                        {"hair", slot.hairId}
                    };
                }
                root[deckKey] = std::move(slotsJson);
            }
            const auto path = Local::GetBasePath() / "deck-visual-outfits.json";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (output) output << root.dump(2);
        } catch (...) {
        }
    }

    void LoadActivityVisualsLocked() {
        if (activityVisualLoaded) return;
        activityVisualLoaded = true;
        try {
            const auto path = Local::GetBasePath() / "activity-visual-outfits.json";
            std::ifstream input(path, std::ios::binary);
            if (!input) return;
            auto root = nlohmann::json::parse(input, nullptr, false);
            if (!root.is_object()) return;
            for (const auto& [kind, entry] : root.items()) {
                if (!entry.is_object()) continue;
                ActivityVisualRecord record;
                record.sceneId = entry.value("sceneId", "");
                if (record.sceneId.empty()) continue;
                if (const auto it = entry.find("outfits"); it != entry.end() && it->is_object()) {
                    for (const auto& [cid, pair] : it->items()) {
                        if (!pair.is_object()) continue;
                        const auto costume = pair.value("costume", "");
                        const auto hair = pair.value("hair", "");
                        if (!cid.empty() && !costume.empty() && !hair.empty()) {
                            record.outfits[cid] = {costume, hair};
                        }
                    }
                }
                if (!record.outfits.empty()) activityVisuals[kind] = std::move(record);
            }
        } catch (...) {
        }
    }

    void SaveActivityVisualsLocked() {
        try {
            nlohmann::json root = nlohmann::json::object();
            for (const auto& [kind, record] : activityVisuals) {
                nlohmann::json outfits = nlohmann::json::object();
                for (const auto& [cid, pair] : record.outfits) {
                    outfits[cid] = {{"costume", pair.first}, {"hair", pair.second}};
                }
                root[kind] = {{"sceneId", record.sceneId}, {"outfits", std::move(outfits)}};
            }
            const auto path = Local::GetBasePath() / "activity-visual-outfits.json";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (output) output << root.dump(2);
        } catch (...) {
        }
    }

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

    bool Contains(void* list, const std::string& value) {
        auto strings = Strings(list);
        return std::find(strings.begin(), strings.end(), value) != strings.end();
    }

    bool ContainsStr(const std::vector<std::string>& list, const std::string& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    bool IsPhotoRequestName(const std::string& name) {
        return name.starts_with("Photo") &&
               (name.find("Check") != std::string::npos ||
                name.find("Create") != std::string::npos ||
                name.find("Submit") != std::string::npos);
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

    bool CostumeAllowed(void* costume, void* specifier, const std::string& character) {
        if (!costume || Text(costume, "get_CharacterId") != character ||
            Field<bool>(costume, "isDisableAdmin_") || Field<bool>(costume, "isUserUnavailable_")) return false;
        if (!specifier) return true;
        // Use the unhooked game predicate. ForceCostumeTypeId is not an additional AND
        // constraint in IsAppropriateCostume (verified against the 6.1.1 ARM64 code).
        return validateCostume && validateCostume(specifier, costume, nullptr);
    }

    bool HairAllowed(void* hair, const std::string& character, const std::string& costume) {
        if (!hair || Text(hair, "get_CharacterId") != character || Field<bool>(hair, "isDisableAdmin_")) return false;
        auto costumeMaster = Find("get_CostumeMaster", costume);
        return costumeMaster && validateHair && validateHair(hair, costumeMaster, nullptr);
    }

    std::set<int> CollectExpressionPoseTypes(void* specifier) {
        std::set<int> types;
        if (!specifier || !MethodOf(specifier, "GetStagePositionList")) return types;
        for (auto position : Items(Get(specifier, "GetStagePositionList"))) {
            const int poseType = Get<int>(position, "get_PossiblePhotoPoseType");
            if (poseType != 0) types.insert(poseType);
        }
        return types;
    }

    bool OutfitSupportsPoseTypes(void* costumeMaster, void* hairMaster, const std::set<int>& requiredPoseTypes) {
        if (requiredPoseTypes.empty()) return true;
        for (auto master : {costumeMaster, hairMaster}) {
            if (!master) continue;
            const auto blocked = Items<int>(Field(master, "impossiblePhotoPoseTypes_"));
            for (const int required : requiredPoseTypes) {
                if (required != 0 && std::find(blocked.begin(), blocked.end(), required) != blocked.end()) {
                    return false;
                }
            }
        }
        return true;
    }

    bool ResolveExpressionStagePoseType(void* expression, int number, int& outPoseType) {
        if (!expression) return false;
        auto positions = Master("get_PhotoExpressionStagePositionMaster");
        static auto resolved = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
            "PhotoExpressionStagePositionMaster", "FindByKey", {"System.String", "System.Int32"});
        auto findPosition = resolved ? static_cast<Method*>(resolved->address) : nullptr;
        if (!positions || !findPosition) return false;
        auto position = reinterpret_cast<void* (*)(void*, String*, int, const Method*)>(findPosition->methodPointer)(
            positions, String::New(Text(expression, "get_StageId")), number, findPosition);
        if (!position) return false;
        outPoseType = Get<int>(position, "get_PossiblePhotoPoseType");
        return true;
    }

    std::string FindOwnedCompatiblePose(const Inventory& owned, void* expression, const std::string& requested,
                                        const std::string& characterId, const std::string& costumeId,
                                        const std::string& hairId, int allowedType) {
        return SelectOwnedId(owned.poses, requested, [&](const auto& id) {
            if (expression && Contains(Field(expression, "impossiblePhotoPoseIds_"), id)) return false;
            auto pose = Find("get_PhotoPoseMaster", id);
            if (!pose || !MotionExists(id, characterId)) return false;
            const int type = Get<int>(pose, "get_PoseType");
            if (allowedType && type != allowedType) return false;
            for (auto master : {Find("get_CostumeMaster", costumeId), Find("get_HairMaster", hairId)}) {
                auto types = Items<int>(Field(master, "impossiblePhotoPoseTypes_"));
                if (std::find(types.begin(), types.end(), type) != types.end()) return false;
            }
            return true;
        });
    }

    bool OutfitHasOwnedPosesForTypes(const Inventory& owned, void* expression, const std::string& characterId,
                                     const std::string& costumeId, const std::string& hairId,
                                     const std::set<int>& requiredPoseTypes) {
        for (const int poseType : requiredPoseTypes) {
            if (FindOwnedCompatiblePose(owned, expression, "", characterId, costumeId, hairId, poseType).empty()) {
                return false;
            }
        }
        return true;
    }

    bool FindOwnedOutfitPair(const Inventory& owned, const std::string& characterId,
                             const std::string& preferredCostumeId, const std::string& preferredHairId,
                             void* specifier, std::string& outCostume, std::string& outHair,
                             const SceneRules* specialRules = nullptr,
                             const std::set<int>& preferredPoseTypes = {},
                             bool requirePoseTypes = false,
                             void* expressionForPoseCheck = nullptr) {
        const auto actorIt = owned.actors.find(characterId);
        if (actorIt == owned.actors.end()) return false;
        const auto& equipped = actorIt->second;

        std::vector<std::string> costumeOrder;
        const auto pushCostume = [&](const std::string& id) {
            if (!id.empty() && owned.costumes.contains(id) &&
                std::find(costumeOrder.begin(), costumeOrder.end(), id) == costumeOrder.end()) {
                costumeOrder.push_back(id);
            }
        };
        pushCostume(preferredCostumeId);
        pushCostume(equipped.liveCostume);
        pushCostume(equipped.favoriteCostume);
        for (const auto& id : owned.costumes) pushCostume(id);

        std::vector<std::string> hairOrder;
        const auto pushHair = [&](const std::string& id) {
            if (!id.empty() && owned.hairs.contains(id) &&
                std::find(hairOrder.begin(), hairOrder.end(), id) == hairOrder.end()) {
                hairOrder.push_back(id);
            }
        };
        pushHair(preferredHairId);
        pushHair(equipped.liveHair);
        pushHair(equipped.favoriteHair);
        for (const auto& id : owned.hairs) pushHair(id);

        const auto search = [&](bool checkPoses) -> bool {
            for (const auto& costumeId : costumeOrder) {
                auto costumeMaster = Find("get_CostumeMaster", costumeId);
                if (!CostumeAllowed(costumeMaster, specifier, characterId)) continue;
                if (specialRules && specialRules->hasCostumeRules &&
                    !ContainsStr(specialRules->costumeTypes, Text(costumeMaster, "get_CostumeTypeId"))) {
                    continue;
                }
                for (const auto& hairId : hairOrder) {
                    auto hairMaster = Find("get_HairMaster", hairId);
                    if (!HairAllowed(hairMaster, characterId, costumeId)) continue;
                    if (checkPoses) {
                        if (!OutfitSupportsPoseTypes(costumeMaster, hairMaster, preferredPoseTypes)) continue;
                        if (expressionForPoseCheck &&
                            !OutfitHasOwnedPosesForTypes(owned, expressionForPoseCheck, characterId,
                                                         costumeId, hairId, preferredPoseTypes)) {
                            continue;
                        }
                    }
                    outCostume = costumeId;
                    outHair = hairId;
                    return true;
                }
            }
            return false;
        };

        if (!preferredPoseTypes.empty()) {
            if (search(true)) return true;
            if (requirePoseTypes) return false;
        }
        return search(false);
    }

    void* BuildForceActorCostume(const std::string& characterId, void* costumeSpecifier) {
        if (characterId.empty() || !costumeSpecifier) return nullptr;
        auto forcedType = Text(costumeSpecifier, "get_ForceCostumeTypeId");
        if (forcedType.empty()) forcedType = Text(Get(costumeSpecifier, "get_ForceCostumeType"), "get_Id");
        if (forcedType.empty()) return nullptr;
        static auto force = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Data",
            "CostumeSpecifierExtensions", "GetForceCostume");
        static auto actorClass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Data", "ActorCostume");
        auto character = Find("get_CharacterMaster", characterId);
        if (!force || !actorClass || !character) return nullptr;
        auto costume = reinterpret_cast<void* (*)(void*, void*, void*)>(force->function)(
            costumeSpecifier, character, force->address);
        if (!costume) return nullptr;
        auto actor = actorClass->New<void*>();
        auto ctor = MethodOf(actor, ".ctor", 2);
        if (!actor || !ctor) return nullptr;
        reinterpret_cast<void (*)(void*, void*, void*, const Method*)>(ctor->methodPointer)(
            actor, costume, nullptr, ctor);
        return Text(actor, "get_CostumeId").empty() ? nullptr : actor;
    }

    SceneRules ReadSpecialActivityRules(void* specifier, const std::string& id) {
        SceneRules scene;
        scene.kind = "SpecialPhotoActivity";
        scene.id = id;
        scene.hasCostumeRules = MethodOf(specifier, "get_CostumeTypes") != nullptr;
        scene.forcedCostumeType = Text(specifier, "get_ForceCostumeTypeId");
        if (scene.forcedCostumeType.empty()) scene.forcedCostumeType = Text(Get(specifier, "get_ForceCostumeType"), "get_Id");
        for (auto type : Items(Get(specifier, "get_CostumeTypes"))) scene.costumeTypes.push_back(Text(type, "get_Id"));
        scene.forbiddenCharacters = Strings(Get(specifier, "get_ImpossibleShootingCharacterIds"));
        scene.allowedCharacters = Strings(Get(specifier, "get_CharacterIds"));
        scene.action = MethodOf(specifier, "get_ActionType") ? Get<int>(specifier, "get_ActionType") : Get<int>(specifier, "get_Type");
        return scene;
    }

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

    void* ResolveRequestCostumeSpecifier(void* request, const std::string& name) {
        if (name == "PhotoCheckExpressionShootingRequest" || name == "PhotoCreateExpressionShootingsRequest") {
            return Find("get_PhotoExpressionMaster", Text(request, "get_PhotoExpressionId"));
        }
        const int action = MethodOf(request, "get_ActionType") ? Get<int>(request, "get_ActionType") : 0;
        if (action == 1 || action == 2) {
            if (name.starts_with("PhotoContest")) {
                return Find("get_PhotoContestActivityMaster", Text(request, "get_PhotoContestActivityId"));
            }
            if (name.find("Special") == std::string::npos) {
                return Find("get_PhotoActivityMaster", Text(request, "get_PhotoActivityId"));
            }
        }
        return nullptr;
    }

    bool ResolveRequestRules(void* request, const std::string& name,
                             void*& outSpecifier, SceneRules& outSpecialRules, bool& outHasSpecialRules) {
        outSpecifier = nullptr;
        outHasSpecialRules = false;
        const bool isExpression = name == "PhotoCheckExpressionShootingRequest" ||
                                  name == "PhotoCreateExpressionShootingsRequest";
        const int action = MethodOf(request, "get_ActionType") ? Get<int>(request, "get_ActionType") : 0;
        const bool isSpecialActivity = name.find("Special") != std::string::npos && (action == 1 || action == 2);
        if (isSpecialActivity) {
            std::lock_guard lock(specialSceneMutex);
            const auto it = specialScenes.find(SceneKey("SpecialPhotoActivity", std::to_string(action)));
            if (it == specialScenes.end()) return false;
            outSpecialRules = it->second;
            outHasSpecialRules = true;
            return true;
        }
        outSpecifier = ResolveRequestCostumeSpecifier(request, name);
        return !(isExpression || action == 1 || action == 2) || outSpecifier != nullptr;
    }

    bool IsActorOutfitValid(const Inventory& owned, const std::string& characterId,
                            const std::string& costumeId, const std::string& hairId,
                            void* specifier, const SceneRules* specialRules) {
        if (characterId.empty() || costumeId.empty() || hairId.empty()) return false;
        if (!owned.actors.contains(characterId)) return false;
        if (specialRules) {
            if (ContainsStr(specialRules->forbiddenCharacters, characterId)) return false;
            if (!specialRules->allowedCharacters.empty() && !ContainsStr(specialRules->allowedCharacters, characterId)) {
                return false;
            }
        }
        if (owned.costumes.contains(costumeId) && owned.hairs.contains(hairId)) {
            auto costumeMaster = Find("get_CostumeMaster", costumeId);
            auto hairMaster = Find("get_HairMaster", hairId);
            bool costumeOk = CostumeAllowed(costumeMaster, specifier, characterId);
            if (costumeOk && specialRules && specialRules->hasCostumeRules) {
                costumeOk = ContainsStr(specialRules->costumeTypes, Text(costumeMaster, "get_CostumeTypeId"));
            }
            if (costumeOk && HairAllowed(hairMaster, characterId, costumeId)) {
                return true;
            }
        }
        if (specifier) {
            if (auto forceActor = BuildForceActorCostume(characterId, specifier)) {
                if (Text(forceActor, "get_CostumeId") == costumeId) {
                    if (Text(forceActor, "get_HairId") == hairId ||
                        (owned.hairs.contains(hairId) &&
                         HairAllowed(Find("get_HairMaster", hairId), characterId, costumeId))) {
                        return true;
                    }
                }
            }
        }
        if (specialRules && !specialRules->forcedCostumeType.empty()) {
            auto costumeMaster = Find("get_CostumeMaster", costumeId);
            if (costumeMaster && Text(costumeMaster, "get_CharacterId") == characterId &&
                !Field<bool>(costumeMaster, "isDisableAdmin_") &&
                !Field<bool>(costumeMaster, "isUserUnavailable_") &&
                Text(costumeMaster, "get_CostumeTypeId") == specialRules->forcedCostumeType) {
                if (Text(costumeMaster, "get_DefaultHairId") == hairId ||
                    (owned.hairs.contains(hairId) &&
                     HairAllowed(Find("get_HairMaster", hairId), characterId, costumeId))) {
                    return true;
                }
            }
        }
        return false;
    }

    bool ArePhotoRequestActorsOwnedValid(void* request, const std::string& name, const Inventory& owned) {
        const bool isExpression = name == "PhotoCheckExpressionShootingRequest" ||
                                  name == "PhotoCreateExpressionShootingsRequest";
        void* specifier = nullptr;
        SceneRules specialRules;
        bool hasSpecialRules = false;
        if (!ResolveRequestRules(request, name, specifier, specialRules, hasSpecialRules)) return false;
        const SceneRules* specialPtr = hasSpecialRules ? &specialRules : nullptr;

        bool checkedAny = false;
        const auto checkRoster = [&](const char* cGetter, const char* cosGetter, const char* hGetter) -> bool {
            if (!MethodOf(request, cGetter)) return true;
            auto chars = Strings(Get(request, cGetter));
            auto costumes = Strings(Get(request, cosGetter));
            auto hairs = Strings(Get(request, hGetter));
            if (chars.empty() && costumes.empty() && hairs.empty()) return true;
            if (!Aligned(chars, costumes, hairs)) return false;
            checkedAny = true;
            for (size_t i = 0; i < chars.size(); ++i) {
                if (!IsActorOutfitValid(owned, chars[i], costumes[i], hairs[i], specifier, specialPtr)) return false;
            }
            return true;
        };

        if (!checkRoster("get_CharacterIds", "get_CostumeIds", "get_HairIds")) return false;
        if (!checkRoster("get_SelectedCharacterIds", "get_SelectedCostumeIds", "get_SelectedHairIds")) return false;
        if (isExpression && !Text(request, "get_CharacterId").empty()) {
            checkedAny = true;
            if (!IsActorOutfitValid(owned, Text(request, "get_CharacterId"), Text(request, "get_CostumeId"),
                                    Text(request, "get_HairId"), specifier, specialPtr)) return false;
        }
        return checkedAny;
    }

    PreparedRequest CloneRequest(void* request) {
        auto clone = Get(request, "Clone");
        if (!clone) return PreparedRequest(request);
        const auto handle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new", clone, false);
        if (!handle) return PreparedRequest(request);
        return PreparedRequest(clone, handle);
    }

    PreparedRequest PrepareCostumeRequest(void* request, const std::string& name, const Inventory& owned) {
        if (name == "CostumeSetRequest" || name == "CostumeLiveSetRequest") {
            const auto reqCostume = Text(request, "get_CostumeId");
            const auto reqHair = Text(request, "get_HairId");
            auto costumeMaster = Find("get_CostumeMaster", reqCostume);
            auto hairMaster = Find("get_HairMaster", reqHair);
            const auto costumeCharId = costumeMaster ? Text(costumeMaster, "get_CharacterId") : "";
            const auto hairCharId = hairMaster ? Text(hairMaster, "get_CharacterId") : "";
            std::string roomCharId;
            {
                std::lock_guard lock(costumeRoomMutex);
                roomCharId = activeCostumeRoomCharacterId;
            }
            std::string characterId;
            if (!costumeCharId.empty() && costumeCharId == hairCharId && owned.actors.contains(costumeCharId)) {
                characterId = costumeCharId;
            } else if (!roomCharId.empty() && owned.actors.contains(roomCharId)) {
                characterId = roomCharId;
            } else if (!hairCharId.empty() && owned.actors.contains(hairCharId)) {
                characterId = hairCharId;
            } else if (!costumeCharId.empty() && owned.actors.contains(costumeCharId)) {
                characterId = costumeCharId;
            } else if (!owned.actors.empty()) {
                characterId = owned.actors.begin()->first;
            }
            std::string chosenCostume, chosenHair;
            if (!FindOwnedOutfitPair(owned, characterId, reqCostume, reqHair, nullptr, chosenCostume, chosenHair)) {
                Log::ErrorFmt("PhotoRequests: %s unchanged, no owned outfit for %s", name.c_str(), characterId.c_str());
                return PreparedRequest(request);
            }
            if (chosenCostume == reqCostume && chosenHair == reqHair) {
                if (Config::dbgMode) {
                    Log::InfoFmt("PhotoRequests: %s forwarded original (costume already owned)", name.c_str());
                }
                return PreparedRequest(request);
            }
            const auto originalFingerprint = RequestFingerprint(request);
            auto candidate = CloneRequest(request);
            auto clone = candidate.get();
            if (clone == request) return PreparedRequest(request);
            if (!SetText(clone, "set_CostumeId", chosenCostume) || !SetText(clone, "set_HairId", chosenHair) ||
                RequestFingerprint(request) != originalFingerprint) {
                Log::ErrorFmt("PhotoRequests: %s failed to write owned outfit on clone", name.c_str());
                return PreparedRequest(request);
            }
            if (Config::dbgMode) {
                Log::InfoFmt("PhotoRequests: %s prepared fast on clone (%s,%s) -> (%s,%s)",
                             name.c_str(), reqCostume.c_str(), reqHair.c_str(),
                             chosenCostume.c_str(), chosenHair.c_str());
            }
            return candidate;
        }
        if (name == "CostumeCheckBulkRequest") {
            const auto origCostumes = Strings(Get(request, "get_CostumeIds"));
            const auto origHairs = Strings(Get(request, "get_HairIds"));
            std::vector<std::string> filteredCostumes, filteredHairs;
            std::set<std::string> seenCostumes, seenHairs;
            for (const auto& id : origCostumes) {
                if (!owned.costumes.contains(id) || !seenCostumes.insert(id).second) continue;
                auto master = Find("get_CostumeMaster", id);
                if (CostumeAllowed(master, nullptr, Text(master, "get_CharacterId"))) {
                    filteredCostumes.push_back(id);
                }
            }
            for (const auto& id : origHairs) {
                if (!owned.hairs.contains(id) || !seenHairs.insert(id).second) continue;
                auto master = Find("get_HairMaster", id);
                if (master && !Field<bool>(master, "isDisableAdmin_")) {
                    filteredHairs.push_back(id);
                }
            }
            if (filteredCostumes == origCostumes && filteredHairs == origHairs) {
                if (Config::dbgMode) {
                    Log::InfoFmt("PhotoRequests: %s forwarded original (bulk ids already owned)", name.c_str());
                }
                return PreparedRequest(request);
            }
            if (filteredCostumes.empty() && filteredHairs.empty() && !owned.costumes.empty()) {
                for (const auto& id : owned.costumes) {
                    auto master = Find("get_CostumeMaster", id);
                    if (CostumeAllowed(master, nullptr, Text(master, "get_CharacterId"))) {
                        filteredCostumes.push_back(id);
                        break;
                    }
                }
            }
            const auto originalFingerprint = RequestFingerprint(request);
            auto candidate = CloneRequest(request);
            auto clone = candidate.get();
            if (clone == request) return PreparedRequest(request);
            if (!Replace(Get(clone, "get_CostumeIds"), filteredCostumes) ||
                !Replace(Get(clone, "get_HairIds"), filteredHairs) ||
                RequestFingerprint(request) != originalFingerprint) {
                Log::ErrorFmt("PhotoRequests: %s failed to write owned bulk ids on clone", name.c_str());
                return PreparedRequest(request);
            }
            if (Config::dbgMode) {
                Log::InfoFmt("PhotoRequests: %s prepared fast on clone costumes=%zu->%zu hairs=%zu->%zu",
                             name.c_str(), origCostumes.size(), filteredCostumes.size(),
                             origHairs.size(), filteredHairs.size());
            }
            return candidate;
        }
        return PreparedRequest(request);
    }

    PreparedRequest PrepareExpressionPoseRequest(void* request, const Inventory& owned) {
        auto expression = Find("get_PhotoExpressionMaster", Text(request, "get_PhotoExpressionId"));
        if (!expression) {
            Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, expression master missing");
            return PreparedRequest(request);
        }

        auto parameters = Items(Get(request, "get_CreateExpressionShootingParams"));
        std::vector<int> paramPoseTypes;
        paramPoseTypes.reserve(parameters.size());
        std::map<std::string, std::set<int>> requiredTypesByActor;
        for (auto param : parameters) {
            int allowedType = 0;
            if (!ResolveExpressionStagePoseType(expression, Get<int>(param, "get_StagePositionNumber"), allowedType)) {
                Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, unknown stage position");
                return PreparedRequest(request);
            }
            paramPoseTypes.push_back(allowedType);
            for (const auto& cid : Strings(Get(param, "get_CharacterIds"))) {
                if (!cid.empty()) requiredTypesByActor[cid].insert(allowedType);
            }
            const auto mainCid = Text(param, "get_MainCharacterId");
            if (!mainCid.empty()) requiredTypesByActor[mainCid].insert(allowedType);
        }

        std::map<std::string, ActorSelection> actors;
        bool actorsChanged = false;
        const auto resolveActor = [&](const std::string& cid, const std::string& reqCostume,
                                      const std::string& reqHair, ActorSelection& out) -> bool {
            if (const auto it = actors.find(cid); it != actors.end()) {
                out = it->second;
                if (out.costume != reqCostume || out.hair != reqHair) actorsChanged = true;
                return true;
            }
            const auto reqTypesIt = requiredTypesByActor.find(cid);
            const std::set<int> emptyTypes;
            const auto& reqTypes = reqTypesIt != requiredTypesByActor.end() ? reqTypesIt->second : emptyTypes;

            if (IsActorOutfitValid(owned, cid, reqCostume, reqHair, expression, nullptr) &&
                OutfitHasOwnedPosesForTypes(owned, expression, cid, reqCostume, reqHair, reqTypes)) {
                out = {cid, reqCostume, reqHair};
                actors[cid] = out;
                return true;
            }

            std::string chosenCostume, chosenHair;
            if (FindOwnedOutfitPair(owned, cid, reqCostume, reqHair, expression, chosenCostume, chosenHair,
                                    nullptr, reqTypes, /*requirePoseTypes=*/true, expression)) {
                out = {cid, chosenCostume, chosenHair};
                actors[cid] = out;
                if (chosenCostume != reqCostume || chosenHair != reqHair) actorsChanged = true;
                return true;
            }
            if (auto forceActor = BuildForceActorCostume(cid, expression)) {
                const auto forceCostume = Text(forceActor, "get_CostumeId");
                const auto forceHair = Text(forceActor, "get_HairId");
                if (!forceCostume.empty() && !forceHair.empty() &&
                    OutfitHasOwnedPosesForTypes(owned, expression, cid, forceCostume, forceHair, reqTypes)) {
                    out = {cid, forceCostume, forceHair};
                    actors[cid] = out;
                    if (forceCostume != reqCostume || forceHair != reqHair) actorsChanged = true;
                    return true;
                }
            }
            return false;
        };

        struct PlannedRoster {
            const char* cosGetter;
            const char* hairGetter;
            std::vector<std::string> costumes;
            std::vector<std::string> hairs;
        };
        std::vector<PlannedRoster> plannedRosters;
        const auto planRoster = [&](const char* cGetter, const char* cosGetter, const char* hGetter) -> bool {
            if (!MethodOf(request, cGetter)) return true;
            const auto chars = Strings(Get(request, cGetter));
            const auto costumes = Strings(Get(request, cosGetter));
            const auto hairs = Strings(Get(request, hGetter));
            if (chars.empty() && costumes.empty() && hairs.empty()) return true;
            if (!Aligned(chars, costumes, hairs)) return false;
            PlannedRoster roster{cosGetter, hGetter, costumes, hairs};
            for (size_t i = 0; i < chars.size(); ++i) {
                ActorSelection selected;
                if (!resolveActor(chars[i], costumes[i], hairs[i], selected)) return false;
                roster.costumes[i] = selected.costume;
                roster.hairs[i] = selected.hair;
            }
            plannedRosters.push_back(std::move(roster));
            return true;
        };

        if (!planRoster("get_CharacterIds", "get_CostumeIds", "get_HairIds") ||
            !planRoster("get_SelectedCharacterIds", "get_SelectedCostumeIds", "get_SelectedHairIds")) {
            Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, no pose-compatible outfit");
            return PreparedRequest(request);
        }
        bool hasSingleActor = false;
        ActorSelection singleActor;
        if (!Text(request, "get_CharacterId").empty()) {
            hasSingleActor = true;
            if (!resolveActor(Text(request, "get_CharacterId"), Text(request, "get_CostumeId"),
                              Text(request, "get_HairId"), singleActor)) {
                Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, no pose-compatible single outfit");
                return PreparedRequest(request);
            }
        }
        if (actors.empty()) {
            Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, empty actor roster");
            return PreparedRequest(request);
        }

        struct PlannedPoseParam {
            std::vector<std::string> poses;
            bool hasMain{};
            std::string mainPose;
        };
        std::vector<PlannedPoseParam> plannedPoses;
        plannedPoses.reserve(parameters.size());
        bool posesChanged = false;

        for (size_t idx = 0; idx < parameters.size(); ++idx) {
            auto param = parameters[idx];
            const int allowedType = paramPoseTypes[idx];
            const auto ids = Strings(Get(param, "get_CharacterIds"));
            const auto poses = Strings(Get(param, "get_PhotoPoseIds"));
            if (ids.size() != poses.size()) {
                Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, pose list mismatch");
                return PreparedRequest(request);
            }
            PlannedPoseParam entry;
            entry.poses.reserve(poses.size());
            for (size_t i = 0; i < ids.size(); ++i) {
                const auto it = actors.find(ids[i]);
                if (it == actors.end()) {
                    Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, unknown actor in param");
                    return PreparedRequest(request);
                }
                const auto resolvedPose = FindOwnedCompatiblePose(
                    owned, expression, poses[i], it->second.character, it->second.costume, it->second.hair, allowedType);
                if (resolvedPose.empty()) {
                    Log::ErrorFmt("PhotoRequests: PhotoCreateExpressionShootingsRequest unchanged, no owned pose for %s type=%d",
                                  ids[i].c_str(), allowedType);
                    return PreparedRequest(request);
                }
                if (resolvedPose != poses[i]) posesChanged = true;
                entry.poses.push_back(resolvedPose);
            }
            const auto main = Text(param, "get_MainCharacterId");
            if (!main.empty()) {
                const auto it = actors.find(main);
                if (it == actors.end()) return PreparedRequest(request);
                const auto reqMainPose = Text(param, "get_PhotoPoseId");
                const auto resolvedMainPose = FindOwnedCompatiblePose(
                    owned, expression, reqMainPose, it->second.character, it->second.costume, it->second.hair, allowedType);
                if (resolvedMainPose.empty()) return PreparedRequest(request);
                if (resolvedMainPose != reqMainPose) posesChanged = true;
                entry.hasMain = true;
                entry.mainPose = resolvedMainPose;
            }
            plannedPoses.push_back(std::move(entry));
        }

        if (!actorsChanged && !posesChanged) {
            if (Config::dbgMode) {
                Log::Info("PhotoRequests: PhotoCreateExpressionShootingsRequest forwarded original (expression outfits & poses valid)");
            }
            return PreparedRequest(request);
        }

        const auto originalFingerprint = RequestFingerprint(request);
        auto candidate = CloneRequest(request);
        auto clone = candidate.get();
        if (clone == request) return PreparedRequest(request);

        if (actorsChanged) {
            for (const auto& roster : plannedRosters) {
                if (!Replace(Get(clone, roster.cosGetter), roster.costumes) ||
                    !Replace(Get(clone, roster.hairGetter), roster.hairs)) {
                    Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest failed to write outfits on clone");
                    return PreparedRequest(request);
                }
            }
            if (hasSingleActor &&
                (!SetText(clone, "set_CostumeId", singleActor.costume) ||
                 !SetText(clone, "set_HairId", singleActor.hair))) {
                Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest failed to write single outfit on clone");
                return PreparedRequest(request);
            }
        }
        if (posesChanged) {
            auto cloneParams = Items(Get(clone, "get_CreateExpressionShootingParams"));
            if (cloneParams.size() != plannedPoses.size()) return PreparedRequest(request);
            for (size_t i = 0; i < cloneParams.size(); ++i) {
                if (!Replace(Get(cloneParams[i], "get_PhotoPoseIds"), plannedPoses[i].poses)) return PreparedRequest(request);
                if (plannedPoses[i].hasMain && !SetText(cloneParams[i], "set_PhotoPoseId", plannedPoses[i].mainPose)) {
                    return PreparedRequest(request);
                }
            }
        }
        if (RequestFingerprint(request) != originalFingerprint) {
            Log::Error("PhotoRequests: PhotoCreateExpressionShootingsRequest clone integrity check failed");
            return PreparedRequest(request);
        }
        if (Config::dbgMode) {
            Log::InfoFmt("PhotoRequests: PhotoCreateExpressionShootingsRequest prepared fast on clone actorsChanged=%d posesChanged=%d params=%zu bytes=%d",
                         actorsChanged ? 1 : 0, posesChanged ? 1 : 0, plannedPoses.size(), Get<int>(clone, "CalculateSize"));
        }
        return candidate;
    }

    PreparedRequest PreparePhotoActorsFallback(void* request, const std::string& name, const Inventory& owned) {
        const bool isExpression = name == "PhotoCheckExpressionShootingRequest";
        void* specifier = nullptr;
        SceneRules specialRules;
        bool hasSpecialRules = false;
        if (!ResolveRequestRules(request, name, specifier, specialRules, hasSpecialRules)) {
            Log::ErrorFmt("PhotoRequests: %s unchanged, scene rules unavailable", name.c_str());
            return PreparedRequest(request);
        }
        const SceneRules* specialPtr = hasSpecialRules ? &specialRules : nullptr;
        const auto preferredPoseTypes = isExpression ? CollectExpressionPoseTypes(specifier) : std::set<int>{};

        bool changed = false;
        const auto resolveActor = [&](const std::string& cid, const std::string& reqCostume,
                                      const std::string& reqHair, std::string& outCostume, std::string& outHair) -> bool {
            if (IsActorOutfitValid(owned, cid, reqCostume, reqHair, specifier, specialPtr)) {
                outCostume = reqCostume;
                outHair = reqHair;
                return true;
            }
            if (FindOwnedOutfitPair(owned, cid, reqCostume, reqHair, specifier, outCostume, outHair,
                                    specialPtr, preferredPoseTypes, /*requirePoseTypes=*/false)) {
                changed = true;
                return true;
            }
            if (specifier) {
                if (auto forceActor = BuildForceActorCostume(cid, specifier)) {
                    outCostume = Text(forceActor, "get_CostumeId");
                    outHair = Text(forceActor, "get_HairId");
                    if (!outCostume.empty() && !outHair.empty()) {
                        changed = true;
                        return true;
                    }
                }
            }
            return false;
        };

        struct PlannedRoster {
            const char* cosGetter;
            const char* hairGetter;
            std::vector<std::string> costumes;
            std::vector<std::string> hairs;
        };
        std::vector<PlannedRoster> plannedRosters;
        const auto planRoster = [&](const char* cGetter, const char* cosGetter, const char* hGetter) -> bool {
            if (!MethodOf(request, cGetter)) return true;
            const auto chars = Strings(Get(request, cGetter));
            const auto costumes = Strings(Get(request, cosGetter));
            const auto hairs = Strings(Get(request, hGetter));
            if (chars.empty() && costumes.empty() && hairs.empty()) return true;
            if (!Aligned(chars, costumes, hairs)) return false;
            PlannedRoster roster{cosGetter, hGetter, costumes, hairs};
            for (size_t i = 0; i < chars.size(); ++i) {
                if (!resolveActor(chars[i], costumes[i], hairs[i], roster.costumes[i], roster.hairs[i])) return false;
            }
            plannedRosters.push_back(std::move(roster));
            return true;
        };

        if (!planRoster("get_CharacterIds", "get_CostumeIds", "get_HairIds") ||
            !planRoster("get_SelectedCharacterIds", "get_SelectedCostumeIds", "get_SelectedHairIds")) {
            Log::ErrorFmt("PhotoRequests: %s unchanged, fallback outfit resolution failed", name.c_str());
            return PreparedRequest(request);
        }
        bool hasSingleActor = false;
        std::string singleCostume, singleHair;
        if (isExpression && !Text(request, "get_CharacterId").empty()) {
            hasSingleActor = true;
            if (!resolveActor(Text(request, "get_CharacterId"), Text(request, "get_CostumeId"),
                              Text(request, "get_HairId"), singleCostume, singleHair)) {
                Log::ErrorFmt("PhotoRequests: %s unchanged, fallback single outfit resolution failed", name.c_str());
                return PreparedRequest(request);
            }
        }
        if (!changed) {
            return PreparedRequest(request);
        }

        const auto originalFingerprint = RequestFingerprint(request);
        auto candidate = CloneRequest(request);
        auto clone = candidate.get();
        if (clone == request) return PreparedRequest(request);
        for (const auto& roster : plannedRosters) {
            if (!Replace(Get(clone, roster.cosGetter), roster.costumes) ||
                !Replace(Get(clone, roster.hairGetter), roster.hairs)) {
                Log::ErrorFmt("PhotoRequests: %s fallback failed to write outfits on clone", name.c_str());
                return PreparedRequest(request);
            }
        }
        if (hasSingleActor &&
            (!SetText(clone, "set_CostumeId", singleCostume) || !SetText(clone, "set_HairId", singleHair))) {
            Log::ErrorFmt("PhotoRequests: %s fallback failed to write single outfit on clone", name.c_str());
            return PreparedRequest(request);
        }
        if (RequestFingerprint(request) != originalFingerprint) {
            Log::ErrorFmt("PhotoRequests: %s fallback clone integrity check failed", name.c_str());
            return PreparedRequest(request);
        }
        if (Config::dbgMode) {
            Log::InfoFmt("PhotoRequests: %s prepared fallback outfits on clone bytes=%d",
                         name.c_str(), Get<int>(clone, "CalculateSize"));
        }
        return candidate;
    }

    void* ResolveActivityStartSpecifier(void* request, const std::string& name,
                                        std::string& outKind, std::string& outSceneId) {
        const auto info = [](const char* getter) -> void* {
            auto method = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.User", "UserDataManager", getter);
            return method ? reinterpret_cast<void* (*)(void*)>(method->function)(method->address) : nullptr;
        };
        if (name == "ActivityStartPromotionRequest") {
            outKind = "Promotion";
            outSceneId = Text(request, "get_ActivityPromotionId");
            for (auto item : Items(Get(info("get_PromotionInfo"), "get_Promotions"))) {
                if (Text(item, "get_Id") == outSceneId) return item;
            }
            return nullptr;
        }
        if (name == "ActivityStartFanEventRequest") {
            outKind = "FanEvent";
            outSceneId = Text(request, "get_ActivityFanEventId");
            for (auto item : Items(Get(info("get_FanEventInfo"), "get_FanEvents"))) {
                if (Text(item, "get_Id") == outSceneId) return item;
            }
            return nullptr;
        }
        if (name == "ActivityStartRefreshRequest") {
            outKind = "Refresh";
            const auto refreshId = Text(request, "get_ActivityRefreshId");
            const int level = Get<int>(request, "get_ActivityRefreshLevel");
            outSceneId = refreshId + "/" + std::to_string(level);
            for (auto refresh : Items(Get(info("get_RefreshInfo"), "get_Refreshes"))) {
                if (Text(refresh, "get_Id") != refreshId) continue;
                for (auto row : Items(Get(refresh, "get_Levels"))) {
                    if (Get<int>(row, "get_Level") == level) return row;
                }
            }
            return nullptr;
        }
        return nullptr;
    }

    bool IsDeckRequestName(const std::string& name) {
        return name == "DeckSaveRequest" ||
               name == "LeagueSaveDeckRequest" ||
               name == "DiceSaveDeckRequest" ||
               name == "RaceSaveDeckRequest" ||
               name == "MarathonDeckSaveRequest" ||
               name == "MarathonRaidDeckSaveRequest" ||
               name == "MarathonBuddyDeckRequest" ||
               name == "MarathonListDeckRequest" ||
               name == "MarathonQuestStartRequest" ||
               name == "MarathonQuestListAssetRequest" ||
               name == "QuestStartRequest" ||
               name == "QuestListAssetRequest" ||
               name == "DreamStartQuestRequest";
    }

    PreparedRequest PrepareDeckRequest(void* request, const std::string& name, const Inventory& owned) {
        std::string deckKey;
        const bool isSaveDeck = name == "DeckSaveRequest" ||
                                name == "LeagueSaveDeckRequest" ||
                                name == "DiceSaveDeckRequest" ||
                                name == "RaceSaveDeckRequest" ||
                                name == "MarathonDeckSaveRequest" ||
                                name == "MarathonRaidDeckSaveRequest";
        if (name == "DeckSaveRequest") {
            deckKey = "Deck/" + std::to_string(Get<int>(request, "get_Number"));
        } else if (name == "LeagueSaveDeckRequest") {
            deckKey = "League/" + Text(request, "get_SeasonId") + "/" + std::to_string(Get<int>(request, "get_DeckType"));
        } else if (name == "DiceSaveDeckRequest") {
            deckKey = "Dice/" + std::to_string(Get<int>(request, "get_Number"));
        } else if (name == "RaceSaveDeckRequest") {
            deckKey = "Race/" + std::to_string(Get<int>(request, "get_Number"));
        } else if (name == "MarathonDeckSaveRequest" || name == "MarathonRaidDeckSaveRequest") {
            deckKey = "Marathon/" + std::to_string(Get<int>(request, "get_Number"));
        } else if (name == "QuestStartRequest" || name == "QuestListAssetRequest" || name == "DreamStartQuestRequest") {
            deckKey = "Deck/" + std::to_string(Get<int>(request, "get_DeckNumber"));
        } else if (name == "MarathonQuestStartRequest" || name == "MarathonQuestListAssetRequest") {
            deckKey = "Marathon/" + std::to_string(Get<int>(request, "get_DeckNumber"));
        }

        if (!deckKey.empty()) {
            std::lock_guard lock(deckVisualMutex);
            activeLiveDeckKey = deckKey;
        }

        struct PositionListRef {
            const char* listGetter;
            int buddyDeckIndex; // -1 for top-level list
        };
        struct PlannedPositionFix {
            size_t listRefIndex;
            size_t itemIndex;
            std::string costumeId;
            std::string hairId;
        };

        std::vector<PositionListRef> listRefs;
        std::vector<std::vector<void*>> rawLists;
        const auto addTopList = [&](const char* getter) {
            if (!MethodOf(request, getter)) return;
            auto list = Get(request, getter);
            if (!list) return;
            listRefs.push_back({getter, -1});
            rawLists.push_back(Items(list));
        };
        addTopList("get_UserDeckPositions");
        addTopList("get_DeckPositions");
        addTopList("get_UserBuddyDeckPositions");
        if (MethodOf(request, "get_MarathonBuddyDecks")) {
            auto decks = Items(Get(request, "get_MarathonBuddyDecks"));
            for (size_t di = 0; di < decks.size(); ++di) {
                if (!decks[di]) continue;
                auto list = Get(decks[di], "get_UserBuddyDeckPositions");
                if (!list) continue;
                listRefs.push_back({"get_UserBuddyDeckPositions", static_cast<int>(di)});
                rawLists.push_back(Items(list));
            }
        }

        std::vector<PlannedPositionFix> fixes;
        std::map<int, DeckVisualSlot> savedSlots;
        std::set<int> clearedPositions;

        for (size_t li = 0; li < rawLists.size(); ++li) {
            const auto& positions = rawLists[li];
            for (size_t pi = 0; pi < positions.size(); ++pi) {
                void* pos = positions[pi];
                if (!pos) continue;
                if (!Text(pos, "get_BuddyUserId").empty()) continue;

                const int positionNum = Get<int>(pos, "get_Position");
                const auto cardId = Text(pos, "get_CardId");
                const auto costumeId = Text(pos, "get_CostumeId");
                const auto hairId = Text(pos, "get_HairId");

                if (cardId.empty() || (costumeId.empty() && hairId.empty())) {
                    if (isSaveDeck && !deckKey.empty()) clearedPositions.insert(positionNum);
                    continue;
                }

                auto cardMaster = Find("get_CardMaster", cardId);
                const auto characterId = Text(cardMaster, "get_CharacterId");
                if (characterId.empty() || !owned.actors.contains(characterId)) continue;

                bool alreadyValid = false;
                if (!costumeId.empty() && !hairId.empty()) {
                    alreadyValid = IsActorOutfitValid(owned, characterId, costumeId, hairId, nullptr, nullptr);
                } else if (!costumeId.empty()) {
                    alreadyValid = owned.costumes.contains(costumeId) &&
                                   CostumeAllowed(Find("get_CostumeMaster", costumeId), nullptr, characterId);
                } else {
                    const auto& liveCos = owned.actors.at(characterId).liveCostume;
                    alreadyValid = owned.hairs.contains(hairId) &&
                                   HairAllowed(Find("get_HairMaster", hairId), characterId, liveCos);
                }

                if (alreadyValid) {
                    if (isSaveDeck && !deckKey.empty()) clearedPositions.insert(positionNum);
                    continue;
                }

                std::string chosenCostume, chosenHair;
                if (!FindOwnedOutfitPair(owned, characterId, costumeId, hairId, nullptr, chosenCostume, chosenHair)) {
                    Log::ErrorFmt("PhotoRequests: %s unchanged, no owned outfit for deck card=%s char=%s",
                                  name.c_str(), cardId.c_str(), characterId.c_str());
                    return PreparedRequest(request);
                }

                if (isSaveDeck && !deckKey.empty() && (!costumeId.empty() || !hairId.empty())) {
                    savedSlots[positionNum] = {cardId, characterId, costumeId, hairId};
                }
                fixes.push_back({li, pi, costumeId.empty() ? "" : chosenCostume, hairId.empty() ? "" : chosenHair});
            }
        }

        if (isSaveDeck && !deckKey.empty()) {
            std::lock_guard lock(deckVisualMutex);
            LoadDeckVisualsLocked();
            auto itDeck = deckVisuals.find(deckKey);
            bool dirty = false;
            if (itDeck != deckVisuals.end()) {
                for (auto it = itDeck->second.begin(); it != itDeck->second.end(); ) {
                    if (!savedSlots.contains(it->first)) {
                        it = itDeck->second.erase(it);
                        dirty = true;
                    } else {
                        ++it;
                    }
                }
            }
            if (!savedSlots.empty()) {
                auto& deckMap = deckVisuals[deckKey];
                for (const auto& [pos, slot] : savedSlots) {
                    const auto it = deckMap.find(pos);
                    if (it == deckMap.end() || it->second.cardId != slot.cardId ||
                        it->second.costumeId != slot.costumeId || it->second.hairId != slot.hairId) {
                        deckMap[pos] = slot;
                        dirty = true;
                    }
                }
            } else if (itDeck != deckVisuals.end() && itDeck->second.empty()) {
                deckVisuals.erase(itDeck);
            }
            if (dirty) SaveDeckVisualsLocked();
        }

        if (fixes.empty()) {
            if (Config::dbgMode) {
                Log::InfoFmt("PhotoRequests: %s forwarded original (deck outfits owned)", name.c_str());
            }
            return PreparedRequest(request);
        }

        auto candidate = CloneRequest(request);
        auto clone = candidate.get();
        if (clone == request) return PreparedRequest(request);

        std::vector<void*> cloneBuddyDecks;
        if (MethodOf(clone, "get_MarathonBuddyDecks")) {
            cloneBuddyDecks = Items(Get(clone, "get_MarathonBuddyDecks"));
        }

        for (const auto& fix : fixes) {
            const auto& ref = listRefs[fix.listRefIndex];
            void* targetList = nullptr;
            if (ref.buddyDeckIndex < 0) {
                targetList = Get(clone, ref.listGetter);
            } else if (static_cast<size_t>(ref.buddyDeckIndex) < cloneBuddyDecks.size()) {
                targetList = Get(cloneBuddyDecks[ref.buddyDeckIndex], ref.listGetter);
            }
            auto cloneItems = Items(targetList);
            if (fix.itemIndex >= cloneItems.size() || !cloneItems[fix.itemIndex]) {
                Log::ErrorFmt("PhotoRequests: %s failed to locate cloned deck position", name.c_str());
                return PreparedRequest(request);
            }
            void* clonePos = cloneItems[fix.itemIndex];
            if ((!fix.costumeId.empty() && !SetText(clonePos, "set_CostumeId", fix.costumeId)) ||
                (!fix.hairId.empty() && !SetText(clonePos, "set_HairId", fix.hairId))) {
                Log::ErrorFmt("PhotoRequests: %s failed to write owned outfit on cloned deck position", name.c_str());
                return PreparedRequest(request);
            }
        }

        if (Config::dbgMode) {
            Log::InfoFmt("PhotoRequests: %s prepared fast on clone deck=%s sanitized=%zu savedVisual=%zu bytes=%d",
                         name.c_str(), deckKey.c_str(), fixes.size(), savedSlots.size(), Get<int>(clone, "CalculateSize"));
        }
        return candidate;
    }

    PreparedRequest PrepareActivityStart(void* request, const std::string& name, const Inventory& owned) {
        std::string kind, sceneId;
        void* specifier = ResolveActivityStartSpecifier(request, name, kind, sceneId);
        if (!specifier || sceneId.empty()) {
            Log::ErrorFmt("PhotoRequests: %s unchanged, activity specifier %s/%s not found",
                          name.c_str(), kind.c_str(), sceneId.c_str());
            return PreparedRequest(request);
        }
        const auto chars = Strings(Get(request, "get_CharacterIds"));
        const auto costumes = Strings(Get(request, "get_CostumeIds"));
        const auto hairs = Strings(Get(request, "get_HairIds"));
        if (chars.empty() || !Aligned(chars, costumes, hairs)) {
            Log::ErrorFmt("PhotoRequests: %s unchanged, invalid actor lists", name.c_str());
            return PreparedRequest(request);
        }

        auto outCostumes = costumes;
        auto outHairs = hairs;
        ActivityVisualRecord visualRecord;
        visualRecord.sceneId = sceneId;
        bool changed = false;

        for (size_t i = 0; i < chars.size(); ++i) {
            const auto& cid = chars[i];
            const auto& selCostume = costumes[i];
            const auto& selHair = hairs[i];
            auto selCostumeMaster = Find("get_CostumeMaster", selCostume);
            const bool matchesType = selCostumeMaster && validateCostume &&
                                     validateCostume(specifier, selCostumeMaster, nullptr);
            const bool hasSdAsset = selCostumeMaster && !Text(selCostumeMaster, "get_SdAssetId").empty();
            if (!cid.empty() && !selCostume.empty() && !selHair.empty() && hasSdAsset) {
                visualRecord.outfits[cid] = {selCostume, selHair};
            }

            std::string chosenCostume, chosenHair;
            if (matchesType) {
                if (owned.costumes.contains(selCostume) && owned.hairs.contains(selHair) &&
                    CostumeAllowed(selCostumeMaster, specifier, cid) &&
                    HairAllowed(Find("get_HairMaster", selHair), cid, selCostume)) {
                    chosenCostume = selCostume;
                    chosenHair = selHair;
                } else if (!FindOwnedOutfitPair(owned, cid, selCostume, selHair, specifier, chosenCostume, chosenHair)) {
                    if (auto forceActor = BuildForceActorCostume(cid, specifier)) {
                        chosenCostume = Text(forceActor, "get_CostumeId");
                        chosenHair = Text(forceActor, "get_HairId");
                    }
                }
            } else {
                // Non-matching costume on restricted schedule: send the schedule's rental outfit
                // to the server while keeping the user's selected outfit for SD display (if SD asset exists).
                if (auto forceActor = BuildForceActorCostume(cid, specifier)) {
                    chosenCostume = Text(forceActor, "get_CostumeId");
                    chosenHair = Text(forceActor, "get_HairId");
                }
                if (chosenCostume.empty() || chosenHair.empty()) {
                    FindOwnedOutfitPair(owned, cid, selCostume, selHair, specifier, chosenCostume, chosenHair);
                }
            }

            if (chosenCostume.empty() || chosenHair.empty()) {
                Log::ErrorFmt("PhotoRequests: %s unchanged, no valid outfit for %s in %s/%s",
                              name.c_str(), cid.c_str(), kind.c_str(), sceneId.c_str());
                return PreparedRequest(request);
            }
            if (chosenCostume != selCostume || chosenHair != selHair) {
                outCostumes[i] = chosenCostume;
                outHairs[i] = chosenHair;
                changed = true;
            }
        }

        const size_t visualCount = visualRecord.outfits.size();
        {
            std::lock_guard lock(activityVisualMutex);
            LoadActivityVisualsLocked();
            if (visualRecord.outfits.empty()) {
                activityVisuals.erase(kind);
            } else {
                activityVisuals[kind] = std::move(visualRecord);
            }
            SaveActivityVisualsLocked();
        }
        if (Config::dbgMode) {
            Log::InfoFmt("ActivityVisual: recorded %s/%s matching=%zu/%zu",
                         kind.c_str(), sceneId.c_str(), visualCount, chars.size());
        }

        if (!changed) {
            if (Config::dbgMode) {
                Log::InfoFmt("PhotoRequests: %s forwarded original (activity entry valid)", name.c_str());
            }
            return PreparedRequest(request);
        }

        const auto originalFingerprint = RequestFingerprint(request);
        auto candidate = CloneRequest(request);
        auto clone = candidate.get();
        if (clone == request) return PreparedRequest(request);
        if (!Replace(Get(clone, "get_CostumeIds"), outCostumes) ||
            !Replace(Get(clone, "get_HairIds"), outHairs)) {
            Log::ErrorFmt("PhotoRequests: %s failed to write activity outfits on clone", name.c_str());
            return PreparedRequest(request);
        }
        if (RequestFingerprint(request) != originalFingerprint ||
            ActivityIdentity(clone) != ActivityIdentity(request)) {
            Log::ErrorFmt("PhotoRequests: %s activity clone integrity check failed", name.c_str());
            return PreparedRequest(request);
        }
        if (Config::dbgMode) {
            Log::InfoFmt("PhotoRequests: %s prepared fast on clone, actors=%zu visual-overrides=%zu bytes=%d",
                         name.c_str(), chars.size(), visualCount, Get<int>(clone, "CalculateSize"));
            Log::DebugFmt("PhotoRequests: prepared activity=%s", Text(clone, "ToString").c_str());
        }
        return candidate;
    }
}

    void CaptureInitial(void* response) {
        if (!response || std::string(Il2cppUtils::get_class_from_instance(response)->name) != "UserGetResponse") return;
        if (auto userPublic = Get(response, "get_UserPublic")) {
            const auto uid = Text(userPublic, "get_PublicUserId");
            if (!uid.empty()) {
                std::lock_guard lock(deckVisualMutex);
                selfPublicUserId = uid;
            }
        }
        Inventory fresh;
        if (!ReadInventory(response, fresh, true)) return;
        std::lock_guard lock(inventoryMutex);
        inventory = std::move(fresh);
        if (Config::dbgMode) {
            Log::InfoFmt("PhotoRequests: server inventory ready=%d actors=%zu costumes=%zu hairs=%zu poses=%zu",
                         inventory.ready, inventory.actors.size(), inventory.costumes.size(), inventory.hairs.size(), inventory.poses.size());
        }
    }

    void CaptureUpdate(void* data) {
        if (!data || std::string(Il2cppUtils::get_class_from_instance(data)->name) != "UpdatedData") return;
        std::lock_guard lock(inventoryMutex);
        if (inventory.ready) {
            ReadInventory(data, inventory, false);
        }
    }

    void CaptureAvailability(void* response) {
        if (!response) return;
        const std::string name = Il2cppUtils::get_class_from_instance(response)->name;
        if (name != "PhotoListSpecialShootingResponse") return;
        SceneCatalog additional;
        for (auto item : Items(Get(response, "get_Activities"))) {
            const auto id = std::to_string(Get<int>(item, "get_ActionType"));
            if (id.empty()) continue;
            additional[SceneKey("SpecialPhotoActivity", id)] = ReadSpecialActivityRules(item, id);
        }
        if (!additional.empty()) {
            std::lock_guard lock(specialSceneMutex);
            for (auto& [key, scene] : additional) specialScenes[key] = std::move(scene);
        }
    }

    void ApplyActivityVisualOutfits(void* progress) {
        if (!progress || !Config::unlockAllLiveCostume) return;
        auto klass = Il2cppUtils::get_class_from_instance(progress);
        if (!klass || !klass->name) return;
        const std::string className = klass->name;
        std::string kind, sceneId;
        if (className == "PromotionProgress") {
            kind = "Promotion";
            sceneId = Text(progress, "get_ActivityPromotionId");
        } else if (className == "RefreshProgress") {
            kind = "Refresh";
            sceneId = Text(progress, "get_ActivityRefreshId") + "/" +
                      std::to_string(Get<int>(progress, "get_ActivityRefreshLevel"));
        } else if (className == "FanEventProgress") {
            kind = "FanEvent";
            sceneId = Text(progress, "get_ActivityFanEventId");
        } else {
            return;
        }
        if (sceneId.empty()) return;

        ActivityVisualRecord saved;
        {
            std::lock_guard lock(activityVisualMutex);
            LoadActivityVisualsLocked();
            const auto it = activityVisuals.find(kind);
            if (it == activityVisuals.end() || it->second.sceneId != sceneId) return;
            saved = it->second;
        }

        const auto characters = Items(Get(progress, "get_Characters"));
        int applied = 0;
        for (auto info : characters) {
            const auto cid = Text(info, "get_CharacterId");
            const auto found = saved.outfits.find(cid);
            if (found == saved.outfits.end()) continue;
            const auto& [costumeId, hairId] = found->second;
            if (costumeId.empty() || hairId.empty()) continue;
            const auto currentCostume = Text(info, "get_CostumeId");
            const auto currentHair = Text(info, "get_HairId");
            if (currentCostume != costumeId || currentHair != hairId) {
                if (SetText(info, "set_CostumeId", costumeId) && SetText(info, "set_HairId", hairId)) {
                    ++applied;
                }
            }
        }
        if (applied > 0 && Config::dbgMode) {
            Log::InfoFmt("ActivityVisual: applied %s/%s overrides=%d/%zu",
                         kind.c_str(), sceneId.c_str(), applied, characters.size());
        }
    }

    void ApplyDeckVisualOutfit(void* deckPosition) {
        if (!deckPosition || !Config::unlockAllLiveCostume) return;
        auto klass = Il2cppUtils::get_class_from_instance(deckPosition);
        if (!klass || !klass->name) return;
        const std::string className = klass->name;
        std::string deckKey;
        if (className == "UserDeckPosition") {
            deckKey = "Deck/" + std::to_string(Get<int>(deckPosition, "get_Number"));
        } else if (className == "UserLeagueDeckPosition") {
            deckKey = "League/" + Text(deckPosition, "get_LeagueSeasonId") + "/" +
                      std::to_string(Get<int>(deckPosition, "get_DeckType"));
        } else {
            return;
        }
        const int position = Get<int>(deckPosition, "get_Position");
        const auto cardId = Text(deckPosition, "get_CardId");
        if (cardId.empty()) return;

        DeckVisualSlot saved;
        {
            std::lock_guard lock(deckVisualMutex);
            LoadDeckVisualsLocked();
            const auto deckIt = deckVisuals.find(deckKey);
            if (deckIt == deckVisuals.end()) return;
            const auto slotIt = deckIt->second.find(position);
            if (slotIt == deckIt->second.end() || slotIt->second.cardId != cardId) return;
            saved = slotIt->second;
        }
        if (saved.costumeId.empty() && saved.hairId.empty()) return;
        bool changed = false;
        if (!saved.costumeId.empty() && Text(deckPosition, "get_CostumeId") != saved.costumeId) {
            if (SetText(deckPosition, "set_CostumeId", saved.costumeId)) changed = true;
        }
        if (!saved.hairId.empty() && Text(deckPosition, "get_HairId") != saved.hairId) {
            if (SetText(deckPosition, "set_HairId", saved.hairId)) changed = true;
        }
        if (changed && Config::dbgMode) {
            Log::InfoFmt("DeckVisual: applied %s pos=%d card=%s -> (%s,%s)",
                         deckKey.c_str(), position, cardId.c_str(),
                         saved.costumeId.c_str(), saved.hairId.c_str());
        }
    }

    void ApplyLiveVisualOutfits(void* liveMessage) {
        if (!liveMessage || !Config::unlockAllLiveCostume) return;
        auto klass = Il2cppUtils::get_class_from_instance(liveMessage);
        if (!klass || !klass->name) return;
        const std::string className = klass->name;

        std::map<std::string, std::map<int, DeckVisualSlot>> snapshot;
        std::string activeKey;
        std::string selfUid;
        {
            std::lock_guard lock(deckVisualMutex);
            LoadDeckVisualsLocked();
            if (deckVisuals.empty()) return;
            snapshot = deckVisuals;
            activeKey = activeLiveDeckKey;
            selfUid = selfPublicUserId;
        }

        const auto findByCard = [&](const std::string& cardId, DeckVisualSlot& out) -> bool {
            if (cardId.empty()) return false;
            if (!activeKey.empty()) {
                if (const auto deckIt = snapshot.find(activeKey); deckIt != snapshot.end()) {
                    for (const auto& [pos, slot] : deckIt->second) {
                        if (slot.cardId == cardId) { out = slot; return true; }
                    }
                }
            }
            for (const auto& [key, slots] : snapshot) {
                for (const auto& [pos, slot] : slots) {
                    if (slot.cardId == cardId) { out = slot; return true; }
                }
            }
            return false;
        };

        if (className == "LiveUserInfo") {
            const int index = Get<int>(liveMessage, "get_Index");
            const bool isNpc = Get<bool>(liveMessage, "get_IsNpc");
            const auto userId = Text(liveMessage, "get_UserId");
            const bool isSelf = (!selfUid.empty() && !userId.empty())
                ? (userId == selfUid)
                : (index == 1 && !isNpc);
            if (!isSelf) return;
            auto userDeck = Get(liveMessage, "get_UserDeck");
            if (!userDeck) return;
            int applied = 0;
            const auto cards = Items(Get(userDeck, "get_Cards"));
            for (auto card : cards) {
                if (!card) continue;
                if (Get<bool>(card, "get_IsFriendBuddy") || Get<bool>(card, "get_IsTourScout")) continue;
                DeckVisualSlot saved;
                if (!findByCard(Text(card, "get_CardId"), saved)) continue;
                bool cardApplied = false;
                if (!saved.costumeId.empty() && SetText(card, "set_DisplayCostumeId", saved.costumeId)) {
                    cardApplied = true;
                }
                if (!saved.hairId.empty() && SetText(card, "set_DisplayHairId", saved.hairId)) {
                    cardApplied = true;
                }
                if (cardApplied) ++applied;
            }
            if (applied > 0 && Config::dbgMode) {
                Log::InfoFmt("DeckVisual: applied LiveUserInfo idx=%d overrides=%d/%zu deck=%s",
                             index, applied, cards.size(), activeKey.c_str());
            }
            return;
        }

        if (className == "QuestListAssetResponse" ||
            className == "MarathonQuestListAssetResponse" ||
            className == "RaceQuestListAssetResponse") {
            if (activeKey.empty()) return;
            const auto deckIt = snapshot.find(activeKey);
            if (deckIt == snapshot.end()) return;
            int applied = 0;
            std::set<int> usedPositions;
            const auto assets = Items(Get(liveMessage, "get_CharacterAssets"));
            for (auto info : assets) {
                if (!info) continue;
                const auto cid = Text(info, "get_CharacterId");
                if (cid.empty()) continue;
                const DeckVisualSlot* chosen = nullptr;
                int chosenPos = -1;
                for (const auto& [pos, slot] : deckIt->second) {
                    if (slot.characterId == cid && !usedPositions.contains(pos)) {
                        chosen = &slot;
                        chosenPos = pos;
                        break;
                    }
                }
                if (!chosen) continue;
                bool infoApplied = false;
                if (!chosen->costumeId.empty() && SetText(info, "set_CostumeId", chosen->costumeId)) {
                    infoApplied = true;
                }
                if (!chosen->hairId.empty() && SetText(info, "set_HairId", chosen->hairId)) {
                    infoApplied = true;
                }
                if (infoApplied) {
                    usedPositions.insert(chosenPos);
                    ++applied;
                }
            }
            if (applied > 0 && Config::dbgMode) {
                Log::InfoFmt("DeckVisual: applied %s asset overrides=%d/%zu deck=%s",
                             className.c_str(), applied, assets.size(), activeKey.c_str());
            }
        }
    }

    void SetValidators(CostumeValidator costume, HairValidator hair) {
        validateCostume = costume;
        validateHair = hair;
        if (Config::dbgMode) {
            Log::Info("PhotoRequests: native revision=fast-paths-v9-deck-live");
        }
    }

    void SetCostumeRoomCharacter(const std::string& characterId) {
        if (characterId.empty()) return;
        std::lock_guard lock(costumeRoomMutex);
        activeCostumeRoomCharacterId = characterId;
    }

    void* FindOwnedPhotoEntryOutfit(const std::string& characterId, void* selectedOutfit, void* costumeSpecifier) {
        if (characterId.empty() || !selectedOutfit) return nullptr;
        const auto selectedCostumeId = Text(selectedOutfit, "get_CostumeId");
        const auto selectedHairId = Text(selectedOutfit, "get_HairId");
        Inventory owned;
        {
            std::lock_guard lock(inventoryMutex);
            if (!inventory.ready) return nullptr;
            owned = inventory;
        }
        if (!owned.actors.contains(characterId)) return nullptr;

        const auto preferredPoseTypes = CollectExpressionPoseTypes(costumeSpecifier);
        if (owned.costumes.contains(selectedCostumeId) && owned.hairs.contains(selectedHairId) &&
            CostumeAllowed(Get(selectedOutfit, "get_Costume"), costumeSpecifier, characterId) &&
            HairAllowed(Get(selectedOutfit, "get_Hair"), characterId, selectedCostumeId) &&
            OutfitSupportsPoseTypes(Get(selectedOutfit, "get_Costume"), Get(selectedOutfit, "get_Hair"), preferredPoseTypes)) {
            return nullptr;
        }

        static auto actorClass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Data", "ActorCostume");
        if (!actorClass) return nullptr;

        std::string costumeId, hairId;
        if (FindOwnedOutfitPair(owned, characterId, selectedCostumeId, selectedHairId, costumeSpecifier,
                                costumeId, hairId, nullptr, preferredPoseTypes, /*requirePoseTypes=*/false)) {
            if (costumeId == selectedCostumeId && hairId == selectedHairId) {
                return nullptr;
            }
            auto costumeMaster = Find("get_CostumeMaster", costumeId);
            auto hairMaster = Find("get_HairMaster", hairId);
            auto actor = actorClass->New<void*>();
            auto ctor = MethodOf(actor, ".ctor", 2);
            if (!actor || !ctor) return nullptr;
            reinterpret_cast<void (*)(void*, void*, void*, const Method*)>(ctor->methodPointer)(
                actor, costumeMaster, hairMaster, ctor);
            if (Text(actor, "get_CostumeId") == costumeId && Text(actor, "get_HairId") == hairId) {
                if (Config::dbgMode) {
                    Log::InfoFmt("PhotoEntry: character=%s selected=(%s,%s) -> owned=(%s,%s)",
                                 characterId.c_str(), selectedCostumeId.c_str(), selectedHairId.c_str(),
                                 costumeId.c_str(), hairId.c_str());
                }
                return actor;
            }
        }
        if (costumeSpecifier) {
            if (auto forceActor = BuildForceActorCostume(characterId, costumeSpecifier)) {
                const auto forceCostumeId = Text(forceActor, "get_CostumeId");
                const auto forceHairId = Text(forceActor, "get_HairId");
                if (forceCostumeId == selectedCostumeId && forceHairId == selectedHairId) {
                    return nullptr;
                }
                if (Config::dbgMode) {
                    Log::InfoFmt("PhotoEntry: character=%s selected=(%s,%s) -> force=(%s,%s)",
                                 characterId.c_str(), selectedCostumeId.c_str(), selectedHairId.c_str(),
                                 forceCostumeId.c_str(), forceHairId.c_str());
                }
                return forceActor;
            }
        }
        if (Config::dbgMode) {
            Log::InfoFmt("PhotoEntry: no owned/force candidate for character=%s selected=(%s,%s)",
                         characterId.c_str(), selectedCostumeId.c_str(), selectedHairId.c_str());
        }
        return nullptr;
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
        if (!request || (!Config::unlockAllLiveCostume && !Config::unlockAllPhotoPose)) return PreparedRequest(request);
        const std::string name = Il2cppUtils::get_class_from_instance(request)->name;
        const bool costume = name.starts_with("Costume");
        if (costume && !Config::unlockAllLiveCostume) return PreparedRequest(request);
        const bool activity = name.starts_with("ActivityStart");
        if (activity && !Config::unlockAllLiveCostume) return PreparedRequest(request);
        const bool deck = IsDeckRequestName(name);
        if (deck && !Config::unlockAllLiveCostume) return PreparedRequest(request);

        Inventory owned;
        { std::lock_guard lock(inventoryMutex); owned = inventory; }
        if (!owned.ready) {
            Log::ErrorFmt("PhotoRequests: %s unchanged, server inventory unavailable", name.c_str());
            return PreparedRequest(request);
        }
        if (costume) {
            return PrepareCostumeRequest(request, name, owned);
        }
        if (activity) {
            return PrepareActivityStart(request, name, owned);
        }
        if (deck) {
            return PrepareDeckRequest(request, name, owned);
        }
        if (name == "PhotoCreateExpressionShootingsRequest") {
            return PrepareExpressionPoseRequest(request, owned);
        }
        if (IsPhotoRequestName(name)) {
            if (ArePhotoRequestActorsOwnedValid(request, name, owned)) {
                if (Config::dbgMode) {
                    Log::InfoFmt("PhotoRequests: %s forwarded original (server-owned entry valid)", name.c_str());
                }
                return PreparedRequest(request);
            }
            return PreparePhotoActorsFallback(request, name, owned);
        }
        return PreparedRequest(request);
    }
}
