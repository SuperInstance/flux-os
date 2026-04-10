/*
 * FLUX OS — FLUX.MD Lexer
 *
 * Tokenizes FLUX.MD markdown format into tokens for the parser.
 * Handles markdown headers (#, ##, ###), code fences (```), lists (-, *),
 * metadata (key: value), inline code (`code`), identifiers, numbers,
 * strings, and operators.
 *
 * Each token carries source location (line, column) for error reporting.
 */

#include "flux/compiler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ========================================================================
 * Token Types
 * ======================================================================== */

typedef enum {
    /* FLUX.MD structural tokens */
    TOK_HEADING       = 1,   /* # heading */
    TOK_TEXT          = 2,   /* Plain text / paragraph content */
    TOK_CODE_BLOCK    = 3,   /* ```lang ... ``` fenced code block */
    TOK_INLINE_CODE   = 4,   /* `code` inline code */
    TOK_LIST_ITEM     = 5,   /* - or * list item */
    TOK_META_KEY      = 6,   /* metadata key (before colon) */
    TOK_META_VALUE    = 7,   /* metadata value (after colon) */
    TOK_NEWLINE       = 8,   /* \n or \r\n */
    TOK_EOF           = 9,   /* End of input */

    /* Programming language tokens */
    TOK_IDENTIFIER    = 10,  /* variable / function name */
    TOK_NUMBER        = 11,  /* integer or float literal */
    TOK_STRING        = 12,  /* "string" or 'string' */
    TOK_OPERATOR      = 13,  /* +, -, *, /, ==, !=, etc. */
    TOK_PUNCTUATION   = 14,  /* (, ), {, }, [, ], ;, ,, : */
    TOK_KEYWORD       = 15,  /* if, else, while, for, fn, let, return, etc. */
    TOK_TYPE          = 16,  /* i32, i64, f32, f64, void, ptr, etc. */
    TOK_COMMENT       = 17,  /* // or /* */ */
    TOK_DIRECTIVE     = 18,  /* #!agent, #!tile, etc. FLUX directives */
} flux_token_type_t;

/* ========================================================================
 * Token Structure
 * ======================================================================== */

#define FLUX_TOKEN_TEXT_MAX 2048

typedef struct {
    flux_token_type_t type;
    int               line;
    int               col;
    int               length;           /* Byte length of token text */
    char              text[FLUX_TOKEN_TEXT_MAX];
    /* Extra data for specific token types */
    int               heading_level;    /* For TOK_HEADING: 1-6 */
    char              lang[64];         /* For TOK_CODE_BLOCK: language tag */
    int               int_val;          /* For TOK_NUMBER: integer value */
    double            float_val;        /* For TOK_NUMBER: float value */
} flux_token_t;

/* ========================================================================
 * Lexer Structure
 * ======================================================================== */

typedef struct {
    const char *source;       /* Source buffer (not owned) */
    flux_size_t source_len;   /* Source length */
    flux_size_t pos;          /* Current position */
    int         line;         /* Current line (1-indexed) */
    int         col;          /* Current column (1-indexed) */
    int         paren_depth;  /* Nesting depth for ( ) [ ] { } */
    bool        in_code_fence;/* Inside a code fence block */
    char        fence_lang[64]; /* Current fence language tag */
    bool        in_frontmatter; /* Inside YAML frontmatter */
} flux_lexer_t;

/* ========================================================================
 * FLUX.MD Keywords
 * ======================================================================== */

static const char *flux_keywords[] = {
    "fn", "let", "var", "const", "if", "else", "elif",
    "while", "for", "loop", "break", "continue", "return",
    "import", "export", "module", "struct", "enum", "union",
    "type", "trait", "impl", "agent", "tile", "region",
    "spawn", "tell", "ask", "delegate", "barrier",
    "true", "false", "nil", "null", "void",
    "i8", "i16", "i32", "i64", "f32", "f64",
    "bool", "ptr", "str", "u8", "u16", "u32", "u64",
    "capability", "bytecode",
    NULL
};

/* FIR type keywords (recognized as TOK_TYPE) */
static const char *flux_type_keywords[] = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool", "ptr", "str", "void",
    "capability", "bytecode", "region",
    "agent", "struct", "enum", "union",
    "func", "array",
    NULL
};

/* ========================================================================
 * Forward Declarations
 * ======================================================================== */

static void   flux_lexer_skip_whitespace_and_comments(flux_lexer_t *lex);
static void   flux_lexer_skip_line_comment(flux_lexer_t *lex);
static void   flux_lexer_skip_block_comment(flux_lexer_t *lex);
static bool   flux_lexer_is_keyword(const char *word);
static bool   flux_lexer_is_type_keyword(const char *word);
static char   flux_lexer_peek_char(flux_lexer_t *lex);
static char   flux_lexer_next_char(flux_lexer_t *lex);
static bool   flux_lexer_match_str(flux_lexer_t *lex, const char *s);

/* ========================================================================
 * Helper Functions
 * ======================================================================== */

static char flux_lexer_peek_char(flux_lexer_t *lex) {
    if (lex->pos >= lex->source_len) return '\0';
    return lex->source[lex->pos];
}

static char flux_lexer_next_char(flux_lexer_t *lex) {
    char c = '\0';
    if (lex->pos < lex->source_len) {
        c = lex->source[lex->pos];
        lex->pos++;
        if (c == '\n') {
            lex->line++;
            lex->col = 1;
        } else {
            lex->col++;
        }
    }
    return c;
}

static bool flux_lexer_match_str(flux_lexer_t *lex, const char *s) {
    flux_size_t i = 0;
    while (s[i] != '\0') {
        if (lex->pos + i >= lex->source_len) return false;
        if (lex->source[lex->pos + i] != s[i]) return false;
        i++;
    }
    return true;
}

static bool flux_lexer_is_keyword(const char *word) {
    for (int i = 0; flux_keywords[i] != NULL; i++) {
        if (strcmp(word, flux_keywords[i]) == 0) return true;
    }
    return false;
}

static bool flux_lexer_is_type_keyword(const char *word) {
    for (int i = 0; flux_type_keywords[i] != NULL; i++) {
        if (strcmp(word, flux_type_keywords[i]) == 0) return true;
    }
    return false;
}

/* Skip whitespace (but NOT newlines — those are tokens) */
static void flux_lexer_skip_whitespace_and_comments(flux_lexer_t *lex) {
    while (lex->pos < lex->source_len) {
        char c = lex->source[lex->pos];
        if (c == ' ' || c == '\t') {
            lex->pos++;
            lex->col++;
        } else if (c == '/' && lex->pos + 1 < lex->source_len) {
            if (lex->source[lex->pos + 1] == '/') {
                flux_lexer_skip_line_comment(lex);
            } else if (lex->source[lex->pos + 1] == '*') {
                flux_lexer_skip_block_comment(lex);
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

static void flux_lexer_skip_line_comment(flux_lexer_t *lex) {
    /* Skip // until end of line */
    while (lex->pos < lex->source_len && lex->source[lex->pos] != '\n') {
        lex->pos++;
        lex->col++;
    }
}

static void flux_lexer_skip_block_comment(flux_lexer_t *lex) {
    /* Skip /* ... */ */
    lex->pos += 2; /* skip opening /* */
    lex->col += 2;
    int depth = 1;
    while (lex->pos < lex->source_len - 1 && depth > 0) {
        if (lex->source[lex->pos] == '/' && lex->source[lex->pos + 1] == '*') {
            depth++;
            lex->pos += 2;
            lex->col += 2;
        } else if (lex->source[lex->pos] == '*' && lex->source[lex->pos + 1] == '/') {
            depth--;
            lex->pos += 2;
            lex->col += 2;
        } else {
            if (lex->source[lex->pos] == '\n') {
                lex->line++;
                lex->col = 1;
            } else {
                lex->col++;
            }
            lex->pos++;
        }
    }
}

/* ========================================================================
 * Token Creation
 * ======================================================================== */

static void flux_token_init(flux_token_t *tok) {
    memset(tok, 0, sizeof(flux_token_t));
    tok->type = TOK_EOF;
    tok->line = 1;
    tok->col = 1;
}

static void flux_token_set_text(flux_token_t *tok, const char *start, int len) {
    if (len <= 0) len = 0;
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
}

/* ========================================================================
 * Lexer Initialization
 * ======================================================================== */

void flux_lexer_init(void *lexer, const char *source, flux_size_t len) {
    flux_lexer_t *lex = (flux_lexer_t *)lexer;
    if (!lex) return;
    memset(lex, 0, sizeof(flux_lexer_t));
    lex->source = source ? source : "";
    lex->source_len = len;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;

    /* Check for YAML frontmatter (---) */
    if (len >= 3 && source[0] == '-' && source[1] == '-' && source[2] == '-') {
        lex->in_frontmatter = true;
        lex->pos = 3;
        lex->col = 4;
        /* Skip until closing --- */
        while (lex->pos < lex->source_len - 2) {
            if (lex->source[lex->pos] == '-' &&
                lex->source[lex->pos + 1] == '-' &&
                lex->source[lex->pos + 2] == '-') {
                lex->pos += 3;
                lex->col += 3;
                /* Skip the newline after closing --- */
                if (lex->pos < lex->source_len && lex->source[lex->pos] == '\n') {
                    lex->pos++;
                    lex->line++;
                    lex->col = 1;
                }
                break;
            }
            if (lex->source[lex->pos] == '\n') {
                lex->line++;
                lex->col = 1;
            } else {
                lex->col++;
            }
            lex->pos++;
        }
        lex->in_frontmatter = false;
    }
}

/* ========================================================================
 * Token Scanning — High-Level Structure
 * ======================================================================== */

/* Scan a newline token */
static void flux_lexer_scan_newline(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    char c = flux_lexer_next_char(lex);
    /* Handle \r\n */
    if (c == '\r' && lex->pos < lex->source_len && lex->source[lex->pos] == '\n') {
        flux_lexer_next_char(lex);
    }

    tok->type = TOK_NEWLINE;
    tok->line = start_line;
    tok->col = start_col;
    flux_token_set_text(tok, lex->source + start, (int)(lex->pos - start));
}

/* Scan a markdown heading (# ## ### etc.) */
static void flux_lexer_scan_heading(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    int level = 0;
    while (lex->pos < lex->source_len && lex->source[lex->pos] == '#') {
        level++;
        lex->pos++;
        lex->col++;
    }
    if (level > 6) level = 6;

    /* Skip optional space after # */
    if (lex->pos < lex->source_len && lex->source[lex->pos] == ' ') {
        lex->pos++;
        lex->col++;
    }

    /* Read heading text until newline */
    flux_size_t text_start = lex->pos;
    while (lex->pos < lex->source_len && lex->source[lex->pos] != '\n') {
        lex->pos++;
        lex->col++;
    }

    tok->type = TOK_HEADING;
    tok->line = start_line;
    tok->col = start_col;
    tok->heading_level = level;
    /* Build token text including the #'s */
    int total_len = (int)(lex->pos - start);
    if (total_len >= FLUX_TOKEN_TEXT_MAX) total_len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)total_len);
    tok->text[total_len] = '\0';
    tok->length = total_len;
}

/* Scan a fenced code block ```lang ... ``` */
static void flux_lexer_scan_code_block(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    /* Skip opening ``` */
    lex->pos += 3;
    lex->col += 3;

    /* Read language tag */
    memset(tok->lang, 0, sizeof(tok->lang));
    int lang_len = 0;
    while (lex->pos < lex->source_len &&
           lex->source[lex->pos] != '\n' &&
           lex->source[lex->pos] != ' ' &&
           lang_len < 63) {
        tok->lang[lang_len++] = lex->source[lex->pos++];
        lex->col++;
    }
    tok->lang[lang_len] = '\0';

    /* Skip to end of opening line */
    while (lex->pos < lex->source_len && lex->source[lex->pos] != '\n') {
        lex->pos++;
        lex->col++;
    }
    if (lex->pos < lex->source_len) {
        lex->pos++; /* skip \n */
        lex->line++;
        lex->col = 1;
    }

    /* Read code body until closing ``` */
    flux_size_t body_start = lex->pos;
    flux_size_t body_len = 0;
    while (lex->pos < lex->source_len) {
        /* Check for closing ``` */
        if (lex->source[lex->pos] == '`' &&
            lex->pos + 2 < lex->source_len &&
            lex->source[lex->pos + 1] == '`' &&
            lex->source[lex->pos + 2] == '`') {
            break;
        }
        if (lex->source[lex->pos] == '\n') {
            lex->line++;
            lex->col = 1;
        } else {
            lex->col++;
        }
        lex->pos++;
        body_len++;
    }

    /* Build the token text (just the code body) */
    tok->type = TOK_CODE_BLOCK;
    tok->line = start_line;
    tok->col = start_col;
    int copy_len = (int)body_len;
    if (copy_len >= FLUX_TOKEN_TEXT_MAX) copy_len = FLUX_TOKEN_TEXT_MAX - 1;
    if (body_len > 0) {
        memcpy(tok->text, lex->source + body_start, (size_t)copy_len);
    }
    tok->text[copy_len] = '\0';
    tok->length = copy_len;

    /* Skip closing ``` */
    if (lex->pos < lex->source_len) {
        lex->pos += 3;
        lex->col += 3;
        /* Skip trailing newline */
        if (lex->pos < lex->source_len && lex->source[lex->pos] == '\n') {
            lex->pos++;
            lex->line++;
            lex->col = 1;
        }
    }
}

/* Scan inline code `code` */
static void flux_lexer_scan_inline_code(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;

    lex->pos++; /* skip opening ` */
    lex->col++;

    flux_size_t text_start = lex->pos;
    while (lex->pos < lex->source_len && lex->source[lex->pos] != '`') {
        if (lex->source[lex->pos] == '\n') {
            lex->line++;
            lex->col = 1;
        } else {
            lex->col++;
        }
        lex->pos++;
    }

    tok->type = TOK_INLINE_CODE;
    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - text_start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + text_start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;

    if (lex->pos < lex->source_len) {
        lex->pos++; /* skip closing ` */
        lex->col++;
    }
}

/* Scan a list item (- item or * item) */
static void flux_lexer_scan_list_item(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    char marker = lex->source[lex->pos]; /* - or * */
    lex->pos++;
    lex->col++;

    /* Skip space after marker */
    if (lex->pos < lex->source_len && lex->source[lex->pos] == ' ') {
        lex->pos++;
        lex->col++;
    }

    /* Read list item text until newline */
    while (lex->pos < lex->source_len && lex->source[lex->pos] != '\n') {
        lex->pos++;
        lex->col++;
    }

    tok->type = TOK_LIST_ITEM;
    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
}

/* Scan a metadata line (key: value) */
static void flux_lexer_scan_metadata(flux_lexer_t *lex, flux_token_t *tok,
                                     bool emit_key) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    if (emit_key) {
        /* Scan key part (identifier before colon) */
        while (lex->pos < lex->source_len &&
               (isalnum((unsigned char)lex->source[lex->pos]) ||
                lex->source[lex->pos] == '_' ||
                lex->source[lex->pos] == '-')) {
            lex->pos++;
            lex->col++;
        }
        tok->type = TOK_META_KEY;
    } else {
        /* Skip the colon and optional space */
        lex->pos++; /* skip : */
        lex->col++;
        if (lex->pos < lex->source_len && lex->source[lex->pos] == ' ') {
            lex->pos++;
            lex->col++;
        }

        /* Read value until newline */
        while (lex->pos < lex->source_len && lex->source[lex->pos] != '\n') {
            lex->pos++;
            lex->col++;
        }
        tok->type = TOK_META_VALUE;
    }

    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
}

/* Scan an identifier or keyword */
static void flux_lexer_scan_identifier(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    while (lex->pos < lex->source_len &&
           (isalnum((unsigned char)lex->source[lex->pos]) ||
            lex->source[lex->pos] == '_' ||
            lex->source[lex->pos] == '-')) {
        lex->pos++;
        lex->col++;
    }

    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
    tok->line = start_line;
    tok->col = start_col;

    /* Classify */
    if (flux_lexer_is_type_keyword(tok->text)) {
        tok->type = TOK_TYPE;
    } else if (flux_lexer_is_keyword(tok->text)) {
        tok->type = TOK_KEYWORD;
    } else {
        tok->type = TOK_IDENTIFIER;
    }
}

/* Scan a number literal (integer or float) */
static void flux_lexer_scan_number(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;
    bool is_float = false;

    /* Handle hex prefix */
    if (lex->pos + 1 < lex->source_len &&
        lex->source[lex->pos] == '0' &&
        (lex->source[lex->pos + 1] == 'x' || lex->source[lex->pos + 1] == 'X')) {
        lex->pos += 2;
        lex->col += 2;
        while (lex->pos < lex->source_len &&
               (isxdigit((unsigned char)lex->source[lex->pos]) ||
                lex->source[lex->pos] == '_')) {
            lex->pos++;
            lex->col++;
        }
    } else {
        /* Decimal digits */
        while (lex->pos < lex->source_len &&
               isdigit((unsigned char)lex->source[lex->pos])) {
            lex->pos++;
            lex->col++;
        }

        /* Fractional part */
        if (lex->pos < lex->source_len && lex->source[lex->pos] == '.') {
            is_float = true;
            lex->pos++;
            lex->col++;
            while (lex->pos < lex->source_len &&
                   isdigit((unsigned char)lex->source[lex->pos])) {
                lex->pos++;
                lex->col++;
            }
        }

        /* Exponent */
        if (lex->pos < lex->source_len &&
            (lex->source[lex->pos] == 'e' || lex->source[lex->pos] == 'E')) {
            is_float = true;
            lex->pos++;
            lex->col++;
            if (lex->pos < lex->source_len &&
                (lex->source[lex->pos] == '+' || lex->source[lex->pos] == '-')) {
                lex->pos++;
                lex->col++;
            }
            while (lex->pos < lex->source_len &&
                   isdigit((unsigned char)lex->source[lex->pos])) {
                lex->pos++;
                lex->col++;
            }
        }
    }

    /* Type suffix (i32, f64, etc.) */
    if (lex->pos < lex->source_len && (lex->source[lex->pos] == 'i' ||
        lex->source[lex->pos] == 'f' || lex->source[lex->pos] == 'u')) {
        lex->pos++;
        lex->col++;
        while (lex->pos < lex->source_len &&
               isdigit((unsigned char)lex->source[lex->pos])) {
            lex->pos++;
            lex->col++;
        }
    }

    tok->type = TOK_NUMBER;
    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;

    /* Parse value */
    if (is_float) {
        tok->float_val = strtod(tok->text, NULL);
        tok->int_val = (int)tok->float_val;
    } else {
        tok->int_val = (int)strtol(tok->text, NULL, 0);
        tok->float_val = (double)tok->int_val;
    }
}

/* Scan a string literal */
static void flux_lexer_scan_string(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    char quote = lex->source[lex->pos]; /* " or ' */
    lex->pos++;
    lex->col++;

    int text_pos = 0;
    while (lex->pos < lex->source_len && lex->source[lex->pos] != quote &&
           text_pos < FLUX_TOKEN_TEXT_MAX - 1) {
        if (lex->source[lex->pos] == '\\' && lex->pos + 1 < lex->source_len) {
            /* Escape sequence */
            lex->pos++;
            lex->col++;
            switch (lex->source[lex->pos]) {
                case 'n':  tok->text[text_pos++] = '\n'; break;
                case 't':  tok->text[text_pos++] = '\t'; break;
                case 'r':  tok->text[text_pos++] = '\r'; break;
                case '0':  tok->text[text_pos++] = '\0'; break;
                case '\\': tok->text[text_pos++] = '\\'; break;
                case '\'': tok->text[text_pos++] = '\''; break;
                case '"':  tok->text[text_pos++] = '"';  break;
                default:
                    if (lex->source[lex->pos] >= '0' && lex->source[lex->pos] <= '7') {
                        /* Octal escape */
                        int octal = 0;
                        int digits = 0;
                        while (digits < 3 && lex->pos < lex->source_len &&
                               lex->source[lex->pos] >= '0' &&
                               lex->source[lex->pos] <= '7') {
                            octal = octal * 8 + (lex->source[lex->pos] - '0');
                            lex->pos++;
                            lex->col++;
                            digits++;
                        }
                        tok->text[text_pos++] = (char)(octal & 0xFF);
                        continue; /* Don't advance pos/col again */
                    }
                    tok->text[text_pos++] = lex->source[lex->pos];
                    break;
            }
            lex->pos++;
            lex->col++;
        } else {
            if (lex->source[lex->pos] == '\n') {
                lex->line++;
                lex->col = 1;
            } else {
                lex->col++;
            }
            tok->text[text_pos++] = lex->source[lex->pos++];
        }
    }
    tok->text[text_pos] = '\0';

    if (lex->pos < lex->source_len) {
        lex->pos++; /* skip closing quote */
        lex->col++;
    }

    tok->type = TOK_STRING;
    tok->line = start_line;
    tok->col = start_col;
    tok->length = text_pos;
}

/* Scan an operator */
static void flux_lexer_scan_operator(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    char c = lex->source[lex->pos];
    lex->pos++;
    lex->col++;

    /* Two-character operators */
    if (lex->pos < lex->source_len) {
        char c2 = lex->source[lex->pos];
        char pair[3] = { c, c2, '\0' };

        /* Check for valid two-char operators */
        if ((c == '=' && (c2 == '=' || c2 == '>')) ||
            (c == '!' && c2 == '=') ||
            (c == '<' && (c2 == '=' || c2 == '<')) ||
            (c == '>' && (c2 == '=' || c2 == '>')) ||
            (c == '&' && c2 == '&') ||
            (c == '|' && c2 == '|') ||
            (c == '-' && c2 == '>') ||
            (c == '+' && (c2 == '=' || c2 == '+')) ||
            (c == '-' && (c2 == '=' || c2 == '-')) ||
            (c == '*' && (c2 == '=' || c2 == '*')) ||
            (c == '/' && c2 == '=') ||
            (c == '%' && c2 == '=') ||
            (c == '^' && c2 == '=') ||
            (c == ':' && c2 == ':') ||
            (c == '.' && c2 == '.')) {
            lex->pos++;
            lex->col++;
            /* Check for three-char operators (e.g., ===, <<=, >>=) */
            if (lex->pos < lex->source_len) {
                char c3 = lex->source[lex->pos];
                if ((c == '=' && c2 == '=' && c3 == '=') ||
                    (c == '<' && c2 == '<' && c3 == '=') ||
                    (c == '>' && c2 == '>' && c3 == '=') ||
                    (c == '.' && c2 == '.' && c3 == '.')) {
                    lex->pos++;
                    lex->col++;
                }
            }
        }
    }

    tok->type = TOK_OPERATOR;
    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
}

/* Scan punctuation */
static void flux_lexer_scan_punctuation(flux_lexer_t *lex, flux_token_t *tok) {
    tok->type = TOK_PUNCTUATION;
    tok->line = lex->line;
    tok->col = lex->col;
    tok->text[0] = lex->source[lex->pos];
    tok->text[1] = '\0';
    tok->length = 1;
    lex->pos++;
    lex->col++;

    /* Track parenthesis depth */
    if (tok->text[0] == '(' || tok->text[0] == '[' || tok->text[0] == '{') {
        lex->paren_depth++;
    } else if (tok->text[0] == ')' || tok->text[0] == ']' || tok->text[0] == '}') {
        if (lex->paren_depth > 0) lex->paren_depth--;
    }
}

/* Scan a FLUX directive (#!name) */
static void flux_lexer_scan_directive(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    /* Skip #! */
    lex->pos += 2;
    lex->col += 2;

    /* Read directive name */
    while (lex->pos < lex->source_len &&
           (isalnum((unsigned char)lex->source[lex->pos]) ||
            lex->source[lex->pos] == '_' ||
            lex->source[lex->pos] == '-')) {
        lex->pos++;
        lex->col++;
    }

    tok->type = TOK_DIRECTIVE;
    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
}

/* Scan plain text (paragraph content in markdown context) */
static void flux_lexer_scan_text(flux_lexer_t *lex, flux_token_t *tok) {
    int start_line = lex->line;
    int start_col = lex->col;
    flux_size_t start = lex->pos;

    /* Read until we hit a structural markdown character or newline */
    while (lex->pos < lex->source_len) {
        char c = lex->source[lex->pos];
        if (c == '\n' || c == '#' || c == '`' || c == '|' ||
            c == '<' || c == '>' || c == '[' || c == ']') {
            break;
        }
        if (c == '-' || c == '*') {
            /* List marker if at start of line after space */
            if (lex->col == 1 ||
                (lex->col > 1 && lex->source[lex->pos - 1] == ' ')) {
                break;
            }
        }
        lex->pos++;
        lex->col++;
    }

    tok->type = TOK_TEXT;
    tok->line = start_line;
    tok->col = start_col;
    int len = (int)(lex->pos - start);
    if (len >= FLUX_TOKEN_TEXT_MAX) len = FLUX_TOKEN_TEXT_MAX - 1;
    memcpy(tok->text, lex->source + start, (size_t)len);
    tok->text[len] = '\0';
    tok->length = len;
}

/* ========================================================================
 * Detect if a line is metadata (key: value format)
 * ======================================================================== */

static bool flux_lexer_is_metadata_line(flux_lexer_t *lex) {
    if (lex->pos >= lex->source_len) return false;

    /* Look ahead to find colon on this line */
    flux_size_t i = lex->pos;
    bool has_alpha = false;
    while (i < lex->source_len && lex->source[i] != '\n') {
        if (isalnum((unsigned char)lex->source[i]) || lex->source[i] == '_') {
            has_alpha = true;
        } else if (lex->source[i] == ':' && has_alpha) {
            /* Check: colon followed by space and non-newline */
            if (i + 1 < lex->source_len &&
                (lex->source[i + 1] == ' ' || lex->source[i + 1] == '\t')) {
                return true;
            }
        } else if (!has_alpha && (lex->source[i] == ' ' || lex->source[i] == '\t')) {
            /* Skip leading spaces */
        } else if (lex->source[i] != '-' && lex->source[i] != '_') {
            return false;
        }
        i++;
    }
    return false;
}

/* ========================================================================
 * Main Tokenization Entry Point
 * ======================================================================== */

static void flux_lexer_scan_next(flux_lexer_t *lex, flux_token_t *tok) {
    flux_token_init(tok);

    /* Skip whitespace and comments (preserving newlines) */
    flux_lexer_skip_whitespace_and_comments(lex);

    if (lex->pos >= lex->source_len) {
        tok->type = TOK_EOF;
        tok->line = lex->line;
        tok->col = lex->col;
        return;
    }

    char c = lex->source[lex->pos];

    /* Newlines */
    if (c == '\n' || c == '\r') {
        flux_lexer_scan_newline(lex, tok);
        return;
    }

    /* Markdown headings */
    if (c == '#' && lex->col == 1) {
        /* Check it's not a directive */
        if (lex->pos + 1 < lex->source_len && lex->source[lex->pos + 1] == '!') {
            flux_lexer_scan_directive(lex, tok);
            return;
        }
        flux_lexer_scan_heading(lex, tok);
        return;
    }

    /* Code fences ``` */
    if (c == '`' && lex->pos + 2 < lex->source_len &&
        lex->source[lex->pos + 1] == '`' && lex->source[lex->pos + 2] == '`') {
        flux_lexer_scan_code_block(lex, tok);
        return;
    }

    /* Inline code `code` */
    if (c == '`' && (lex->pos + 1 >= lex->source_len ||
                     lex->source[lex->pos + 1] != '`')) {
        flux_lexer_scan_inline_code(lex, tok);
        return;
    }

    /* FLUX directive #!name */
    if (c == '#' && lex->pos + 1 < lex->source_len && lex->source[lex->pos + 1] == '!') {
        flux_lexer_scan_directive(lex, tok);
        return;
    }

    /* List markers at line start (after optional space) */
    if ((c == '-' || c == '*') && lex->col <= 3) {
        /* Check if followed by space (list marker) */
        if (lex->pos + 1 < lex->source_len && lex->source[lex->pos + 1] == ' ') {
            flux_lexer_scan_list_item(lex, tok);
            return;
        }
    }

    /* Metadata lines (key: value) at non-indented position */
    if (lex->col <= 4 && flux_lexer_is_metadata_line(lex)) {
        flux_lexer_scan_metadata(lex, tok, true);
        return;
    }

    /* String literals */
    if (c == '"' || c == '\'') {
        flux_lexer_scan_string(lex, tok);
        return;
    }

    /* Numbers */
    if (isdigit((unsigned char)c) ||
        (c == '.' && lex->pos + 1 < lex->source_len &&
         isdigit((unsigned char)lex->source[lex->pos + 1]))) {
        flux_lexer_scan_number(lex, tok);
        return;
    }

    /* Identifiers and keywords */
    if (isalpha((unsigned char)c) || c == '_') {
        flux_lexer_scan_identifier(lex, tok);
        return;
    }

    /* Operators */
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '=' || c == '!' || c == '<' || c == '>' ||
        c == '&' || c == '|' || c == '^' || c == '~' ||
        c == '?' || c == ':') {
        flux_lexer_scan_operator(lex, tok);
        return;
    }

    /* Punctuation */
    if (c == '(' || c == ')' || c == '{' || c == '}' ||
        c == '[' || c == ']' || c == ';' || c == ',' ||
        c == '.') {
        flux_lexer_scan_punctuation(lex, tok);
        return;
    }

    /* Fallback: treat as text */
    flux_lexer_scan_text(lex, tok);
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/* Size of lexer state for external allocation */
size_t flux_lexer_size(void) {
    return sizeof(flux_lexer_t);
}

/* Get next token from lexer */
void flux_lexer_next(void *lexer, void *token) {
    flux_lexer_t *lex = (flux_lexer_t *)lexer;
    flux_token_t *tok = (flux_token_t *)token;
    if (!lex || !tok) return;
    flux_lexer_scan_next(lex, tok);
}

/* Peek at next token without consuming */
void flux_lexer_peek(void *lexer, void *token) {
    flux_lexer_t *lex = (flux_lexer_t *)lexer;
    flux_token_t *tok = (flux_token_t *)token;
    if (!lex || !tok) return;
    /* Save state */
    flux_size_t saved_pos = lex->pos;
    int saved_line = lex->line;
    int saved_col = lex->col;
    int saved_paren = lex->paren_depth;
    /* Scan one token */
    flux_lexer_scan_next(lex, tok);
    /* Restore state */
    lex->pos = saved_pos;
    lex->line = saved_line;
    lex->col = saved_col;
    lex->paren_depth = saved_paren;
}

/* Get the current lexer line position */
int flux_lexer_line(void *lexer) {
    return ((flux_lexer_t *)lexer)->line;
}

/* Get the current lexer column position */
int flux_lexer_column(void *lexer) {
    return ((flux_lexer_t *)lexer)->col;
}

/* Get token type name (for debugging / error messages) */
const char *flux_token_type_name(int type) {
    switch (type) {
        case TOK_HEADING:     return "HEADING";
        case TOK_TEXT:        return "TEXT";
        case TOK_CODE_BLOCK:  return "CODE_BLOCK";
        case TOK_INLINE_CODE: return "INLINE_CODE";
        case TOK_LIST_ITEM:   return "LIST_ITEM";
        case TOK_META_KEY:    return "META_KEY";
        case TOK_META_VALUE:  return "META_VALUE";
        case TOK_NEWLINE:     return "NEWLINE";
        case TOK_EOF:         return "EOF";
        case TOK_IDENTIFIER:  return "IDENTIFIER";
        case TOK_NUMBER:      return "NUMBER";
        case TOK_STRING:      return "STRING";
        case TOK_OPERATOR:    return "OPERATOR";
        case TOK_PUNCTUATION: return "PUNCTUATION";
        case TOK_KEYWORD:     return "KEYWORD";
        case TOK_TYPE:        return "TYPE";
        case TOK_COMMENT:     return "COMMENT";
        case TOK_DIRECTIVE:   return "DIRECTIVE";
        default:              return "UNKNOWN";
    }
}
