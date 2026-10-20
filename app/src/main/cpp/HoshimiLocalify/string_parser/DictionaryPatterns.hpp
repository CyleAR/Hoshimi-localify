#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HoshimiLocal::DictionaryPatterns {
    class Matcher {
    public:
        void Clear();
        void LoadDictionary(const nlohmann::json& document);
        bool HasDictionaryPlaceholder(std::string_view source) const;
        std::vector<std::string> SetPatterns(const std::unordered_map<std::string, std::string>& sources);
        bool Match(std::string_view input, std::string* output) const;

    private:
        enum class TokenKind { Literal, Dictionary, Number };

        struct Token {
            TokenKind kind;
            std::string text;
            size_t slot = 0;
        };

        struct Entry {
            std::string source;
            std::string translation;
        };

        using Group = std::unordered_map<unsigned char, std::vector<Entry>>;

        struct Pattern {
            std::string source;
            std::vector<Token> tokens;
            std::vector<Token> replacement;
            size_t slotCount = 0;
            size_t literalLength = 0;
            std::string anchor;
        };

        struct Capture {
            std::string_view source;
            std::string_view translation;
        };

        bool Compile(const std::string& source, const std::string& translation, Pattern* pattern) const;
        bool MatchTokens(const Pattern& pattern, size_t tokenIndex, std::string_view input,
                         size_t position, std::vector<Capture>* captures, size_t* budget) const;

        std::unordered_map<std::string, Group> groups;
        std::vector<Pattern> patterns;
    };
}
