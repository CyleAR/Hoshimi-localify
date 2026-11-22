#pragma once

#include <cstddef>
#include <string>

namespace HoshimiLocal::PrivateServer {
    // Returns true only if leafDer is a currently-valid X.509 certificate signed by one of the
    // certificates in caPem (PEM, one or more "-----BEGIN CERTIFICATE-----" blocks).
    // Thread-safe; may be called from any thread. Returns false on any failure.
    bool VerifyChainAgainstPem(const unsigned char* leafDer, std::size_t leafDerLen,
                               const std::string& caPem);
}
