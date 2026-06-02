#ifndef STRING_MATCHER_H
#define STRING_MATCHER_H

#include "fs_common.h"
#include "fs_regex.h"

typedef enum {
    MatchMode_PlainText,
    MatchMode_Wildcard,
    MatchMode_Regex
} MatchMode;

// Handles substring, wildcard, and regular expression searches
typedef struct {
    wchar_t pattern[512];
    wchar_t patternLower[512];
    wchar_t words[32][128];               // Space-separated terms for AND search
    wchar_t wordsLower[32][128];
    int wordsCount;
    MatchMode mode;
    bool caseSensitive;
    
    regex_t regex;                         // Compiled regex handle
    bool regexValid;
} StringMatcher;

void StringMatcher_Init(StringMatcher* matcher);
void StringMatcher_Free(StringMatcher* matcher);

bool StringMatcher_SetPattern(StringMatcher* matcher, const wchar_t* pattern, MatchMode mode, bool caseSensitive);
bool StringMatcher_Matches(const StringMatcher* matcher, const wchar_t* candidate);

bool StringMatcher_SubstringMatchCaseInsensitive(const wchar_t* source, const wchar_t* targetLower);
bool StringMatcher_WildcardMatch(const wchar_t* pat, const wchar_t* str, bool caseInsensitive);

#endif // STRING_MATCHER_H
