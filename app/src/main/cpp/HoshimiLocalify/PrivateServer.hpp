#pragma once

#include <string>

namespace HoshimiLocal {
    struct HookInstaller;

    namespace PrivateServer {
        // Installs the private-server redirect hooks. No-op unless
        // Config::privateServerEnabled is true and Config::privateServerUrl is non-empty.
        void Install(HookInstaller* hookInstaller);

        // Game JWT captured from AuthLoginResponse.GameAuthToken (empty until the first login).
        std::string GetAuthToken();
    }
}
