#ifndef IDOLYPRIDE_LOCALIFY_HOOK_H
#define IDOLYPRIDE_LOCALIFY_HOOK_H

#include <string>

namespace HoshimiLocal::Hook
{
    void Install();
    std::string LocalizeNotificationText(std::string text, const std::string& userName);
}

#endif //IDOLYPRIDE_LOCALIFY_HOOK_H
