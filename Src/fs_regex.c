#include "fs_regex.h"

// Helper to determine if a single character matches a token's type
static bool match_token(const regex_token_t* t, wchar_t c) {
    if (c == L'\0') return false;
    switch (t->type) {
        case REG_DOT: return true;
        case REG_CHAR: return towlower(c) == towlower(t->val.ch);
        case REG_DIGIT: return iswdigit(c) != 0;
        case REG_NOT_DIGIT: return iswdigit(c) == 0;
        case REG_ALPHA: return (iswalnum(c) || c == L'_') != 0;
        case REG_NOT_ALPHA: return !(iswalnum(c) || c == L'_');
        case REG_WHITESPACE: return iswspace(c) != 0;
        case REG_NOT_WHITESPACE: return iswspace(c) == 0;
        case REG_CHAR_CLASS: {
            for (int i = 0; i < t->val.class_val.len; i++) {
                if (towlower(c) == towlower(t->val.class_val.chars[i])) return true;
            }
            return false;
        }
        case REG_INV_CHAR_CLASS: {
            for (int i = 0; i < t->val.class_val.len; i++) {
                if (towlower(c) == towlower(t->val.class_val.chars[i])) return false;
            }
            return true;
        }
        default: return false;
    }
}

// Inner backtracking matcher
static bool match_here(const regex_token_t* tokens, int token_count, int t_idx, const wchar_t* text) {
    if (t_idx >= token_count) {
        return true; // Match successfully completed
    }

    const regex_token_t* t = &tokens[t_idx];

    if (t->type == REG_BEGIN) {
        return match_here(tokens, token_count, t_idx + 1, text);
    }

    if (t->type == REG_END) {
        return *text == L'\0';
    }

    if (t->quant == QUANT_QUESTION) {
        // Quantifier '?': try 1 match, fallback to 0 matches
        if (match_token(t, *text) && match_here(tokens, token_count, t_idx + 1, text + 1)) {
            return true;
        }
        return match_here(tokens, token_count, t_idx + 1, text);
    }

    if (t->quant == QUANT_STAR) {
        // Quantifier '*': match greedily, then backtrack
        const wchar_t* start = text;
        while (*text != L'\0' && match_token(t, *text)) {
            text++;
        }
        while (text >= start) {
            if (match_here(tokens, token_count, t_idx + 1, text)) {
                return true;
            }
            text--;
        }
        return false;
    }

    if (t->quant == QUANT_PLUS) {
        // Quantifier '+': match at least 1, then match greedily and backtrack
        if (!match_token(t, *text)) return false;
        text++;
        const wchar_t* start = text;
        while (*text != L'\0' && match_token(t, *text)) {
            text++;
        }
        while (text >= start) {
            if (match_here(tokens, token_count, t_idx + 1, text)) {
                return true;
            }
            text--;
        }
        return false;
    }

    // QUANT_NONE: match single char
    if (*text != L'\0' && match_token(t, *text)) {
        return match_here(tokens, token_count, t_idx + 1, text + 1);
    }

    return false;
}

regex_t re_compile(const wchar_t* pattern) {
    regex_t re;
    memset(&re, 0, sizeof(re));
    int i = 0;
    int t_idx = 0;

    while (pattern[i] != L'\0' && t_idx < MAX_REGEX_TOKENS) {
        regex_token_t* t = &re.tokens[t_idx];
        t->quant = QUANT_NONE;

        wchar_t c = pattern[i];
        if (c == L'^') {
            t->type = REG_BEGIN;
            i++;
        } else if (c == L'$') {
            t->type = REG_END;
            i++;
        } else if (c == L'.') {
            t->type = REG_DOT;
            i++;
        } else if (c == L'\\') {
            i++;
            wchar_t next = pattern[i];
            if (next == L'd') { t->type = REG_DIGIT; }
            else if (next == L'D') { t->type = REG_NOT_DIGIT; }
            else if (next == L'w') { t->type = REG_ALPHA; }
            else if (next == L'W') { t->type = REG_NOT_ALPHA; }
            else if (next == L's') { t->type = REG_WHITESPACE; }
            else if (next == L'S') { t->type = REG_NOT_WHITESPACE; }
            else if (next == L'\0') { 
                t->type = REG_CHAR; 
                t->val.ch = L'\\'; 
            } else { 
                t->type = REG_CHAR; 
                t->val.ch = next; 
            }
            if (pattern[i] != L'\0') i++;
        } else if (c == L'[') {
            i++;
            if (pattern[i] == L'^') {
                t->type = REG_INV_CHAR_CLASS;
                i++;
            } else {
                t->type = REG_CHAR_CLASS;
            }
            int len = 0;
            while (pattern[i] != L'\0' && pattern[i] != L']' && len < 127) {
                // Compile character ranges: [a-z]
                if (pattern[i+1] == L'-' && pattern[i+2] != L'\0' && pattern[i+2] != L']') {
                    wchar_t start = pattern[i];
                    wchar_t end = pattern[i+2];
                    for (wchar_t rc = start; rc <= end && len < 127; rc++) {
                        t->val.class_val.chars[len++] = rc;
                    }
                    i += 3;
                } else {
                    t->val.class_val.chars[len++] = pattern[i++];
                }
            }
            t->val.class_val.chars[len] = L'\0';
            t->val.class_val.len = len;
            if (pattern[i] == L']') i++;
        } else {
            t->type = REG_CHAR;
            t->val.ch = c;
            i++;
        }

        // Check for quantifiers (*, +, ?)
        if (pattern[i] == L'*') {
            t->quant = QUANT_STAR;
            i++;
        } else if (pattern[i] == L'+') {
            t->quant = QUANT_PLUS;
            i++;
        } else if (pattern[i] == L'?') {
            t->quant = QUANT_QUESTION;
            i++;
        }

        t_idx++;
    }
    re.token_count = t_idx;
    return re;
}

bool re_match(const regex_t* re, const wchar_t* text) {
    if (re->token_count > 0 && re->tokens[0].type == REG_BEGIN) {
        return match_here(re->tokens, re->token_count, 1, text);
    }

    // Attempt to match at each starting position
    do {
        if (match_here(re->tokens, re->token_count, 0, text)) {
            return true;
        }
    } while (*text++ != L'\0');

    return false;
}
