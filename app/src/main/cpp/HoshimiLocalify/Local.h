#ifndef IDOLYPRIDE_LOCALIFY_LOCAL_H
#define IDOLYPRIDE_LOCALIFY_LOCAL_H

#include <string>
#include <filesystem>
#include <unordered_set>
#include <shared_mutex>
#include <mutex>

namespace HoshimiLocal::Local {
    extern std::shared_mutex localDataMutex;
    extern std::mutex dumpDataMutex;
    extern std::unordered_set<std::string> translatedText;

    std::filesystem::path GetBasePath();
    void LoadData();
    bool GetI18n(const std::string& key, std::string* ret);

    bool GetResourceText(const std::string& name, std::string* ret);
    bool GetResourceBytes(const std::string& name, std::vector<uint8_t>* ret);
    bool GetGenericText(const std::string& origText, std::string* newStr);

    std::string OnKeyDown(int message, int key);
}

#endif //IDOLYPRIDE_LOCALIFY_LOCAL_H
