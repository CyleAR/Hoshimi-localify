#include "MasterLocal.h"
#include "Local.h"
#include "Il2cppUtils.hpp"
#include "Misc.hpp"
#include "config/Config.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <regex>
#include <nlohmann/json.hpp>

namespace HoshimiLocal::MasterLocal {
    using Il2cppString = UnityResolve::UnityType::String;

    static std::unordered_map<std::string, Il2cppUtils::MethodInfo*> fieldSetCache;
    static std::unordered_map<std::string, Il2cppUtils::MethodInfo*> fieldGetCache;

    enum class JsonValueType {
        JVT_String,
        JVT_Int,
        JVT_Object,
        JVT_ArrayObject,
        JVT_ArrayString,
        JVT_Unsupported,
        JVT_NeedMore_EmptyArray
    };

    struct ItemRule {
        std::vector<std::string> mainPrimaryKey;
        std::map<std::string, std::vector<std::string>> subPrimaryKey;

        std::vector<std::string> mainLocalKey;
        std::map<std::string, std::vector<std::string>> subLocalKey;
    };

    struct TableLocalData {
        ItemRule itemRule;

        std::unordered_map<std::string, JsonValueType> mainKeyType;
        std::unordered_map<std::string, std::unordered_map<std::string, JsonValueType>> subKeyType;

        std::unordered_map<std::string, std::string> transData;
        std::unordered_map<std::string, std::vector<std::string>> transStrListData;

        [[nodiscard]] JsonValueType GetMainKeyType(const std::string& mainKey) const {
            if (auto it = mainKeyType.find(mainKey); it != mainKeyType.end()) {
                return it->second;
            }
            return JsonValueType::JVT_Unsupported;
        }

        [[nodiscard]] JsonValueType GetSubKeyType(const std::string& parentKey, const std::string& subKey) const {
            if (auto it = subKeyType.find(parentKey); it != subKeyType.end()) {
                if (auto subIt = it->second.find(subKey); subIt != it->second.end()) {
                    return subIt->second;
                }
            }
            return JsonValueType::JVT_Unsupported;
        }
    };

    static std::unordered_map<std::string, TableLocalData> masterLocalData;

    bool IsFlatArrayStringLocalKey(const std::string& tableName, const std::string& parent, const std::string& child) {
        return tableName == "Tutorial" && parent == "stepInfo" && child == "texts";
    }

    bool IsFlatArrayStringDataKey(const std::string& tableName, const std::string& key) {
        if (tableName != "Tutorial") return false;
        auto pipePos = key.find('|');
        if (pipePos == std::string::npos) return false;
        auto suffix = key.substr(pipePos + 1);
        return suffix.rfind("stepInfo[", 0) == 0 && suffix.size() >= 6 &&
               suffix.compare(suffix.size() - 6, 6, ".texts") == 0;
    }

    std::vector<std::string> SplitTutorialTextMarkers(const std::string& value) {
        static const std::string firstMarker = "[LA_F]";
        static const std::string nextMarker = "[LA_N_F]";
        std::vector<std::string> ret;
        size_t pos = 0;

        while (pos < value.size()) {
            if (value.compare(pos, firstMarker.size(), firstMarker) == 0) {
                pos += firstMarker.size();
                continue;
            }
            if (value.compare(pos, nextMarker.size(), nextMarker) == 0) {
                pos += nextMarker.size();
                continue;
            }

            auto firstPos = value.find(firstMarker, pos);
            auto nextPos = value.find(nextMarker, pos);
            auto endPos = std::min(firstPos == std::string::npos ? value.size() : firstPos,
                                   nextPos == std::string::npos ? value.size() : nextPos);
            auto part = value.substr(pos, endPos - pos);
            if (!part.empty()) ret.push_back(part);
            pos = endPos;
        }

        if (ret.empty() && value.find(firstMarker) == std::string::npos && value.find(nextMarker) == std::string::npos) {
            ret.push_back(value);
        }
        return ret;
    }
    class FieldController {
        void* self;
        std::string self_klass_name;

        static std::string capitalizeFirstLetter(const std::string& input) {
            if (input.empty()) return input;
            std::string result = input;
            result[0] = static_cast<char>(std::toupper(result[0]));
            return result;
        }

        Il2cppUtils::MethodInfo* GetGetSetMethodFromCache(const std::string& fieldName, int argsCount,
                                                          std::unordered_map<std::string, Il2cppUtils::MethodInfo*>& fromCache, const std::string& prefix = "set_") {
            const std::string methodName = prefix + capitalizeFirstLetter(fieldName);
            const std::string searchName = self_klass_name + "." + methodName;

            if (auto it = fromCache.find(searchName); it != fromCache.end()) {
                return it->second;
            }
            auto set_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    self_klass,
                    methodName.c_str(),
                    argsCount
            );
            fromCache.emplace(searchName, set_mtd);
            return set_mtd;
        }

    public:
        Il2cppUtils::Il2CppClassHead* self_klass;

        explicit FieldController(void* from) {
            if (!from) {
                self = nullptr;
                return;
            }
            self = from;
            self_klass = Il2cppUtils::get_class_from_instance(self);
            if (self_klass) {
                self_klass_name = self_klass->name;
            }
        }

        template<typename T>
        T ReadField(const std::string& fieldName) {
            if (!self) return T();
            auto get_mtd = GetGetSetMethodFromCache(fieldName, 0, fieldGetCache, "get_");
            if (get_mtd) {
                return reinterpret_cast<T (*)(void*, void*)>(get_mtd->methodPointer)(self, get_mtd);
            }

            auto field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>(
                    "il2cpp_class_get_field_from_name",
                    self_klass,
                    (fieldName + '_').c_str()
            );
            if (!field) {
                return T();
            }
            return Il2cppUtils::ClassGetFieldValue<T>(self, field);
        }

        template<typename T>
        void SetField(const std::string& fieldName, T value) {
            if (!self) return;
            auto set_mtd = GetGetSetMethodFromCache(fieldName, 1, fieldSetCache, "set_");
            if (set_mtd) {
                reinterpret_cast<void (*)(void*, T, void*)>(
                        set_mtd->methodPointer
                )(self, value, set_mtd);
                return;
            }
            auto field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>(
                    "il2cpp_class_get_field_from_name",
                    self_klass,
                    (fieldName + '_').c_str()
            );
            if (!field) return;
            Il2cppUtils::ClassSetFieldValue(self, field, value);
        }

        int ReadIntField(const std::string& fieldName) {
            return ReadField<int>(fieldName);
        }

        Il2cppString* ReadStringField(const std::string& fieldName) {
            if (!self) return nullptr;
            auto get_mtd = GetGetSetMethodFromCache(fieldName, 0, fieldGetCache, "get_");
            if (!get_mtd) {
                return ReadField<Il2cppString*>(fieldName);
            }
            auto returnClass = UnityResolve::Invoke<Il2cppUtils::Il2CppClassHead*>(
                    "il2cpp_class_from_type",
                    UnityResolve::Invoke<void*>("il2cpp_method_get_return_type", get_mtd)
            );
            if (!returnClass) {
                return reinterpret_cast<Il2cppString* (*)(void*, void*)>(
                        get_mtd->methodPointer
                )(self, get_mtd);
            }
            auto isEnum = UnityResolve::Invoke<bool>("il2cpp_class_is_enum", returnClass);
            if (!isEnum) {
                return reinterpret_cast<Il2cppString* (*)(void*, void*)>(
                        get_mtd->methodPointer
                )(self, get_mtd);
            }
            auto enumMap = Il2cppUtils::EnumToValueMap(returnClass, true);
            auto enumValue = reinterpret_cast<int (*)(void*, void*)>(
                    get_mtd->methodPointer
            )(self, get_mtd);
            if (auto it = enumMap.find(enumValue); it != enumMap.end()) {
                return Il2cppString::New(it->second);
            }
            return nullptr;
        }

        bool ReadEnumIntField(const std::string& fieldName, int* outValue) {
            if (!self || !outValue) return false;
            auto get_mtd = GetGetSetMethodFromCache(fieldName, 0, fieldGetCache, "get_");
            if (!get_mtd) return false;

            auto returnClass = UnityResolve::Invoke<Il2cppUtils::Il2CppClassHead*>(
                    "il2cpp_class_from_type",
                    UnityResolve::Invoke<void*>("il2cpp_method_get_return_type", get_mtd)
            );
            if (!returnClass) return false;

            auto isEnum = UnityResolve::Invoke<bool>("il2cpp_class_is_enum", returnClass);
            if (!isEnum) return false;

            *outValue = reinterpret_cast<int (*)(void*, void*)>(
                    get_mtd->methodPointer
            )(self, get_mtd);
            return true;
        }

        void SetStringField(const std::string& fieldName, const std::string& value) {
            if (!self) return;
            auto newString = Il2cppString::New(Config::ReplaceDisplayUserName(value));
            SetField(fieldName, newString);
        }

        void SetStringListField(const std::string& fieldName, const std::vector<std::string>& data) {
            if (!self) return;
            auto targetList = ReadObjectField(fieldName);
            if (!targetList) {
                Log::ErrorFmt("SetStringListField failed: %s has no existing collection", fieldName.c_str());
                return;
            }

            auto listKlass = Il2cppUtils::get_class_from_instance(targetList);
            auto clearMtd = listKlass ? Il2cppUtils::il2cpp_class_get_method_from_name(listKlass, "Clear", 0) : nullptr;
            if (!clearMtd) {
                Log::ErrorFmt("SetStringListField failed: %s collection has no Clear method", fieldName.c_str());
                return;
            }

            reinterpret_cast<void (*)(void*, void*)>(clearMtd->methodPointer)(targetList, clearMtd);

            Il2cppUtils::Tools::CSListEditor<Il2cppString*> newListEditor(targetList);
            for (auto& s : data) {
                newListEditor.Add(Il2cppString::New(Config::ReplaceDisplayUserName(s)));
            }
        }

        void* ReadObjectField(const std::string& fieldName) {
            if (!self) return nullptr;
            return ReadField<void*>(fieldName);
        }

        void* ReadObjectListField(const std::string& fieldName) {
            if (!self) return nullptr;
            return ReadField<void*>(fieldName);
        }

        static FieldController CreateSubFieldController(void* subObj) {
            return FieldController(subObj);
        }

        FieldController CreateSubFieldController(const std::string& subObjName) {
            auto field = ReadObjectField(subObjName);
            return FieldController(field);
        }
    };


    JsonValueType checkJsonValueType(const nlohmann::json& j) {
        if (j.is_string())  return JsonValueType::JVT_String;
        if (j.is_number_integer()) return JsonValueType::JVT_Int;
        if (j.is_object())  return JsonValueType::JVT_Object;
        if (j.is_array()) {
            if (!j.empty()) {
                if (j.begin()->is_object()) {
                    return JsonValueType::JVT_ArrayObject;
                }
                else if (j.begin()->is_string()) {
                    return JsonValueType::JVT_ArrayString;
                }
            }
            else {
                return JsonValueType::JVT_NeedMore_EmptyArray;
            }
        }
        return JsonValueType::JVT_Unsupported;
    }


    std::string ReadFileToString(const std::filesystem::path& path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return {};
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        return buffer.str();
    }

    namespace Load {
        std::vector<std::string> ArrayStrJsonToVec(nlohmann::json& data) {
            return data;
        }

        bool BuildObjectItemLocalRule(nlohmann::json& transData, ItemRule& itemRule) {
            // transData: data[]
            bool hasSuccess = false;
            for (auto& data : transData) {
                // data: {"id": "xxx", "produceDescriptions": [{"k", "v"}], "descriptions": {"k2", "v2"}}
                if (!data.is_object()) continue;
                for (auto& [key, value] : data.items()) {
                    // key: "id", value: "xxx"
                    // key: "produceDescriptions", value: [{"k", "v"}]
                    const auto valueType = checkJsonValueType(value);
                    switch (valueType) {
                        case JsonValueType::JVT_String:
                            // case JsonValueType::JVT_Int:
                        case JsonValueType::JVT_ArrayString: {
                            if (std::find(itemRule.mainPrimaryKey.begin(), itemRule.mainPrimaryKey.end(), key) != itemRule.mainPrimaryKey.end()) {
                                continue;
                            }
                            if (auto it = std::find(itemRule.mainLocalKey.begin(), itemRule.mainLocalKey.end(), key); it == itemRule.mainLocalKey.end()) {
                                itemRule.mainLocalKey.emplace_back(key);
                            }
                            hasSuccess = true;
                        } break;

                        case JsonValueType::JVT_Object: {
                            ItemRule currRule{ .mainPrimaryKey = itemRule.subPrimaryKey[key] };

                            auto vJson = nlohmann::json::array();
                            vJson.push_back(value);

                            if (BuildObjectItemLocalRule(vJson, currRule)) {
                                itemRule.subLocalKey.emplace(key, currRule.mainLocalKey);
                                hasSuccess = true;
                            }
                        } break;

                        case JsonValueType::JVT_ArrayObject: {
                            for (auto& obj : value) {
                                // obj: {"k", "v"}
                                ItemRule currRule{ .mainPrimaryKey = itemRule.subPrimaryKey[key] };
                                if (BuildObjectItemLocalRule(value, currRule)) {
                                    itemRule.subLocalKey.emplace(key, currRule.mainLocalKey);
                                    hasSuccess = true;
                                    break;
                                }
                            }
                        } break;

                        case JsonValueType::JVT_Unsupported:
                        default:
                            break;
                    }
                }
                if (hasSuccess) break;
            }
            return hasSuccess;
        }

        bool GetItemRule(nlohmann::json& fullData, ItemRule& itemRule) {
            auto& primaryKeys = fullData["rules"]["primaryKeys"];
            auto& transData = fullData["data"];
            if (!primaryKeys.is_array()) return false;
            if (!transData.is_array()) return false;

            // 首先构造 mainPrimaryKey 规则
            for (auto& pkItem : primaryKeys) {
                if (!pkItem.is_string()) {
                    return false;
                }
                std::string pk = pkItem;
                auto dotCount = std::ranges::count(pk, '.');
                if (dotCount == 0) {
                    itemRule.mainPrimaryKey.emplace_back(pk);
                }
                else if (dotCount == 1) {
                    auto [parentKey, subKey] = Misc::StringFormat::split_once(pk, ".");
                    if (itemRule.subPrimaryKey.contains(parentKey)) {
                        itemRule.subPrimaryKey[parentKey].emplace_back(subKey);
                    }
                    else {
                        itemRule.subPrimaryKey.emplace(parentKey, std::vector<std::string>{subKey});
                    }
                }
                else {
                    Log::ErrorFmt("Unsupported depth: %d", dotCount);
                    continue;
                }
            }
            return BuildObjectItemLocalRule(transData, itemRule);
        }

        std::string BuildBaseMainUniqueKey(nlohmann::json& data, TableLocalData& tableLocalData) {
            try {
                std::string mainBaseUniqueKey;
                for (auto& mainPrimaryKey : tableLocalData.itemRule.mainPrimaryKey) {
                    if (!data.contains(mainPrimaryKey)) {
                        return "";
                    }
                    auto& value = data[mainPrimaryKey];
                    if (value.is_number_integer()) {
                        mainBaseUniqueKey.append(std::to_string(value.get<int>()));
                    }
                    else {
                        mainBaseUniqueKey.append(value);
                    }
                    mainBaseUniqueKey.push_back('|');
                }
                return mainBaseUniqueKey;
            }
            catch (std::exception& e) {
                Log::ErrorFmt("LoadData - BuildBaseMainUniqueKey failed: %s", e.what());
                throw e;
            }
        }

        void BuildBaseObjectSubUniqueKey(nlohmann::json& value, JsonValueType valueType, std::string& currLocalKey) {
            switch (valueType) {
                case JsonValueType::JVT_String:
                    currLocalKey.append(value.get<std::string>());  // p_card-00-acc-0_002|0|produceDescriptions|ProduceDescriptionType_Exam|
                    currLocalKey.push_back('|');
                    break;
                case JsonValueType::JVT_Int:
                    currLocalKey.append(std::to_string(value.get<int>()));
                    currLocalKey.push_back('|');
                    break;
                default:
                    break;
            }
        }

        bool BuildUniqueKeyValue(nlohmann::json& data, TableLocalData& tableLocalData) {
            // 首先处理 main 部分
            const std::string mainBaseUniqueKey = BuildBaseMainUniqueKey(data, tableLocalData);  // p_card-00-acc-0_002|0|
            if (mainBaseUniqueKey.empty()) return false;
            for (auto& mainLocalKey : tableLocalData.itemRule.mainLocalKey) {
                if (!data.contains(mainLocalKey)) continue;
                auto& currLocalValue = data[mainLocalKey];
                auto currUniqueKey = mainBaseUniqueKey + mainLocalKey;  // p_card-00-acc-0_002|0|name
                if (tableLocalData.GetMainKeyType(mainLocalKey) == JsonValueType::JVT_ArrayString) {
                    tableLocalData.transStrListData.emplace(currUniqueKey, ArrayStrJsonToVec(currLocalValue));
                }
                else {
                    tableLocalData.transData.emplace(currUniqueKey, currLocalValue);
                }
            }
            // 然后处理 sub 部分
            /*
            for (const auto& [subPrimaryParentKey, subPrimarySubKeys] : tableLocalData.itemRule.subPrimaryKey) {
                if (!data.contains(subPrimaryParentKey)) continue;

                const std::string subBaseUniqueKey = mainBaseUniqueKey + subPrimaryParentKey + '|';  // p_card-00-acc-0_002|0|produceDescriptions|

                auto subValueType = checkJsonValueType(data[subPrimaryParentKey]);
                std::string currLocalKey = subBaseUniqueKey;  // p_card-00-acc-0_002|0|produceDescriptions|
                switch (subValueType) {
                    case JsonValueType::JVT_Object: {
                        for (auto& subPrimarySubKey : subPrimarySubKeys) {
                            if (!data[subPrimaryParentKey].contains(subPrimarySubKey)) continue;
                            auto& value = data[subPrimaryParentKey][subPrimarySubKey];
                            auto valueType = tableLocalData.GetSubKeyType(subPrimaryParentKey, subPrimarySubKey);
                            BuildBaseObjectSubUniqueKey(value, valueType, currLocalKey);  // p_card-00-acc-0_002|0|produceDescriptions|ProduceDescriptionType_Exam|
                        }
                    } break;
                    case JsonValueType::JVT_ArrayObject: {
                        int currIndex = 0;
                        for (auto& obj : data[subPrimaryParentKey]) {
                            for (auto& subPrimarySubKey : subPrimarySubKeys) {

                            }
                            currIndex++;
                        }
                    } break;
                    default:
                        break;
                }
            }*/

            for (const auto& [subLocalParentKey, subLocalSubKeys] : tableLocalData.itemRule.subLocalKey) {
                if (!data.contains(subLocalParentKey)) continue;

                const std::string subBaseUniqueKey = mainBaseUniqueKey + subLocalParentKey + '|';  // p_card-00-acc-0_002|0|produceDescriptions|
                auto subValueType = checkJsonValueType(data[subLocalParentKey]);
                if (subValueType != JsonValueType::JVT_NeedMore_EmptyArray) {
                    tableLocalData.mainKeyType.emplace(subLocalParentKey, subValueType);  // 在这里插入 subParent 的类型
                }
                switch (subValueType) {
                    case JsonValueType::JVT_Object: {
                        for (auto& localSubKey : subLocalSubKeys) {
                            const std::string currLocalUniqueKey = subBaseUniqueKey + localSubKey;  // p_card-00-acc-0_002|0|produceDescriptions|text
                            if (tableLocalData.GetSubKeyType(subLocalParentKey, localSubKey) == JsonValueType::JVT_ArrayString) {
                                tableLocalData.transStrListData.emplace(currLocalUniqueKey, ArrayStrJsonToVec(data[subLocalParentKey][localSubKey]));
                            }
                            else {
                                tableLocalData.transData.emplace(currLocalUniqueKey, data[subLocalParentKey][localSubKey]);
                            }
                        }
                    } break;
                    case JsonValueType::JVT_ArrayObject: {
                        int currIndex = 0;
                        for (auto& obj : data[subLocalParentKey]) {
                            for (auto& localSubKey : subLocalSubKeys) {
                                std::string currLocalUniqueKey = subBaseUniqueKey;  // p_card-00-acc-0_002|0|produceDescriptions|
                                currLocalUniqueKey.push_back('[');
                                currLocalUniqueKey.append(std::to_string(currIndex));
                                currLocalUniqueKey.append("]|");
                                currLocalUniqueKey.append(localSubKey);  // p_card-00-acc-0_002|0|produceDescriptions|[0]|text

                                if (tableLocalData.GetSubKeyType(subLocalParentKey, localSubKey) == JsonValueType::JVT_ArrayString) {
                                    // if (obj[localSubKey].is_array()) {
                                    tableLocalData.transStrListData.emplace(currLocalUniqueKey, ArrayStrJsonToVec(obj[localSubKey]));
                                }
                                else if (obj[localSubKey].is_string()) {
                                    tableLocalData.transData.emplace(currLocalUniqueKey, obj[localSubKey]);
                                }
                            }
                            currIndex++;
                        }
                    } break;
                    default:
                        break;
                }
            }
            return true;
        }

#define MainKeyTypeProcess() if (!data.contains(mainPrimaryKey)) { Log::ErrorFmt("mainPrimaryKey: %s not found", mainPrimaryKey.c_str()); isFailed = true; break; } \
    auto currType = checkJsonValueType(data[mainPrimaryKey]); \
    if (currType == JsonValueType::JVT_NeedMore_EmptyArray) goto NextLoop; \
    tableLocalData.mainKeyType[mainPrimaryKey] = currType
#define SubKeyTypeProcess() if (!data.contains(subKeyParent)) { Log::ErrorFmt("subKeyParent: %s not found", subKeyParent.c_str()); isFailed = true; break; } \
                for (auto& subKey : subKeys) { \
                    auto& subKeyValue = data[subKeyParent]; \
                    if (subKeyValue.is_object()) { \
                        if (!subKeyValue.contains(subKey)) { \
                            Log::ErrorFmt("subKey: %s not in subKeyParent: %s", subKey.c_str(), subKeyParent.c_str()); isFailed = true; break; \
                        }                                                                                                                                    \
                        auto currType = checkJsonValueType(subKeyValue[subKey]);                                                                             \
                        if (currType == JsonValueType::JVT_NeedMore_EmptyArray) goto NextLoop; \
                        tableLocalData.subKeyType[subKeyParent].emplace(subKey, currType); \
                    } \
                    else if (subKeyValue.is_array()) {                                                                                                       \
                        if (subKeyValue.empty()) goto NextLoop;                                                                                              \
                        for (auto& i : subKeyValue) { \
                            if (!i.is_object()) continue; \
                            if (!i.contains(subKey)) continue;  \
                            auto currType = checkJsonValueType(i[subKey]); \
                            if (currType == JsonValueType::JVT_NeedMore_EmptyArray) goto NextLoop; \
                            tableLocalData.subKeyType[subKeyParent].emplace(subKey, currType); \
                            break; \
                        } \
                    }                                                                                                                                        \
                    else {                                                                                                                                   \
                        goto NextLoop;\
                    } \
                }

        bool GetTableLocalData(nlohmann::json& fullData, TableLocalData& tableLocalData) {
            bool isFailed = false;

            // 首先 Build mainKeyType 和 subKeyType
            for (auto& data : fullData["data"]) {
                if (!data.is_object()) continue;

                for (auto& mainPrimaryKey : tableLocalData.itemRule.mainPrimaryKey) {
                    MainKeyTypeProcess();
                }
                for (auto& mainPrimaryKey : tableLocalData.itemRule.mainLocalKey) {
                    MainKeyTypeProcess();
                }

                for (const auto& [subKeyParent, subKeys] : tableLocalData.itemRule.subPrimaryKey) {
                    SubKeyTypeProcess()

                    if (isFailed) break;
                }
                for (const auto& [subKeyParent, subKeys] : tableLocalData.itemRule.subLocalKey) {
                    SubKeyTypeProcess()
                    if (isFailed) break;
                }
                if (!isFailed) break;
            NextLoop:
                ;
            }

            if (isFailed) return false;

            bool hasSuccess = false;
            // 然后构造 transData
            for (auto& data : fullData["data"]) {
                if (!data.is_object()) continue;
                if (BuildUniqueKeyValue(data, tableLocalData)) {
                    hasSuccess = true;
                }
            }
            if (!hasSuccess) {
                Log::ErrorFmt("BuildUniqueKeyValue failed.");
            }
            return hasSuccess;
        }

        void LoadData() {
            masterLocalData.clear();
            static auto masterDir = Local::GetBasePath() / "local-files" / "masterTrans";
            if (!std::filesystem::is_directory(masterDir)) {
                Log::ErrorFmt("LoadData: not found: %s", masterDir.string().c_str());
                return;
            }

            bool isFirstIteration = true;
            for (auto& p : std::filesystem::directory_iterator(masterDir)) {
                if (isFirstIteration) {
                    auto totalFileCount = std::distance(
                            std::filesystem::directory_iterator(masterDir),
                            std::filesystem::directory_iterator{}
                    );
                    UnityResolveProgress::classProgress.total = totalFileCount <= 0 ? 1 : totalFileCount;
                    isFirstIteration = false;
                }
                UnityResolveProgress::classProgress.current++;

                if (!p.is_regular_file()) continue;
                const auto& path = p.path();
                if (path.extension() != ".json") continue;

                std::string tableName = path.stem().string();
                auto fileContent = ReadFileToString(path);
                if (fileContent.empty()) continue;

                try {
                    auto j = nlohmann::json::parse(fileContent);
                    ItemRule currRule;
                    TableLocalData tableLocalData;

                    if (j.contains("rule") && j["rule"].is_array() && j.contains("data") && j["data"].is_object()) {
                        // 새로운 최적화된 형식 (Flat Map) 처리
                        for (auto& ruleItem : j["rule"]) {
                            if (!ruleItem.is_string()) continue;
                            std::string ruleStr = ruleItem;
                            auto parts = Misc::StringFormat::split(ruleStr, '|');
                            if (parts.size() < 2) continue;

                            // 마지막 항목은 LocalKey, 나머지는 PrimaryKeys
                            std::string localKey = parts.back();
                            parts.pop_back();

                            // PK 등록 (중복 방지)
                            for (const auto& pk : parts) {
                                if (std::find(currRule.mainPrimaryKey.begin(), currRule.mainPrimaryKey.end(), pk) == currRule.mainPrimaryKey.end()) {
                                    currRule.mainPrimaryKey.push_back(pk);
                                }
                                tableLocalData.mainKeyType[pk] = JsonValueType::JVT_String; // 기본적으로 문자열로 취급
                            }

                            // LocalKey 등록
                            if (localKey.find('.') != std::string::npos) {
                                auto subParts = Misc::StringFormat::split(localKey, '.');
                                if (subParts.size() >= 2) {
                                    std::string parent = subParts[0];
                                    std::string child = subParts[1];
                                    currRule.subLocalKey[parent].push_back(child);
                                    if (tableLocalData.mainKeyType.find(parent) == tableLocalData.mainKeyType.end()) {
                                        tableLocalData.mainKeyType[parent] = JsonValueType::JVT_Object;
                                    }
                                    tableLocalData.subKeyType[parent][child] = IsFlatArrayStringLocalKey(tableName, parent, child) ? JsonValueType::JVT_ArrayString : JsonValueType::JVT_String;
                                }
                            } else {
                                currRule.mainLocalKey.push_back(localKey);
                                tableLocalData.mainKeyType[localKey] = JsonValueType::JVT_String;
                            }
                        }

                        // 번역 데이터 직접 매핑
                        for (auto& [k, v] : j["data"].items()) {
                            if (v.is_string()) {
                                if (IsFlatArrayStringDataKey(tableName, k)) {
                                    tableLocalData.transStrListData.emplace(k, SplitTutorialTextMarkers(v.get<std::string>()));
                                } else {
                                    tableLocalData.transData.emplace(k, v);
                                }
                                if (k.find('[') != std::string::npos) {
                                    auto pipePos = k.find('|');
                                    auto dotPos = k.find('.');
                                    if (pipePos != std::string::npos && dotPos != std::string::npos && dotPos > pipePos) {
                                        auto bracketPos = k.find('[', pipePos);
                                        if (bracketPos != std::string::npos && bracketPos < dotPos) {
                                            std::string parent = k.substr(pipePos + 1, bracketPos - pipePos - 1);
                                            tableLocalData.mainKeyType[parent] = JsonValueType::JVT_ArrayObject;
                                        }
                                    }
                                }
                                Local::translatedText.emplace(v);
                            } else if (v.is_array() && IsFlatArrayStringDataKey(tableName, k)) {
                                std::vector<std::string> splitTexts;
                                for (const auto& item : v) {
                                    if (!item.is_string()) continue;
                                    auto parts = SplitTutorialTextMarkers(item.get<std::string>());
                                    splitTexts.insert(splitTexts.end(), parts.begin(), parts.end());
                                    Local::translatedText.emplace(item);
                                }
                                tableLocalData.transStrListData.emplace(k, splitTexts);
                            }
                        }
                        tableLocalData.itemRule = std::move(currRule);
                    }
                    else if (j.contains("rules") && j["rules"].contains("primaryKeys")) {
                        // 기존 방식 처리
                        if (!GetItemRule(j, currRule)) {
                            Log::ErrorFmt("GetItemRule failed: %s", path.string().c_str());
                            continue;
                        }
                        tableLocalData.itemRule = std::move(currRule);
                        if (!GetTableLocalData(j, tableLocalData)) {
                            Log::ErrorFmt("GetTableLocalData failed: %s", path.string().c_str());
                            continue;
                        }
                        // 기존 방식의 데이터 추출
                        for (auto& i : tableLocalData.transData) {
                            Local::translatedText.emplace(i.second);
                        }
                        for (auto& i : tableLocalData.transStrListData) {
                            for (auto& str : i.second) {
                                Local::translatedText.emplace(str);
                            }
                        }
                    }
                    else {
                        continue;
                    }

                    masterLocalData.emplace(tableName, std::move(tableLocalData));
                } catch (std::exception& e) {
                    Log::ErrorFmt("MasterLocal::LoadData: parse error in '%s': %s",
                                  path.string().c_str(), e.what());
                }
            }
        }
    }

    void LoadData() {
        return Load::LoadData();
    }

    std::string GetTransString(const std::string& key, const TableLocalData& localData) {
        if (auto it = localData.transData.find(key); it != localData.transData.end()) {
            return it->second;
        }
        return {};
    }

    std::vector<std::string> GetTransArrayString(const std::string& key, const TableLocalData& localData) {
        if (auto it = localData.transStrListData.find(key); it != localData.transStrListData.end()) {
            return it->second;
        }
        return {};
    }

    void AppendUnique(std::vector<std::string>& values, const std::string& value) {
        for (const auto& existing : values) {
            if (existing == value) return;
        }
        values.push_back(value);
    }

    std::vector<std::string> AppendCandidateKeyParts(
            const std::vector<std::string>& baseKeys,
            const std::vector<std::string>& parts
    ) {
        std::vector<std::string> nextKeys;
        for (const auto& baseKey : baseKeys) {
            for (const auto& part : parts) {
                auto nextKey = baseKey + part + "|";
                AppendUnique(nextKeys, nextKey);
            }
        }
        return nextKeys;
    }

    std::string JoinCandidateKeys(const std::vector<std::string>& keys) {
        std::string joined;
        for (const auto& key : keys) {
            if (!joined.empty()) joined.append(",");
            joined.append(key);
        }
        return joined;
    }

    std::string GetTransStringFromCandidates(
            const std::vector<std::string>& baseKeys,
            const std::string& suffix,
            const TableLocalData& localData,
            std::string* matchedKey
    ) {
        for (const auto& baseKey : baseKeys) {
            auto key = baseKey + suffix;
            auto value = GetTransString(key, localData);
            if (!value.empty()) {
                if (matchedKey && matchedKey->empty()) *matchedKey = key;
                return value;
            }
        }
        return {};
    }

    std::vector<std::string> GetTransArrayStringFromCandidates(
            const std::vector<std::string>& baseKeys,
            const std::string& suffix,
            const TableLocalData& localData,
            std::string* matchedKey
    ) {
        for (const auto& baseKey : baseKeys) {
            auto key = baseKey + suffix;
            auto value = GetTransArrayString(key, localData);
            if (!value.empty()) {
                if (matchedKey && matchedKey->empty()) *matchedKey = key;
                return value;
            }
        }
        return {};
    }

    void ReplaceDisplayUserNameInStringField(FieldController& fc, const std::string& fieldName) {
        auto currentValue = fc.ReadStringField(fieldName);
        if (!currentValue) return;

        auto currentText = currentValue->ToString();
        auto replacedText = Config::ReplaceDisplayUserName(currentText);
        if (replacedText != currentText) {
            fc.SetStringField(fieldName, replacedText);
        }
    }

    void ReplaceDisplayUserNameInMasterItem(FieldController& fc, const TableLocalData& localData) {
        if (Config::displayUserName.empty()) return;

        for (const auto& mainLocalKey : localData.itemRule.mainLocalKey) {
            if (localData.GetMainKeyType(mainLocalKey) == JsonValueType::JVT_String) {
                ReplaceDisplayUserNameInStringField(fc, mainLocalKey);
            }
        }

        for (const auto& [subParentKey, subLocalKeys] : localData.itemRule.subLocalKey) {
            const auto subParentType = localData.GetMainKeyType(subParentKey);
            if (subParentType == JsonValueType::JVT_Object) {
                auto subField = fc.CreateSubFieldController(subParentKey);
                for (const auto& subLocalKey : subLocalKeys) {
                    if (localData.GetSubKeyType(subParentKey, subLocalKey) == JsonValueType::JVT_String) {
                        ReplaceDisplayUserNameInStringField(subField, subLocalKey);
                    }
                }
            }
            else if (subParentType == JsonValueType::JVT_ArrayObject) {
                auto subList = fc.ReadObjectListField(subParentKey);
                if (!subList) continue;

                Il2cppUtils::Tools::CSListEditor<void*> subListEditor(subList);
                if (!subListEditor.list_klass) continue;

                auto count = subListEditor.get_Count();
                if (count < 0 || count > 10000) continue;

                for (int index = 0; index < count; index++) {
                    auto subItem = subListEditor.get_Item(index);
                    if (!subItem) continue;

                    auto subField = FieldController::CreateSubFieldController(subItem);
                    for (const auto& subLocalKey : subLocalKeys) {
                        if (localData.GetSubKeyType(subParentKey, subLocalKey) == JsonValueType::JVT_String) {
                            ReplaceDisplayUserNameInStringField(subField, subLocalKey);
                        }
                    }
                }
            }
        }
    }

    void LocalizeMasterItem(FieldController& fc, const std::string& tableName) {
        auto it = masterLocalData.find(tableName);
        static std::unordered_set<std::string> loggedNoDataTables;
        if (it == masterLocalData.end()) {
            if (Config::debugMasterDbLog && loggedNoDataTables.insert(tableName).second) {
                Log::DebugFmt("ResourceDebug[MasterDB.Translate]: table=%s result=no_translation_data", tableName.c_str());
            }
            return;
        }

        const auto& localData = it->second;
        std::vector<std::string> baseDataKeys { "" };

        for (auto& mainPk : localData.itemRule.mainPrimaryKey) {
            auto mainPkType = localData.GetMainKeyType(mainPk);
            switch (mainPkType) {
                case JsonValueType::JVT_Int: {
                    auto readValue = std::to_string(fc.ReadIntField(mainPk));
                    baseDataKeys = AppendCandidateKeyParts(baseDataKeys, { readValue });
                } break;
                case JsonValueType::JVT_String: {
                    std::vector<std::string> keyParts;
                    auto readValue = fc.ReadStringField(mainPk);
                    if (!readValue) return;
                    AppendUnique(keyParts, readValue->ToString());

                    int enumValue = 0;
                    if (fc.ReadEnumIntField(mainPk, &enumValue)) {
                        AppendUnique(keyParts, std::to_string(enumValue));
                    }

                    baseDataKeys = AppendCandidateKeyParts(baseDataKeys, keyParts);
                } break;
                default:
                    break;
            }
        }

        ReplaceDisplayUserNameInMasterItem(fc, localData);

        int appliedStringCount = 0;
        int appliedArrayStringCount = 0;
        std::string matchedKeyForLog;

        for (auto& mainLocal : localData.itemRule.mainLocalKey) {
            auto localVType = localData.GetMainKeyType(mainLocal);
            switch (localVType) {
                case JsonValueType::JVT_String: {
                    auto localValue = GetTransStringFromCandidates(baseDataKeys, mainLocal, localData, &matchedKeyForLog);
                    if (!localValue.empty()) {
                        fc.SetStringField(mainLocal, localValue);
                        appliedStringCount++;
                    }
                } break;
                case JsonValueType::JVT_ArrayString: {
                    auto localValue = GetTransArrayStringFromCandidates(baseDataKeys, mainLocal, localData, &matchedKeyForLog);
                    if (!localValue.empty()) {
                        fc.SetStringListField(mainLocal, localValue);
                        appliedArrayStringCount++;
                    }
                } break;
                default:
                    break;
            }
        }

        for (const auto& [subParentKey, subLocalKeys] : localData.itemRule.subLocalKey) {
            const auto subParentType = localData.GetMainKeyType(subParentKey);
            switch (subParentType) {
                case JsonValueType::JVT_Object: {
                    auto subParentField = fc.CreateSubFieldController(subParentKey);
                    for (const auto& subLocalKey : subLocalKeys) {
                        const auto suffix = subParentKey + "." + subLocalKey;
                        auto localKeyType = localData.GetSubKeyType(subParentKey, subLocalKey);
                        if (localKeyType == JsonValueType::JVT_String) {
                            auto setData = GetTransStringFromCandidates(baseDataKeys, suffix, localData, &matchedKeyForLog);
                            if (!setData.empty()) {
                                subParentField.SetStringField(subLocalKey, setData);
                                appliedStringCount++;
                            }
                        }
                        else if (localKeyType == JsonValueType::JVT_ArrayString) {
                            auto setData = GetTransArrayStringFromCandidates(baseDataKeys, suffix, localData, &matchedKeyForLog);
                            if (!setData.empty()) {
                                subParentField.SetStringListField(subLocalKey, setData);
                                appliedArrayStringCount++;
                            }
                        }
                    }
                } break;
                case JsonValueType::JVT_ArrayObject: {
                    auto subArrField = fc.ReadObjectListField(subParentKey);
                    if (!subArrField) continue;

                    Il2cppUtils::Tools::CSListEditor<void*> subListEdit(subArrField);
                    if (!subListEdit.list_klass) {
                        Log::ErrorFmt("Failed to create CSListEditor for %s in %s", subParentKey.c_str(), tableName.c_str());
                        continue;
                    }

                    auto count = subListEdit.get_Count();
                    if (count < 0 || count > 10000) {
                        Log::ErrorFmt("Invalid count %d for %s in %s", count, subParentKey.c_str(), tableName.c_str());
                        continue;
                    }

                    for (int idx = 0; idx < count; idx++) {
                        auto currItem = subListEdit.get_Item(idx);
                        if (!currItem) continue;
                        auto currFc = FieldController::CreateSubFieldController(currItem);

                        const auto currSearchBaseKey = subParentKey + "[" + std::to_string(idx) + "].";

                        for (const auto& subLocalKey : subLocalKeys) {
                            const auto suffix = currSearchBaseKey + subLocalKey;
                            auto localKeyType = localData.GetSubKeyType(subParentKey, subLocalKey);

                            if (localKeyType == JsonValueType::JVT_String) {
                                auto setData = GetTransStringFromCandidates(baseDataKeys, suffix, localData, &matchedKeyForLog);
                                if (!setData.empty()) {
                                    currFc.SetStringField(subLocalKey, setData);
                                    appliedStringCount++;
                                }
                            }
                            else if (localKeyType == JsonValueType::JVT_ArrayString) {
                                auto setData = GetTransArrayStringFromCandidates(baseDataKeys, suffix, localData, &matchedKeyForLog);
                                if (!setData.empty()) {
                                    currFc.SetStringListField(subLocalKey, setData);
                                    appliedArrayStringCount++;
                                }
                            }
                        }
                    }

                } break;
                default:
                    break;
            }
        }

        if (Config::debugMasterDbLog) {
            static std::unordered_set<std::string> loggedAppliedTables;
            static std::unordered_set<std::string> loggedNoMatchTables;
            if (appliedStringCount > 0 || appliedArrayStringCount > 0) {
                if (loggedAppliedTables.insert(tableName).second) {
                    Log::DebugFmt("ResourceDebug[MasterDB.Translate]: table=%s result=applied key=%s string=%d array=%d",
                                  tableName.c_str(), matchedKeyForLog.c_str(), appliedStringCount, appliedArrayStringCount);
                }
            }
            else if (loggedNoMatchTables.insert(tableName).second) {
                Log::DebugFmt("ResourceDebug[MasterDB.Translate]: table=%s result=no_matching_key sampleKey=%s",
                              tableName.c_str(), JoinCandidateKeys(baseDataKeys).c_str());
            }
        }
    }

    void LocalizeMasterItem(void* item, const std::string& tableName) {
        if (!Config::useMasterTrans) return;
        // Log::DebugFmt("LocalizeMasterItem: %s", tableName.c_str());
        FieldController fc(item);
        LocalizeMasterItem(fc, tableName);
    }

} // namespace HoshimiLocal::MasterLocal
