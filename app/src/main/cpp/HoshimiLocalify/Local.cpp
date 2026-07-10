#include "Local.h"
#include "Log.h"
#include "Plugin.h"
#include "config/Config.hpp"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <thread>
#include <regex>
#include <ranges>
#include <string>
#include <cctype>
#include <algorithm>
#include "BaseDefine.h"
#include "string_parser/StringParser.hpp"

// #include "cpprest/details/http_helpers.h"


namespace HoshimiLocal::Local {
    std::shared_mutex localDataMutex;
    std::mutex dumpDataMutex;
    std::unordered_map<std::string, std::string> i18nData{};
    std::unordered_map<std::string, std::string> genericText{};
    std::unordered_map<std::string, std::string> genericSplitText{};
    std::unordered_map<std::string, std::string> genericFmtText{};
    std::vector<std::string> genericTextDumpData{};
    std::vector<std::string> genericSplittedDumpData{};
    std::vector<std::string> genericOrigTextDumpData{};
    std::vector<std::string> genericFmtTextDumpData{};

    std::unordered_set<std::string> translatedText{};
    int genericDumpFileIndex = 0;
    const std::string splitTextPrefix = "[__split__]";

    std::filesystem::path GetBasePath() {
        return Plugin::GetInstance().GetHookInstaller()->localizationFilesDir;
    }

    bool isAllSpace(const std::string& str) {
        return std::all_of(str.begin(), str.end(), [](unsigned char c) {
            return std::isspace(c);
        });
    }

    std::string trim(const std::string& str) {
        auto is_not_space = [](char ch) { return !std::isspace(ch); };
        auto start = std::ranges::find_if(str, is_not_space);
        auto end = std::ranges::find_if(str | std::views::reverse, is_not_space).base();

        if (start < end) {
            return {start, end};
        }
        return "";
    }

    std::string findInMapIgnoreSpace(const std::string& key, const std::unordered_map<std::string, std::string>& searchMap) {
        auto is_space = [](char ch) { return std::isspace(ch); };
        auto front = std::ranges::find_if_not(key, is_space);
        auto back = std::ranges::find_if_not(key | std::views::reverse, is_space).base();

        std::string prefix(key.begin(), front);
        std::string suffix(back, key.end());

        std::string trimmedKey = trim(key);
        if ( auto it = searchMap.find(trimmedKey); it != searchMap.end()) {
            return prefix + it->second + suffix;
        }
        else {
            return "";
        }
    }

    enum class DumpStrStat {
        DEFAULT = 0,
        SPLITTABLE_ORIG = 1,
        SPLITTED = 2,
        FMT = 3
    };

    enum class SplitTagsTranslationStat {
        NO_TRANS,
        PART_TRANS,
        FULL_TRANS,
        NO_SPLIT,
        NO_SPLIT_AND_EMPTY
    };

    void InsertJsonStringDataToMap(const nlohmann::json& data, std::unordered_map<std::string, std::string>& dict,
                                   const bool insertToTranslated, const bool needCheckSplitPrefix) {
        if (!data.is_object()) return;

        for (auto& i : data.items()) {
            const auto& key = i.key();
            const auto& jsonValue = i.value();

            if (jsonValue.is_object()) {
                InsertJsonStringDataToMap(jsonValue, dict, insertToTranslated, needCheckSplitPrefix);
                continue;
            }
            if (!jsonValue.is_string()) {
                continue;
            }

            const std::string value = jsonValue;
            if (needCheckSplitPrefix && key.starts_with(splitTextPrefix) && value.starts_with(splitTextPrefix)) {
                static const auto splitTextPrefixLength = splitTextPrefix.size();
                const auto splitValue = value.substr(splitTextPrefixLength);
                genericSplitText[key.substr(splitTextPrefixLength)] = splitValue;
                if (insertToTranslated) translatedText.emplace(splitValue);
            }
            else {
                dict[key] = value;
                if (insertToTranslated) translatedText.emplace(value);
            }
        }
    }
    void LoadJsonDataToMap(const std::filesystem::path& filePath, std::unordered_map<std::string, std::string>& dict,
                           const bool insertToTranslated = false, const bool needClearDict = true,
                           const bool needCheckSplitPrefix = false) {
        if (!exists(filePath)) return;
        try {
            if (needClearDict) {
                dict.clear();
            }
            std::ifstream file(filePath);
            if (!file.is_open()) {
                Log::ErrorFmt("Load %s failed.\n", filePath.string().c_str());
                return;
            }
            std::string fileContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
            auto fileData = nlohmann::json::parse(fileContent);
            InsertJsonStringDataToMap(fileData, dict, insertToTranslated, needCheckSplitPrefix);
        }
        catch (std::exception& e) {
            Log::ErrorFmt("Load %s failed: %s\n", filePath.string().c_str(), e.what());
        }
    }

    void DumpMapDataToJson(const std::filesystem::path& dumpBasePath, const std::filesystem::path& fileName,
                           const std::unordered_map<std::string, std::string>& dict) {
        const auto dumpFilePath = dumpBasePath / fileName;
        try {
            if (!is_directory(dumpBasePath)) {
                std::filesystem::create_directories(dumpBasePath);
            }
            if (!std::filesystem::exists(dumpFilePath)) {
                std::ofstream dumpWriteLrcFile(dumpFilePath, std::ofstream::out);
                dumpWriteLrcFile << "{}";
                dumpWriteLrcFile.close();
            }

            std::ifstream dumpLrcFile(dumpFilePath);
            std::string fileContent((std::istreambuf_iterator<char>(dumpLrcFile)), std::istreambuf_iterator<char>());
            dumpLrcFile.close();
            auto fileData = nlohmann::ordered_json::parse(fileContent);
            for (const auto& i : dict) {
                fileData[i.first] = i.second;
            }
            const auto newStr = fileData.dump(4, 32, false);
            std::ofstream dumpWriteLrcFile(dumpFilePath, std::ofstream::out);
            dumpWriteLrcFile << newStr.c_str();
            dumpWriteLrcFile.close();
        }
        catch (std::exception& e) {
            Log::ErrorFmt("DumpMapDataToJson %s failed: %s", dumpFilePath.c_str(), e.what());
        }
    }

    void DumpVectorDataToJson(const std::filesystem::path& dumpBasePath, const std::filesystem::path& fileName,
                           const std::vector<std::string>& vec, const std::string& prefix = "") {
        const auto dumpFilePath = dumpBasePath / fileName;
        try {
            if (!is_directory(dumpBasePath)) {
                std::filesystem::create_directories(dumpBasePath);
            }
            if (!std::filesystem::exists(dumpFilePath)) {
                std::ofstream dumpWriteLrcFile(dumpFilePath, std::ofstream::out);
                dumpWriteLrcFile << "{}";
                dumpWriteLrcFile.close();
            }

            std::ifstream dumpLrcFile(dumpFilePath);
            std::string fileContent((std::istreambuf_iterator<char>(dumpLrcFile)), std::istreambuf_iterator<char>());
            dumpLrcFile.close();
            auto fileData = nlohmann::ordered_json::parse(fileContent);
            for (const auto& i : vec) {
                if (!prefix.empty()) {
                    fileData[prefix + i] = prefix + i;
                }
                else {
                    fileData[i] = i;
                }
            }
            const auto newStr = fileData.dump(4, 32, false);
            std::ofstream dumpWriteLrcFile(dumpFilePath, std::ofstream::out);
            dumpWriteLrcFile << newStr.c_str();
            dumpWriteLrcFile.close();
        }
        catch (std::exception& e) {
            Log::ErrorFmt("DumpVectorDataToJson %s failed: %s", dumpFilePath.c_str(), e.what());
        }
    }

    std::string to_lower(const std::string& str) {
        std::string lower_str = str;
        std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
        return lower_str;
    }

    bool IsPureStringValue(const std::string& str) {
        static std::unordered_set<char> notDeeds = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':',
                                                    '/', ' ', '.', '%', ',', '+', '-', 'x', '\n'};
        for (const auto& i : str) {
            if (!notDeeds.contains(i)) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::string> SplitByTags(const std::string& origText) {
        static const std::regex tagsRe("<.*?>(.*?)</.*?>");
        std::string text = origText;
        std::smatch match;

        std::vector<std::string> ret{};

        std::string lastSuffix;
        while (std::regex_search(text, match, tagsRe)) {
            const auto tagValue = match[1].str();
            if (IsPureStringValue(tagValue)) {
                ret.push_back(match.prefix().str());
                lastSuffix = match.suffix().str();
            }
            text = match.suffix().str();
        }
        if (!lastSuffix.empty()) {
            ret.push_back(lastSuffix);
        }

        return ret;
    }

    void ProcessGenericTextLabels() {
        std::unordered_map<std::string, std::string> appendsText{};

        for (const auto& i : genericText) {
            const auto origContents = SplitByTags(i.first);
            if (origContents.empty()) {
                continue;
            }
            const auto translatedContents = SplitByTags(i.second);
            if (origContents.size() == translatedContents.size()) {
                for (const auto& [orig, trans] : std::ranges::views::zip(origContents, translatedContents)) {
                    appendsText.emplace(orig, trans);
                }
            }
        }
        genericText.insert(appendsText.begin(), appendsText.end());
    }

    bool ReplaceString(std::string* str, const std::string& oldSubstr, const std::string& newSubstr) {
        size_t pos = str->find(oldSubstr);
        if (pos != std::string::npos) {
            str->replace(pos, oldSubstr.length(), newSubstr);
            return true;
        }
        return false;
    }

    bool GetSplitTagsTranslation(const std::string& origText, std::string* newText, std::vector<std::string>& unTransResultRet) {
        if (!origText.contains('<')) return false;
        const auto splitResult = SplitByTags(origText);
        if (splitResult.empty()) return false;

        *newText = origText;
        bool ret = true;
        for (const auto& i : splitResult) {
            if (const auto iter = genericText.find(i); iter != genericText.end()) {
                ReplaceString(newText, i, iter->second);
            }
            else {
                unTransResultRet.emplace_back(i);
                ret = false;
            }
        }
        return ret;
    }

    void ReplaceNumberComma(std::string* orig) {
        if (!orig->contains("，")) return;
        std::string newStr = *orig;
        ReplaceString(&newStr, "，", ",");
        if (IsPureStringValue(newStr)) {
            *orig = newStr;
        }
    }

    SplitTagsTranslationStat GetSplitTagsTranslationFull(const std::string& origTextIn, std::string* newText, std::vector<std::string>& unTransResultRet) {
        // static const std::u16string splitFlags = u"0123456789+＋-－%％【】.";
        static const std::unordered_set<char16_t> splitFlags = {u'0', u'1', u'2', u'3', u'4', u'5',
                                                                u'6', u'7', u'8', u'9', u'+', u'＋',
                                                                u'-', u'－', u'%', u'％', u'【', u'】',
                                                                u'.', u':', u'：', u'×'};

        const auto origText = Misc::ToUTF16(origTextIn);
        bool isInTag = false;
        std::vector<std::string> waitingReplaceTexts{};

        std::u16string currentWaitingReplaceText;

#define checkCurrentWaitingReplaceTextAndClear() \
    if (!currentWaitingReplaceText.empty()) { \
        waitingReplaceTexts.push_back(Misc::ToUTF8(currentWaitingReplaceText)); \
        currentWaitingReplaceText.clear(); }

        for (char16_t currChar : origText) {
            if (currChar == u'<') {
                isInTag = true;
            }
            if (currChar == u'>') {
                isInTag = false;
                checkCurrentWaitingReplaceTextAndClear()
                continue;
            }
            if (isInTag) {
                checkCurrentWaitingReplaceTextAndClear()
                continue;
            }

            if (!splitFlags.contains(currChar)) {
                currentWaitingReplaceText.push_back(currChar);
            }
            else {
                checkCurrentWaitingReplaceTextAndClear()
            }
        }
        if (waitingReplaceTexts.empty()) {
            if (currentWaitingReplaceText.empty()) {
                return SplitTagsTranslationStat::NO_SPLIT_AND_EMPTY;
            }
            else {
                if (!(!origText.empty() && splitFlags.contains(origText[0]))) {  // 开头为特殊符号或数字
                    return SplitTagsTranslationStat::NO_SPLIT;
                }
            }
        }
        checkCurrentWaitingReplaceTextAndClear()

        *newText = origTextIn;
        SplitTagsTranslationStat ret;
        bool hasTrans = false;
        bool hasNotTrans = false;
        if (!waitingReplaceTexts.empty()) {
            for (const auto& i : waitingReplaceTexts) {
                if (isAllSpace(i)) continue;
                std::string searchResult = findInMapIgnoreSpace(i, genericSplitText);
                if (!searchResult.empty()) {
                    ReplaceNumberComma(&searchResult);
                    ReplaceString(newText, i, searchResult);
                    hasTrans = true;
                }
                else {
                    unTransResultRet.emplace_back(trim(i));
                    hasNotTrans = true;
                }
            }
            if (hasTrans && hasNotTrans) {
                ret = SplitTagsTranslationStat::PART_TRANS;
            }
            else if (hasTrans && !hasNotTrans) {
                ret = SplitTagsTranslationStat::FULL_TRANS;
            }
            else {
                ret = SplitTagsTranslationStat::NO_TRANS;
            }
        }
        else {
            ret = SplitTagsTranslationStat::NO_TRANS;
        }
        return ret;
    }

    void LoadData() {
        std::unique_lock<std::shared_mutex> lock(localDataMutex);
        std::lock_guard<std::mutex> dumpLock(dumpDataMutex);
        static auto localizationFile = GetBasePath() / "local-files" / "localization.json";
        static auto genericFile = GetBasePath() / "local-files" / "generic.json";
        static auto genericSplitFile = GetBasePath() / "local-files" / "generic.split.json";
        static auto genericDir = GetBasePath() / "local-files" / "genericTrans";

        if (!std::filesystem::is_regular_file(localizationFile)) {
            Log::ErrorFmt("localizationFile: %s not found.", localizationFile.c_str());
            return;
        }
        LoadJsonDataToMap(localizationFile, i18nData, true);
        Log::InfoFmt("%ld localization items loaded.", i18nData.size());

        LoadJsonDataToMap(genericFile, genericText, true, true, true);
        genericSplitText.clear();
        genericFmtText.clear();
        LoadJsonDataToMap(genericSplitFile, genericSplitText, true, true, true);
        if (std::filesystem::exists(genericDir) || std::filesystem::is_directory(genericDir)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(genericDir)) {
                if (std::filesystem::is_regular_file(entry.path())) {
                    const auto& currFile = entry.path();
                    if (to_lower(currFile.extension().string()) == ".json") {
                        if (currFile.filename().string().ends_with(".split.json")) {  // split text file
                            // Split files are also loaded into genericText below for direct-match fallback.
                            LoadJsonDataToMap(currFile, genericSplitText, true, false, true);
                        }
                        if (currFile.filename().string().ends_with(".fmt.json")) {  // fmt text file
                            LoadJsonDataToMap(currFile, genericFmtText, true, false, false);
                        }
                        else {
                            LoadJsonDataToMap(currFile, genericText, true, false, true);
                        }
                    }
                }
            }
        }
        ProcessGenericTextLabels();
        Log::InfoFmt("%ld generic text items loaded.", genericText.size());
    }

    bool GetI18n(const std::string& key, std::string* ret) {
        std::shared_lock<std::shared_mutex> lock(localDataMutex);
        if (const auto iter = i18nData.find(key); iter != i18nData.end()) {
            *ret = iter->second;
            return true;
        }
        return false;
    }

    std::string readFileToString(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            throw std::exception();
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                            (std::istreambuf_iterator<char>()));
        file.close();
        return content;
    }

    std::string ReplaceParentheses(const std::string& input) {
        std::string result;
        result.reserve(static_cast<size_t>(static_cast<double>(input.size()) * 1.1));
        for (char c : input) {
            if (c == '(') {
                result.append("（");
            } else if (c == ')') {
                result.append("）");
            } else {
                result.push_back(c);
            }
        }
        return result;
    }

    //adv 파일 읽어서 한글패치
    bool GetResourceText(const std::string& name, std::string* ret) {
        static std::filesystem::path basePath = GetBasePath();

        try {
            const auto targetFilePath = basePath / "local-files" / "resource" / "adv" / name;
            // Log::DebugFmt("GetResourceText: %s", targetFilePath.c_str());
            if (exists(targetFilePath)) {
                auto readStr = readFileToString(targetFilePath.string());
                *ret = ReplaceParentheses(readStr);
                return true;
            }
        }
        catch (std::exception& e) {
            Log::ErrorFmt("read file: %s failed.", name.c_str());
        }
        return false;
    }

    bool GetResourceBytes(const std::string& name, std::vector<uint8_t>* ret) {
        static std::filesystem::path basePath = GetBasePath();

        try {
            const auto targetFilePath = basePath / "local-files" / "resource" / "img" / name;
            if (exists(targetFilePath)) {
                // Log::DebugFmt("GetResourceBytes: File found! %s", targetFilePath.string().c_str());
                std::ifstream file(targetFilePath.string(), std::ios::binary);
                if (!file.is_open()) return false;
                
                file.seekg(0, std::ios::end);
                size_t fileSize = file.tellg();
                file.seekg(0, std::ios::beg);
                
                ret->resize(fileSize);
                file.read(reinterpret_cast<char*>(ret->data()), fileSize);
                file.close();
                return true;
            }
        }
        catch (std::exception& e) {
            Log::ErrorFmt("read image file: %s failed.", name.c_str());
        }
        return false;
    }

    std::string GetDumpGenericFileName(DumpStrStat stat = DumpStrStat::DEFAULT) {
        if (stat == DumpStrStat::SPLITTABLE_ORIG) {
            if (genericDumpFileIndex == 0) return "generic_orig.json";
            return Log::StringFormat("generic_orig_%d.json", genericDumpFileIndex);
        }
        else if (stat == DumpStrStat::FMT) {
            if (genericDumpFileIndex == 0) return "generic.fmt.json";
            return Log::StringFormat("generic_%d.fmt.json", genericDumpFileIndex);
        }
        else {
            if (genericDumpFileIndex == 0) return "generic.json";
            return Log::StringFormat("generic_%d.json", genericDumpFileIndex);
        }
    }

    bool inDumpGeneric = false;
    void DumpGenericText(const std::string& origText, DumpStrStat stat = DumpStrStat::DEFAULT) {
        if (!Config::dumpText) return;
        if (!Misc::ContainsJapanese(origText)) return;
        
        {
            std::lock_guard<std::mutex> lock(dumpDataMutex);
            std::array<std::reference_wrapper<std::vector<std::string>>, 4> targets = {
                    genericTextDumpData,
                    genericOrigTextDumpData,
                    genericSplittedDumpData,
                    genericFmtTextDumpData
            };

            auto& appendTarget = targets[static_cast<int>(stat)].get();

            if (std::find(appendTarget.begin(), appendTarget.end(), origText) != appendTarget.end()) {
                return;
            }
            if (IsPureStringValue(origText)) return;

            appendTarget.push_back(origText);
        }

        static auto dumpBasePath = GetBasePath() / "dump-files";

        if (inDumpGeneric) return;
        inDumpGeneric = true;
        std::thread([](){
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::lock_guard<std::mutex> lock(dumpDataMutex);
            DumpVectorDataToJson(dumpBasePath, GetDumpGenericFileName(DumpStrStat::DEFAULT), genericTextDumpData);
            DumpVectorDataToJson(dumpBasePath, GetDumpGenericFileName(DumpStrStat::SPLITTABLE_ORIG), genericOrigTextDumpData);
            DumpVectorDataToJson(dumpBasePath, GetDumpGenericFileName(DumpStrStat::SPLITTED), genericSplittedDumpData, splitTextPrefix);
            DumpVectorDataToJson(dumpBasePath, GetDumpGenericFileName(DumpStrStat::FMT), genericFmtTextDumpData);
            genericTextDumpData.clear();
            genericSplittedDumpData.clear();
            genericOrigTextDumpData.clear();
            genericFmtTextDumpData.clear();
            inDumpGeneric = false;
        }).detach();
    }

    void DumpRemainingJapaneseText(const std::string& text) {
        DumpGenericText(text);

        auto fmtText = StringParser::ParseItems::parse(text, false);
        if (fmtText.isValid) {
            DumpGenericText(fmtText.ToFmtString(), DumpStrStat::FMT);
        }
    }

    bool GetGenericText(const std::string& origText, std::string* newStr) {
        std::shared_lock<std::shared_mutex> lock(localDataMutex);
        // 완전 일치
        if (const auto iter = genericText.find(origText); iter != genericText.end()) {
            *newStr = iter->second;
            return true;
        }
        // 번역된 텍스트 필터링
        if (translatedText.contains(origText)) {
            return false;
        }

        // 카드 레벨 필터링
        if (auto plusPos = origText.find_last_not_of('+'); plusPos != std::string::npos) {
            const auto noPlusText = origText.substr(0, plusPos + 1);

            if (const auto iter = genericText.find(noPlusText); iter != genericText.end()) {
                size_t plusCount = origText.length() - (plusPos + 1);
                *newStr = iter->second + std::string(plusCount, '+');
                return true;
            }
        }

        // fmt 텍스트
        auto fmtText = StringParser::ParseItems::parse(origText, false);
        if (fmtText.isValid) {
            const auto fmtStr = fmtText.ToFmtString();
            if (auto it = genericFmtText.find(fmtStr); it != genericFmtText.end()) {
                auto newRet = fmtText.MergeText(it->second);
                if (!newRet.empty()) {
                    *newStr = newRet;
                    return true;
                }
            }
        }

        auto ret = false;

        // 분할 매칭
        std::vector<std::string> unTransResultRet;
        const auto splitTransStat = GetSplitTagsTranslationFull(origText, newStr, unTransResultRet);
        switch (splitTransStat) {
            case SplitTagsTranslationStat::FULL_TRANS: {
                return true;
            } break;

            case SplitTagsTranslationStat::NO_SPLIT_AND_EMPTY: {
                return false;
            } break;

            case SplitTagsTranslationStat::NO_SPLIT: {
                ret = false;
            } break;

            case SplitTagsTranslationStat::NO_TRANS: {
                ret = false;
            } break;

            case SplitTagsTranslationStat::PART_TRANS: {
                ret = true;
            } break;
        }


        return ret;
    }

    std::string ChangeDumpTextIndex(int changeValue) {
        if (!Config::dumpText) return "";
        genericDumpFileIndex += changeValue;
        return Log::StringFormat("GenericDumpFile: %s", GetDumpGenericFileName().c_str());
    }

    std::string OnKeyDown(int message, int key) {
        if (message == WM_KEYDOWN) {
            switch (key) {
                case KEY_ADD: {
                    return ChangeDumpTextIndex(1);
                } break;
                case KEY_SUB: {
                    return ChangeDumpTextIndex(-1);
                } break;
            }
        }
        return "";
    }

}
