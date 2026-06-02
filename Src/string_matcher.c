#include "string_matcher.h"

void StringMatcher_Init(StringMatcher* matcher) {
    memset(matcher, 0, sizeof(StringMatcher));
    matcher->mode = MatchMode_PlainText;
    matcher->caseSensitive = false;
    matcher->regexValid = false;
}

void StringMatcher_Free(StringMatcher* matcher) {
    // No dynamic memory to free, but keep for future proofing
    (void)matcher;
}

bool StringMatcher_SetPattern(StringMatcher* matcher, const wchar_t* pattern, MatchMode mode, bool caseSensitive) {
    wcscpy_s(matcher->pattern, 512, pattern);
    matcher->mode = mode;
    matcher->caseSensitive = caseSensitive;
    matcher->regexValid = false;

    // Cache lowercase pattern
    size_t patLen = wcslen(pattern);
    if (patLen >= 512) patLen = 511;
    for (size_t i = 0; i < patLen; i++) {
        matcher->patternLower[i] = towlower(pattern[i]);
    }
    matcher->patternLower[patLen] = L'\0';

    // Split pattern by spaces for standard Everything-style multi-word matches
    matcher->wordsCount = 0;
    wchar_t patCopy[512];
    wcscpy_s(patCopy, 512, pattern);

    wchar_t* context = NULL;
    wchar_t* token = wcstok_s(patCopy, L" ", &context);
    while (token != NULL && matcher->wordsCount < 32) {
        wcscpy_s(matcher->words[matcher->wordsCount], 128, token);
        
        size_t tokLen = wcslen(token);
        if (tokLen >= 128) tokLen = 127;
        for (size_t i = 0; i < tokLen; i++) {
            matcher->wordsLower[matcher->wordsCount][i] = towlower(token[i]);
        }
        matcher->wordsLower[matcher->wordsCount][tokLen] = L'\0';

        matcher->wordsCount++;
        token = wcstok_s(NULL, L" ", &context);
    }

    if (mode == MatchMode_Regex) {
        // Compile regex using our lightweight wide-character compiled regex engine!
        matcher->regex = re_compile(pattern);
        matcher->regexValid = (matcher->regex.token_count > 0);
        return matcher->regexValid;
    }

    return true;
}

bool StringMatcher_Matches(const StringMatcher* matcher, const wchar_t* candidate) {
    if (matcher->pattern[0] == L'\0') {
        return true; // Empty query matches all files
    }

    if (matcher->mode == MatchMode_PlainText) {
        if (matcher->wordsCount == 0) return true;

        // All words must match (AND condition)
        wchar_t candidateLower[1024];
        size_t candLen = wcslen(candidate);
        if (candLen >= 1024) candLen = 1023;
        for (size_t i = 0; i < candLen; i++) {
            candidateLower[i] = towlower(candidate[i]);
        }
        candidateLower[candLen] = L'\0';

        for (int i = 0; i < matcher->wordsCount; i++) {
            if (wcsstr(candidateLower, matcher->wordsLower[i]) == NULL) {
                return false;
            }
        }
        return true;
    } 
    else if (matcher->mode == MatchMode_Wildcard) {
        if (matcher->wordsCount == 0) return true;

        for (int i = 0; i < matcher->wordsCount; i++) {
            wchar_t globPattern[256];
            wcscpy_s(globPattern, 256, matcher->words[i]);

            // If it does not contain wildcards, wrap it so it searches as substring
            if (wcspbrk(globPattern, L"*?") == NULL) {
                swprintf_s(globPattern, 256, L"*%s*", matcher->words[i]);
            }

            if (!StringMatcher_WildcardMatch(globPattern, candidate, !matcher->caseSensitive)) {
                return false;
            }
        }
        return true;
    } 
    else if (matcher->mode == MatchMode_Regex) {
        if (matcher->regexValid) {
            return re_match(&matcher->regex, candidate);
        }
    }

    return false;
}

bool StringMatcher_SubstringMatchCaseInsensitive(const wchar_t* source, const wchar_t* targetLower) {
    size_t srcLen = wcslen(source);
    size_t tgtLen = wcslen(targetLower);
    if (tgtLen > srcLen) {
        return false;
    }

    wchar_t* srcLower = (wchar_t*)malloc((srcLen + 1) * sizeof(wchar_t));
    if (!srcLower) return false;

    for (size_t i = 0; i < srcLen; i++) {
        srcLower[i] = towlower(source[i]);
    }
    srcLower[srcLen] = L'\0';

    bool matched = wcsstr(srcLower, targetLower) != NULL;
    free(srcLower);

    return matched;
}

bool StringMatcher_WildcardMatch(const wchar_t* pat, const wchar_t* str, bool caseInsensitive) {
    if (*pat == L'\0' && *str == L'\0') {
        return true;
    }

    if (*pat == L'*') {
        while (*(pat + 1) == L'*') {
            pat++; // Skip consecutive asterisks
        }
        if (*(pat + 1) == L'\0') {
            return true; // Terminal asterisk matches all remaining
        }
        while (*str != L'\0') {
            if (StringMatcher_WildcardMatch(pat + 1, str, caseInsensitive)) {
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
        return StringMatcher_WildcardMatch(pat + 1, str + 1, caseInsensitive);
    }

    wchar_t cPat = caseInsensitive ? towlower(*pat) : *pat;
    wchar_t cStr = caseInsensitive ? towlower(*str) : *str;

    if (cPat == cStr) {
        return StringMatcher_WildcardMatch(pat + 1, str + 1, caseInsensitive);
    }

    return false;
}
