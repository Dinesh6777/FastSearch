#include "stdafx.h"
#include "StringMatcher.h"

namespace Search {

    StringMatcher::StringMatcher() 
        : m_mode(MatchMode::PlainText), m_caseSensitive(false), m_regexValid(false) {
    }

    StringMatcher::~StringMatcher() {
    }

    // Prepare and compile the search pattern.
    // Handles invalid regex strings safely by catching std::regex_error.
    bool StringMatcher::SetPattern(const std::wstring& pattern, MatchMode mode, bool caseSensitive) {
        m_pattern = pattern;
        m_mode = mode;
        m_caseSensitive = caseSensitive;
        m_regexValid = false;

        // Cache lowercase pattern for O(N) substring comparisons
        m_patternLower.resize(pattern.size());
        std::transform(pattern.begin(), pattern.end(), m_patternLower.begin(), ::towlower);

        // Split pattern by spaces into sub-patterns for AND matching (Everything style)
        m_words.clear();
        m_wordsLower.clear();
        std::wstringstream wss(pattern);
        std::wstring word;
        while (wss >> word) {
            m_words.push_back(word);
            std::wstring wordLower = word;
            std::transform(wordLower.begin(), wordLower.end(), wordLower.begin(), ::towlower);
            m_wordsLower.push_back(wordLower);
        }

        if (m_mode == MatchMode::Regex) {
            try {
                std::regex_constants::syntax_option_type options = std::regex_constants::ECMAScript;
                if (!m_caseSensitive) {
                    options |= std::regex_constants::icase;
                }
                m_regex = std::wregex(pattern, options);
                m_regexValid = true;
                return true;
            } catch (const std::regex_error&) {
                // Gracefully handle partial/invalid user-typed regex expressions without throwing
                m_regexValid = false;
                return false;
            }
        }

        return true;
    }

    // Core matching function called in hot loops. Optimized for throughput.
    bool StringMatcher::Matches(const std::wstring& candidate) const {
        if (m_pattern.empty()) {
            return true; // Empty query matches all candidates
        }

        if (m_mode == MatchMode::PlainText) {
            if (m_wordsLower.empty()) return true;

            // All words must match (AND condition)
            std::wstring candidateLower = candidate;
            std::transform(candidate.begin(), candidate.end(), candidateLower.begin(), ::towlower);

            for (const auto& w : m_wordsLower) {
                if (candidateLower.find(w) == std::wstring::npos) {
                    return false;
                }
            }
            return true;
        } 
        else if (m_mode == MatchMode::Wildcard) {
            if (m_words.empty()) return true;

            for (const auto& w : m_words) {
                // If the word doesn't contain * or ?, wrap it in asterisks so it matches anywhere in the string
                std::wstring globPattern = w;
                if (globPattern.find_first_of(L"*?") == std::wstring::npos) {
                    globPattern = L"*" + globPattern + L"*";
                }
                if (!WildcardMatch(globPattern.c_str(), candidate.c_str(), !m_caseSensitive)) {
                    return false;
                }
            }
            return true;
        } 
        else if (m_mode == MatchMode::Regex) {
            if (m_regexValid) {
                return std::regex_search(candidate, m_regex);
            }
        }

        return false;
    }

    // High performance case-insensitive substring search
    bool StringMatcher::SubstringMatchCaseInsensitive(const std::wstring& source, const std::wstring& targetLower) {
        if (targetLower.size() > source.size()) {
            return false;
        }

        // Convert the candidate source string to lowercase.
        // Using stack allocation or in-place buffer optimization for speed.
        std::wstring sourceLower;
        sourceLower.resize(source.size());
        std::transform(source.begin(), source.end(), sourceLower.begin(), ::towlower);

        return sourceLower.find(targetLower) != std::wstring::npos;
    }

    // Classical recursive glob matching for wildcards (*, ?)
    bool StringMatcher::WildcardMatch(const wchar_t* pat, const wchar_t* str, bool caseInsensitive) {
        if (*pat == L'\0' && *str == L'\0') {
            return true;
        }

        if (*pat == L'*') {
            while (*(pat + 1) == L'*') {
                pat++; // Skip consecutive duplicate asterisks
            }
            if (*(pat + 1) == L'\0') {
                return true; // Asterisk at the end matches all trailing characters
            }
            while (*str != L'\0') {
                if (WildcardMatch(pat + 1, str, caseInsensitive)) {
                    return true;
                }
                str++;
            }
            return false;
        }

        if (*pat == L'?') {
            if (*str == L'\0') {
                return false;
            }
            return WildcardMatch(pat + 1, str + 1, caseInsensitive);
        }

        wchar_t cPat = caseInsensitive ? ::towlower(*pat) : *pat;
        wchar_t cStr = caseInsensitive ? ::towlower(*str) : *str;

        if (cPat == cStr) {
            return WildcardMatch(pat + 1, str + 1, caseInsensitive);
        }

        return false;
    }

} // namespace Search
