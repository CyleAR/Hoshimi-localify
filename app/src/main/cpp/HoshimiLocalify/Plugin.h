#ifndef IDOLYPRIDE_LOCALIFY_PLUGIN_H
#define IDOLYPRIDE_LOCALIFY_PLUGIN_H

#include "Misc.hpp"
#include <string>
#include <memory>
#include <jni.h>

namespace HoshimiLocal {
    struct HookInstaller
    {
        virtual ~HookInstaller();
        virtual void* InstallHook(void* addr, void* hook, void** orig) = 0;
        virtual OpaqueFunctionPointer LookupSymbol(const char* name) = 0;
        virtual void* GetIl2CppLibraryHandle() const = 0;

        std::string localizationFilesDir;
    };

    class Plugin
    {
    public:
        static Plugin& GetInstance();

        void InstallHook(std::unique_ptr<HookInstaller>&& hookInstaller);

        HookInstaller* GetHookInstaller() const;

        Plugin(Plugin const&) = delete;
        Plugin& operator=(Plugin const&) = delete;

    private:
        Plugin() = default;

        std::unique_ptr<HookInstaller> m_HookInstaller;
    };

}

#endif //IDOLYPRIDE_LOCALIFY_PLUGIN_H
