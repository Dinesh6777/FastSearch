#pragma once
#include "stdafx.h"

namespace Search {

    enum class MatchMode {
        PlainText,
        Wildcard,
        Regex
    };

    // class StringMatcher
    // Encapsulates high-performance, case-insensitive string matching logic.
    // Handles substring, wildcard (glob), and full Regular Expressions safely.
    class StringMatcher {
    public:
        StringMatcher();
        ~StringMatcher();

        // Sets the search pattern and matching mode
        bool SetPattern(const std::wstring& pattern, MatchMode mode, bool caseSensitive = false);

        // Evaluates if a candidate string matches the configured pattern
        bool Matches(const std::wstring& candidate) const;

        // Helper to perform simple case-insensitive substring checks
        static bool SubstringMatchCaseInsensitive(const std::wstring& source, const std::wstring& target);

        // Helper to perform wildcard (glob) matching (e.g. "*.txt", "test??.cpp")
        static bool WildcardMatch(const wchar_t* pat, const wchar_t* str, bool caseInsensitive = true);

        // Retrieves the space-separated search words
        const std::vector<std::wstring>& GetWordsLower() const { return m_wordsLower; }

    private:
        std::wstring m_pattern;
        std::wstring m_patternLower; // Cached lower case copy for fast O(N) substring lookups
        std::vector<std::wstring> m_words; // Space-separated search terms
        std::vector<std::wstring> m_wordsLower; // Lowercase space-separated search terms
        MatchMode m_mode;
        bool m_caseSensitive;

        // C++ Standard regex for advanced matching
        std::wregex m_regex;
        bool m_regexValid;
    };

} // namespace Search
