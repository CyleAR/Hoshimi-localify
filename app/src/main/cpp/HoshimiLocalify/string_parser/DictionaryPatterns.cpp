#include "DictionaryPatterns.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace HoshimiLocal::DictionaryPatterns {
    namespace {
        bool StartsWith(std::string_view text, std::string_view prefix) {
            return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
        }

        bool IsNumberPlaceholder(std::string_view name) {
            return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
                return character >= '0' && character <= '9';
            });
        }

        size_t NumberCharacterLength(std::string_view text, size_t position, bool* hasDigit) {
            const unsigned char character = text[position];
            if (character >= '0' && character <= '9') {
                *hasDigit = true;
                return 1;
            }
            if (character == '+' || character == '-' || character == '.' || character == ',' || character == '%') {
                return 1;
            }
            const auto remaining = text.substr(position);
            if (remaining.size() >= 3 && character == 0xEF &&
                static_cast<unsigned char>(remaining[1]) == 0xBC &&
                static_cast<unsigned char>(remaining[2]) >= 0x90 &&
                static_cast<unsigned char>(remaining[2]) <= 0x99) {
                *hasDigit = true;
                return 3;
            }
            for (const std::string_view symbol : {"＋", "－", "％", "，", "×"}) {
                if (StartsWith(remaining, symbol)) return symbol.size();
            }
            return 0;
        }
    }

    void Matcher::Clear() {
        patterns.clear();
        groups.clear();
    }

    void Matcher::LoadDictionary(const nlohmann::json& document) {
        Clear();
        if (!document.is_object()) return;
        for (const auto& [name, values] : document.items()) {
            if (name.empty() || name.front() == '_' || IsNumberPlaceholder(name) ||
                name.find_first_of("{}") != std::string::npos || !values.is_object()) continue;
            auto& group = groups[name];
            std::unordered_map<std::string, std::string> names;
            std::vector<std::string> translatedNames;
            for (const auto& [source, translation] : values.items()) {
                if (source.empty() || !translation.is_string()) continue;
                auto target = translation.get<std::string>();
                if (target.empty()) continue;
                translatedNames.push_back(target);
                names.emplace(source, std::move(target));
            }
            for (const auto& translatedName : translatedNames) names.try_emplace(translatedName, translatedName);
            for (auto& [source, translation] : names) {
                group[static_cast<unsigned char>(source.front())].push_back({source, std::move(translation)});
            }
            for (auto& [initial, entries] : group) {
                std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
                    if (left.source.size() != right.source.size()) return left.source.size() > right.source.size();
                    return left.source < right.source;
                });
            }
        }
    }

    bool Matcher::HasDictionaryPlaceholder(std::string_view source) const {
        size_t position = 0;
        while ((position = source.find('{', position)) != std::string_view::npos) {
            const auto end = source.find('}', position + 1);
            if (end == std::string_view::npos) return false;
            if (groups.find(std::string(source.substr(position + 1, end - position - 1))) != groups.end()) return true;
            position = end + 1;
        }
        return false;
    }

    bool Matcher::Compile(const std::string& source, const std::string& translation, Pattern* pattern) const {
        std::unordered_map<std::string, size_t> slots;
        bool hasDictionary = false;
        auto tokenize = [&](std::string_view text, bool replacement, std::vector<Token>* tokens) {
            size_t literalStart = 0;
            size_t position = 0;
            while ((position = text.find('{', position)) != std::string_view::npos) {
                const auto end = text.find('}', position + 1);
                if (end == std::string_view::npos) break;
                const std::string name(text.substr(position + 1, end - position - 1));
                const bool number = IsNumberPlaceholder(name);
                const bool dictionary = groups.find(name) != groups.end();
                if (!number && !dictionary) {
                    if (replacement && source.find("{" + name + "}") == std::string::npos) return false;
                    position = end + 1;
                    continue;
                }
                if (position > literalStart) {
                    tokens->push_back({TokenKind::Literal, std::string(text.substr(literalStart, position - literalStart))});
                }
                auto slot = slots.find(name);
                if (slot == slots.end()) {
                    if (replacement) return false;
                    slot = slots.emplace(name, slots.size()).first;
                }
                if (!replacement && dictionary) hasDictionary = true;
                const auto kind = number ? TokenKind::Number : TokenKind::Dictionary;
                if (!replacement && number && !tokens->empty() && tokens->back().kind == TokenKind::Number) return false;
                tokens->push_back({kind, name, slot->second});
                literalStart = end + 1;
                position = literalStart;
            }
            if (literalStart < text.size()) tokens->push_back({TokenKind::Literal, std::string(text.substr(literalStart))});
            return tokens->size() <= 64;
        };

        if (source.empty() || translation.empty() || !tokenize(source, false, &pattern->tokens) ||
            !hasDictionary || !tokenize(translation, true, &pattern->replacement)) return false;
        pattern->source = source;
        pattern->slotCount = slots.size();
        for (const auto& token : pattern->tokens) {
            if (token.kind != TokenKind::Literal) continue;
            pattern->literalLength += token.text.size();
            if (token.text.size() > pattern->anchor.size()) pattern->anchor = token.text;
        }
        return true;
    }

    std::vector<std::string> Matcher::SetPatterns(const std::unordered_map<std::string, std::string>& sources) {
        patterns.clear();
        std::vector<std::string> invalid;
        for (const auto& [source, translation] : sources) {
            Pattern pattern;
            if (Compile(source, translation, &pattern)) patterns.emplace_back(std::move(pattern));
            else invalid.push_back(source);
        }
        std::sort(patterns.begin(), patterns.end(), [](const Pattern& left, const Pattern& right) {
            if (left.literalLength != right.literalLength) return left.literalLength > right.literalLength;
            return left.source < right.source;
        });
        return invalid;
    }

    bool Matcher::MatchTokens(const Pattern& pattern, size_t tokenIndex, std::string_view input,
                              size_t position, std::vector<Capture>* captures, size_t* budget) const {
        if (*budget == 0) return false;
        --*budget;
        if (tokenIndex == pattern.tokens.size()) return position == input.size();
        const auto& token = pattern.tokens[tokenIndex];
        if (token.kind == TokenKind::Literal) {
            if (!StartsWith(input.substr(position), token.text)) return false;
            return MatchTokens(pattern, tokenIndex + 1, input, position + token.text.size(), captures, budget);
        }
        auto& capture = (*captures)[token.slot];
        if (!capture.source.empty()) {
            if (!StartsWith(input.substr(position), capture.source)) return false;
            return MatchTokens(pattern, tokenIndex + 1, input, position + capture.source.size(), captures, budget);
        }
        if (position == input.size()) return false;
        if (token.kind == TokenKind::Dictionary) {
            const auto& group = groups.at(token.text);
            const auto candidates = group.find(static_cast<unsigned char>(input[position]));
            if (candidates == group.end()) return false;
            for (const auto& entry : candidates->second) {
                if (*budget == 0) return false;
                --*budget;
                if (!StartsWith(input.substr(position), entry.source)) continue;
                capture = {input.substr(position, entry.source.size()), entry.translation};
                if (MatchTokens(pattern, tokenIndex + 1, input, position + entry.source.size(), captures, budget)) return true;
                capture = {};
            }
            return false;
        }

        std::vector<size_t> endings;
        bool hasDigit = false;
        size_t end = position;
        while (end < input.size()) {
            const auto length = NumberCharacterLength(input, end, &hasDigit);
            if (length == 0) break;
            end += length;
            if (hasDigit) endings.push_back(end);
        }
        for (auto ending = endings.rbegin(); ending != endings.rend(); ++ending) {
            if (*budget == 0) return false;
            capture.source = input.substr(position, *ending - position);
            capture.translation = capture.source;
            if (MatchTokens(pattern, tokenIndex + 1, input, *ending, captures, budget)) return true;
            capture = {};
        }
        return false;
    }

    bool Matcher::Match(std::string_view input, std::string* output) const {
        if (!output || input.empty() || input.size() > 16384 || patterns.empty()) return false;
        size_t budget = 8192;
        for (const auto& pattern : patterns) {
            if (budget == 0) return false;
            if (!pattern.anchor.empty() && input.find(pattern.anchor) == std::string_view::npos) continue;
            std::vector<Capture> captures(pattern.slotCount);
            if (!MatchTokens(pattern, 0, input, 0, &captures, &budget)) continue;
            std::string translated;
            translated.reserve(input.size());
            for (const auto& token : pattern.replacement) {
                if (token.kind == TokenKind::Literal) translated += token.text;
                else translated += captures[token.slot].translation;
            }
            *output = std::move(translated);
            return true;
        }
        return false;
    }
}
