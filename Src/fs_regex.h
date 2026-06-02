#ifndef FS_REGEX_H
#define FS_REGEX_H

#include "fs_common.h"

#define MAX_REGEX_TOKENS 128

typedef enum {
    REG_UNUSED = 0,
    REG_DOT,
    REG_BEGIN,
    REG_END,
    REG_CHAR,
    REG_CHAR_CLASS,
    REG_INV_CHAR_CLASS,
    REG_DIGIT,
    REG_NOT_DIGIT,
    REG_ALPHA,
    REG_NOT_ALPHA,
    REG_WHITESPACE,
    REG_NOT_WHITESPACE
} regex_type_t;

typedef enum {
    QUANT_NONE,
    QUANT_STAR,
    QUANT_PLUS,
    QUANT_QUESTION
} quant_type_t;

typedef struct {
    regex_type_t type;
    quant_type_t quant;
    union {
        wchar_t ch;
        struct {
            wchar_t chars[128];
            int len;
        } class_val;
    } val;
} regex_token_t;

typedef struct {
    regex_token_t tokens[MAX_REGEX_TOKENS];
    int token_count;
} regex_t;

// Compiles a wide-character regex pattern string. Returns a compiled regex_t.
regex_t re_compile(const wchar_t* pattern);

// Matches the compiled regex against wide text. Returns true if match found, else false.
bool re_match(const regex_t* re, const wchar_t* text);

#endif // FS_REGEX_H
