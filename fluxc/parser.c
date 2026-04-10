/*
 * FLUX OS — FLUX.MD Parser
 *
 * Parses tokens from the lexer into an Abstract Syntax Tree (AST).
 * Supports the FLUX.MD format where markdown constructs map to
 * compilation units:
 *
 *   # Module Name          -> MODULE node
 *   ## function name       -> FUNCTION node
 *   ```flux code ```       -> parsed as function body
 *   ## agent: name         -> AGENT_OP node
 *   ## tile: name          -> TILE node
 *   ## region: name        -> REGION node
 *   ## import: module      -> IMPORT node
 *   ## export: symbol      -> EXPORT node
 *   ### meta key: value    -> META node
 *
 * The parser uses recursive descent with error recovery: on syntax errors,
 * it skips to the next section (## heading) and reports the error.
 */

#include "flux/compiler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ========================================================================
 * AST Node Types
 * ======================================================================== */

typedef enum {
    AST_MODULE         = 1,
    AST_FUNCTION       = 2,
    AST_BLOCK          = 3,
    AST_ASSIGNMENT     = 4,
    AST_BINARY_OP      = 5,
    AST_UNARY_OP       = 6,
    AST_CALL           = 7,
    AST_RETURN         = 8,
    AST_IF             = 9,
    AST_LOOP           = 10,
    AST_AGENT_OP       = 11,
    AST_TILE           = 12,
    AST_REGION         = 13,
    AST_IMPORT         = 14,
    AST_EXPORT         = 15,
    AST_LITERAL        = 16,
    AST_IDENTIFIER     = 17,
    AST_META           = 18,
    AST_CODE_INJECTION = 19,
    AST_PARAM          = 20,
    AST_FIELD_ACCESS   = 21,
    AST_INDEX_ACCESS   = 22,
    AST_STRUCT_DEF     = 23,
    AST_ENUM_DEF       = 24,
    AST_TYPE_DECL      = 25,
    AST_CAST_EXPR      = 26,
    AST_SIZEOF_EXPR    = 27,
    AST_MATCH_EXPR     = 28,
    AST_SPAWN_EXPR     = 29,
} ast_node_type_t;

/* Forward declaration */
typedef struct ast_node_t ast_node_t;

/* ========================================================================
 * AST Node Structure
 * ======================================================================== */

#define AST_MAX_CHILDREN 64
#define AST_NAME_MAX     256
#define AST_TEXT_MAX     2048

struct ast_node_t {
    ast_node_type_t  type;
    int              line;
    int              col;

    /* Node name (for functions, modules, identifiers) */
    char             name[AST_NAME_MAX];

    /* Literal value */
    union {
        int64_t  ival;
        double   fval;
        bool     bval;
    } literal;

    /* Type information */
    char             type_name[64];   /* Type annotation string */
    bool             has_type;

    /* Children */
    ast_node_t      *children[AST_MAX_CHILDREN];
    int              num_children;

    /* Code text (for CODE_INJECTION, FUNCTION body source) */
    char             code_text[AST_TEXT_MAX];

    /* Binary operator string ("+", "-", "==", etc.) */
    char             op_str[16];

    /* Unary operator prefix ("-", "!", "*", "&") */
    char             unary_op[4];

    /* Metadata key-value */
    char             meta_key[128];
    char             meta_value[512];

    /* For IMPORT: module path */
    char             import_path[256];

    /* For TILE: tile category */
    char             tile_category[32];

    /* For REGION: region properties */
    int              region_size;
    char             region_perm[16];
};

/* ========================================================================
 * AST Pool Allocator
 * ======================================================================== */

#define AST_POOL_SIZE 4096

typedef struct {
    ast_node_t nodes[AST_POOL_SIZE];
    int        count;
} ast_pool_t;

static ast_node_t *ast_alloc(ast_pool_t *pool) {
    if (pool->count >= AST_POOL_SIZE) return NULL;
    ast_node_t *node = &pool->nodes[pool->count++];
    memset(node, 0, sizeof(ast_node_t));
    node->type = AST_MODULE;
    node->line = 1;
    node->col = 1;
    return node;
}

static void ast_add_child(ast_node_t *parent, ast_node_t *child) {
    if (!parent || !child) return;
    if (parent->num_children < AST_MAX_CHILDREN) {
        parent->children[parent->num_children++] = child;
    }
}

/* ========================================================================
 * Parser State
 * ======================================================================== */

typedef struct {
    /* Lexer integration */
    void       *lexer;       /* flux_lexer_t* */
    void       *peek_tok;    /* flux_token_t* (reusable) */

    /* Token access — function pointers set by parser_init */
    void      (*next_fn)(void *lexer, void *token);
    void      (*peek_fn)(void *lexer, void *token);
    int       (*line_fn)(void *lexer);
    int       (*col_fn)(void *lexer);

    /* AST */
    ast_pool_t pool;
    ast_node_t *root;

    /* Error tracking */
    flux_compile_error_t errors[32];
    int               num_errors;
    int               max_errors;

    /* Current context */
    ast_node_t       *current_function;
    int               in_code_block;
    char              current_lang[64]; /* Current code fence language */
} flux_parser_t;

/* ========================================================================
 * Token wrapper — read fields from the opaque token struct
 *
 * The lexer produces flux_token_t which has specific layout. We access
 * it via the parser's next/peek functions. We define inline helpers
 * that read the fields we need.
 * ======================================================================== */

/* These offsets match the flux_token_t structure in lexer.c */
#define TOK_TYPE_OFFSET    0
#define TOK_LINE_OFFSET    4
#define TOK_COL_OFFSET     8
#define TOK_LEN_OFFSET     12
#define TOK_TEXT_OFFSET    16
#define TOK_HEADING_OFFSET (16 + 2048)  /* after text[2048] */
#define TOK_LANG_OFFSET    (TOK_HEADING_OFFSET + 4)
#define TOK_INTVAL_OFFSET  (TOK_LANG_OFFSET + 64)
#define TOK_FLOATVAL_OFFSET (TOK_INTVAL_OFFSET + 4)

static inline int token_get_type(void *token) {
    int val;
    memcpy(&val, (char *)token + TOK_TYPE_OFFSET, sizeof(int));
    return val;
}

static inline int token_get_line(void *token) {
    int val;
    memcpy(&val, (char *)token + TOK_LINE_OFFSET, sizeof(int));
    return val;
}

static inline int token_get_col(void *token) {
    int val;
    memcpy(&val, (char *)token + TOK_COL_OFFSET, sizeof(int));
    return val;
}

static inline const char *token_get_text(void *token) {
    return (const char *)((char *)token + TOK_TEXT_OFFSET);
}

static inline int token_get_heading_level(void *token) {
    int val;
    memcpy(&val, (char *)token + TOK_HEADING_OFFSET, sizeof(int));
    return val;
}

static inline const char *token_get_lang(void *token) {
    return (const char *)((char *)token + TOK_LANG_OFFSET);
}

/* ========================================================================
 * Parser Helpers
 * ======================================================================== */

static void parser_error(flux_parser_t *p, int line, int col, const char *msg) {
    if (p->num_errors < p->max_errors) {
        flux_compile_error_t *err = &p->errors[p->num_errors++];
        err->line = line;
        err->col = col;
        err->fatal = false;
        snprintf(err->message, FLUX_COMPILE_ERR_MAX, "%s", msg);
        snprintf(err->context, 512, "at line %d, col %d", line, col);
    }
}

/* Consume next token from lexer */
static void parser_next(flux_parser_t *p) {
    if (p->next_fn && p->lexer) {
        p->next_fn(p->lexer, p->peek_tok);
    }
}

/* Peek at current token type */
static int parser_peek_type(flux_parser_t *p) {
    if (!p->peek_tok) return 9; /* EOF */
    return token_get_type(p->peek_tok);
}

/* Peek at current token text */
static const char *parser_peek_text(flux_parser_t *p) {
    if (!p->peek_tok) return "";
    return token_get_text(p->peek_tok);
}

/* Peek at heading level */
static int parser_peek_heading(flux_parser_t *p) {
    if (!p->peek_tok) return 0;
    return token_get_heading_level(p->peek_tok);
}

/* Peek at code block language */
static const char *parser_peek_lang(flux_parser_t *p) {
    if (!p->peek_tok) return "";
    return token_get_lang(p->peek_tok);
}

/* Check if current token type matches */
static bool parser_check(flux_parser_t *p, int type) {
    return parser_peek_type(p) == type;
}

/* Consume and return true if token matches expected type */
static bool parser_match(flux_parser_t *p, int type) {
    if (parser_check(p, type)) {
        parser_next(p);
        return true;
    }
    return false;
}

/* Expect a token type; report error if not found */
static bool parser_expect(flux_parser_t *p, int type, const char *expected) {
    if (parser_check(p, type)) {
        parser_next(p);
        return true;
    }
    parser_error(p, p->line_fn ? p->line_fn(p->lexer) : 0,
                     p->col_fn ? p->col_fn(p->lexer) : 0,
                     expected);
    return false;
}

/* Skip newlines */
static void parser_skip_newlines(flux_parser_t *p) {
    while (parser_check(p, 8 /* TOK_NEWLINE */)) {
        parser_next(p);
    }
}

/* Skip to next section (## heading or EOF) */
static void parser_skip_to_section(flux_parser_t *p) {
    while (!parser_check(p, 9 /* TOK_EOF */)) {
        int tt = parser_peek_type(p);
        if (tt == 1 /* TOK_HEADING */ && parser_peek_heading(p) == 2) {
            break; /* Found ## section */
        }
        if (tt == 1 /* TOK_HEADING */ && parser_peek_heading(p) == 1) {
            break; /* Found # module heading */
        }
        parser_next(p);
    }
}

/* Trim whitespace from start and end of string */
static void str_trim(char *dst, const char *src, int max_len) {
    if (!src || !dst) return;
    int len = (int)strlen(src);
    int start = 0, end = len;

    while (start < len && (src[start] == ' ' || src[start] == '\t' ||
                           src[start] == '\n' || src[start] == '\r')) {
        start++;
    }
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' ||
                           src[end - 1] == '\n' || src[end - 1] == '\r')) {
        end--;
    }

    int copy_len = end - start;
    if (copy_len >= max_len) copy_len = max_len - 1;
    memcpy(dst, src + start, (size_t)copy_len);
    dst[copy_len] = '\0';
}

/* ========================================================================
 * AST Node Constructors
 * ======================================================================== */

static ast_node_t *ast_make_module(flux_parser_t *p, const char *name) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_MODULE;
    node->line = p->line_fn ? p->line_fn(p->lexer) : 1;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    return node;
}

static ast_node_t *ast_make_function(flux_parser_t *p, const char *name) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_FUNCTION;
    node->line = p->line_fn ? p->line_fn(p->lexer) : 1;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    return node;
}

static ast_node_t *ast_make_block(flux_parser_t *p) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_BLOCK;
    node->line = p->line_fn ? p->line_fn(p->lexer) : 1;
    return node;
}

static ast_node_t *ast_make_literal_int(flux_parser_t *p, int64_t val) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_LITERAL;
    node->literal.ival = val;
    snprintf(node->type_name, 64, "i64");
    node->has_type = true;
    return node;
}

static ast_node_t *ast_make_literal_float(flux_parser_t *p, double val) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_LITERAL;
    node->literal.fval = val;
    snprintf(node->type_name, 64, "f64");
    node->has_type = true;
    return node;
}

static ast_node_t *ast_make_literal_string(flux_parser_t *p, const char *val) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_LITERAL;
    snprintf(node->code_text, AST_TEXT_MAX, "%s", val);
    snprintf(node->type_name, 64, "str");
    node->has_type = true;
    return node;
}

static ast_node_t *ast_make_literal_bool(flux_parser_t *p, bool val) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_LITERAL;
    node->literal.bval = val;
    snprintf(node->type_name, 64, "bool");
    node->has_type = true;
    return node;
}

static ast_node_t *ast_make_ident(flux_parser_t *p, const char *name) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_IDENTIFIER;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    return node;
}

static ast_node_t *ast_make_binary_op(flux_parser_t *p, const char *op,
                                       ast_node_t *left, ast_node_t *right) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_BINARY_OP;
    snprintf(node->op_str, 16, "%s", op);
    ast_add_child(node, left);
    ast_add_child(node, right);
    return node;
}

static ast_node_t *ast_make_unary_op(flux_parser_t *p, const char *op,
                                      ast_node_t *operand) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_UNARY_OP;
    snprintf(node->unary_op, 4, "%s", op);
    ast_add_child(node, operand);
    return node;
}

static ast_node_t *ast_make_call(flux_parser_t *p, const char *name,
                                  ast_node_t *args[], int nargs) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_CALL;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    for (int i = 0; i < nargs && i < AST_MAX_CHILDREN; i++) {
        ast_add_child(node, args[i]);
    }
    return node;
}

static ast_node_t *ast_make_return(flux_parser_t *p, ast_node_t *expr) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_RETURN;
    if (expr) ast_add_child(node, expr);
    return node;
}

static ast_node_t *ast_make_assignment(flux_parser_t *p, const char *name,
                                        ast_node_t *value, const char *type) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_ASSIGNMENT;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    if (type) {
        snprintf(node->type_name, 64, "%s", type);
        node->has_type = true;
    }
    if (value) ast_add_child(node, value);
    return node;
}

static ast_node_t *ast_make_if(flux_parser_t *p, ast_node_t *cond,
                                ast_node_t *then_block, ast_node_t *else_block) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_IF;
    if (cond) ast_add_child(node, cond);
    if (then_block) ast_add_child(node, then_block);
    if (else_block) ast_add_child(node, else_block);
    return node;
}

static ast_node_t *ast_make_loop(flux_parser_t *p, ast_node_t *cond,
                                  ast_node_t *body) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_LOOP;
    if (cond) ast_add_child(node, cond);
    if (body) ast_add_child(node, body);
    return node;
}

static ast_node_t *ast_make_agent_op(flux_parser_t *p, const char *agent_name,
                                      const char *op_type) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_AGENT_OP;
    snprintf(node->name, AST_NAME_MAX, "%s", agent_name);
    snprintf(node->op_str, 16, "%s", op_type);
    return node;
}

static ast_node_t *ast_make_tile(flux_parser_t *p, const char *name,
                                  const char *category) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_TILE;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    snprintf(node->tile_category, 32, "%s", category);
    return node;
}

static ast_node_t *ast_make_region(flux_parser_t *p, const char *name,
                                    int size, const char *perm) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_REGION;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    node->region_size = size;
    snprintf(node->region_perm, 16, "%s", perm);
    return node;
}

static ast_node_t *ast_make_import(flux_parser_t *p, const char *path) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_IMPORT;
    snprintf(node->import_path, 256, "%s", path);
    return node;
}

static ast_node_t *ast_make_export(flux_parser_t *p, const char *symbol) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_EXPORT;
    snprintf(node->name, AST_NAME_MAX, "%s", symbol);
    return node;
}

static ast_node_t *ast_make_meta(flux_parser_t *p, const char *key,
                                  const char *value) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_META;
    snprintf(node->meta_key, 128, "%s", key);
    snprintf(node->meta_value, 512, "%s", value);
    return node;
}

static ast_node_t *ast_make_code_injection(flux_parser_t *p,
                                            const char *code,
                                            const char *lang) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_CODE_INJECTION;
    snprintf(node->code_text, AST_TEXT_MAX, "%s", code);
    snprintf(node->type_name, 64, "%s", lang);
    return node;
}

static ast_node_t *ast_make_param(flux_parser_t *p, const char *name,
                                   const char *type) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_PARAM;
    snprintf(node->name, AST_NAME_MAX, "%s", name);
    snprintf(node->type_name, 64, "%s", type);
    node->has_type = true;
    return node;
}

static ast_node_t *ast_make_spawn(flux_parser_t *p, const char *agent_name,
                                   ast_node_t *body) {
    ast_node_t *node = ast_alloc(&p->pool);
    if (!node) return NULL;
    node->type = AST_SPAWN_EXPR;
    snprintf(node->name, AST_NAME_MAX, "%s", agent_name);
    if (body) ast_add_child(node, body);
    return node;
}

/* ========================================================================
 * Simple Expression Parser (for code inside code blocks)
 * ======================================================================== */

/* Forward declaration */
static ast_node_t *parse_expression(flux_parser_t *p);

static ast_node_t *parse_primary(flux_parser_t *p) {
    int tt = parser_peek_type(p);
    const char *text = parser_peek_text(p);

    switch (tt) {
        case 11: { /* TOK_NUMBER */
            parser_next(p);
            /* Check if float or int */
            if (strchr(text, '.') || strchr(text, 'e') || strchr(text, 'E')) {
                return ast_make_literal_float(p, strtod(text, NULL));
            }
            return ast_make_literal_int(p, strtoll(text, NULL, 0));
        }
        case 12: { /* TOK_STRING */
            parser_next(p);
            /* Remove surrounding quotes */
            char buf[AST_TEXT_MAX];
            int slen = (int)strlen(text);
            if (slen >= 2) {
                memcpy(buf, text + 1, (size_t)(slen - 2));
                buf[slen - 2] = '\0';
            } else {
                buf[0] = '\0';
            }
            return ast_make_literal_string(p, buf);
        }
        case 15: { /* TOK_KEYWORD */
            if (strcmp(text, "true") == 0) {
                parser_next(p);
                return ast_make_literal_bool(p, true);
            }
            if (strcmp(text, "false") == 0) {
                parser_next(p);
                return ast_make_literal_bool(p, false);
            }
            if (strcmp(text, "nil") == 0 || strcmp(text, "null") == 0) {
                parser_next(p);
                ast_node_t *n = ast_alloc(&p->pool);
                if (n) { n->type = AST_LITERAL; snprintf(n->type_name, 64, "void"); }
                return n;
            }
            /* Fall through to identifier */
        }
        case 10: { /* TOK_IDENTIFIER */
        case 16: { /* TOK_TYPE */
            parser_next(p);
            return ast_make_ident(p, text);
        }
        case 14: { /* TOK_PUNCTUATION */
            if (text[0] == '(') {
                parser_next(p);
                ast_node_t *expr = parse_expression(p);
                parser_match(p, 14); /* ) */
                return expr;
            }
            break;
        }
        default:
            break;
    }

    parser_error(p, p->line_fn ? p->line_fn(p->lexer) : 0,
                     p->col_fn ? p->col_fn(p->lexer) : 0,
                     "expected expression");
    return NULL;
}

static int get_precedence(const char *op) {
    if (strcmp(op, "||") == 0) return 1;
    if (strcmp(op, "&&") == 0) return 2;
    if (strcmp(op, "|") == 0) return 3;
    if (strcmp(op, "^") == 0) return 4;
    if (strcmp(op, "&") == 0) return 5;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) return 6;
    if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) return 7;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) return 8;
    if (strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
        strcmp(op, "%") == 0) return 9;
    return 0;
}

static ast_node_t *parse_unary(flux_parser_t *p) {
    int tt = parser_peek_type(p);
    const char *text = parser_peek_text(p);

    if (tt == 13 /* TOK_OPERATOR */) {
        if (strcmp(text, "-") == 0 || strcmp(text, "!") == 0 ||
            strcmp(text, "~") == 0 || strcmp(text, "*") == 0 ||
            strcmp(text, "&") == 0) {
            parser_next(p);
            ast_node_t *operand = parse_unary(p);
            return ast_make_unary_op(p, text, operand);
        }
    }
    return parse_primary(p);
}

static ast_node_t *parse_expression(flux_parser_t *p) {
    ast_node_t *left = parse_unary(p);
    if (!left) return NULL;

    while (parser_check(p, 13 /* TOK_OPERATOR */)) {
        const char *op = parser_peek_text(p);
        int prec = get_precedence(op);
        if (prec == 0) break;

        parser_next(p);
        ast_node_t *right = parse_unary(p);
        if (!right) break;

        left = ast_make_binary_op(p, op, left, right);
    }

    return left;
}

/* Parse a function call expression */
static ast_node_t *parse_call(flux_parser_t *p, const char *name) {
    ast_node_t *args[AST_MAX_CHILDREN];
    int nargs = 0;

    parser_match(p, 14); /* ( */

    while (!parser_check(p, 14) || parser_peek_text(p)[0] != ')') {
        if (nargs > 0) {
            if (!parser_match(p, 14)) break; /* , */
        }
        ast_node_t *arg = parse_expression(p);
        if (!arg) break;
        if (nargs < AST_MAX_CHILDREN) args[nargs++] = arg;
    }

    parser_match(p, 14); /* ) */
    return ast_make_call(p, name, args, nargs);
}

/* ========================================================================
 * Section Parsers
 * ======================================================================== */

/* Parse a function declaration: ## fn name(params) -> ret_type */
static ast_node_t *parse_function_section(flux_parser_t *p, const char *heading_text) {
    (void)heading_text;

    /* Get the function heading text */
    const char *text = parser_peek_text(p);
    parser_next(p);
    parser_skip_newlines(p);

    /* Parse: fn name(params) -> ret_type */
    char name[AST_NAME_MAX] = {0};
    char ret_type[64] = "void";
    char params_str[1024] = {0};

    /* Extract function signature from heading text */
    /* Format: "fn name(params) -> ret_type" or "function name(params)" */
    const char *fn_start = strstr(text, "fn ");
    const char *func_start = strstr(text, "function ");
    const char *sig_start = fn_start ? fn_start + 3 :
                            (func_start ? func_start + 9 : text);

    /* Skip ## and space */
    while (*sig_start == '#' || *sig_start == ' ') sig_start++;

    /* Extract name */
    int ni = 0;
    while (*sig_start && *sig_start != '(' && *sig_start != ':' &&
           *sig_start != ' ' && ni < AST_NAME_MAX - 1) {
        name[ni++] = *sig_start++;
    }
    name[ni] = '\0';

    /* Skip whitespace */
    while (*sig_start == ' ') sig_start++;

    /* Extract params */
    if (*sig_start == '(') {
        sig_start++; /* skip ( */
        int pi = 0;
        int depth = 1;
        while (*sig_start && depth > 0 && pi < 1023) {
            if (*sig_start == '(') depth++;
            if (*sig_start == ')') depth--;
            if (depth > 0) params_str[pi++] = *sig_start;
            sig_start++;
        }
        params_str[pi] = '\0';
        if (*sig_start == ')') sig_start++;
    }

    /* Extract return type (-> type) */
    const char *arrow = strstr(sig_start, "->");
    if (arrow) {
        arrow += 2;
        while (*arrow == ' ') arrow++;
        int ri = 0;
        while (*arrow && *arrow != ' ' && ri < 63) {
            ret_type[ri++] = *arrow++;
        }
        ret_type[ri] = '\0';
    }

    ast_node_t *func_node = ast_make_function(p, name);
    if (!func_node) return NULL;
    snprintf(func_node->type_name, 64, "%s", ret_type);
    func_node->has_type = true;

    /* Parse parameters */
    if (params_str[0] != '\0') {
        char *param = strtok(params_str, ",");
        while (param) {
            /* Trim whitespace */
            while (*param == ' ') param++;
            char pname[128] = {0};
            char ptype[64] = "i32";

            /* Parse name: type */
            int i = 0;
            while (param[i] && param[i] != ':' && param[i] != ' ' && i < 127) {
                pname[i] = param[i];
                i++;
            }
            pname[i] = '\0';

            /* Skip to type */
            const char *colon = strchr(param, ':');
            if (colon) {
                colon++;
                while (*colon == ' ') colon++;
                int ti = 0;
                while (*colon && *colon != ',' && *colon != ')' &&
                       *colon != ' ' && ti < 63) {
                    ptype[ti++] = *colon++;
                }
                ptype[ti] = '\0';
            }

            ast_add_child(func_node, ast_make_param(p, pname, ptype));
            param = strtok(NULL, ",");
        }
    }

    /* Parse function body (code blocks and statements) */
    ast_node_t *body = ast_make_block(p);
    if (body) {
        p->current_function = func_node;

        /* Look for code blocks under this function */
        while (!parser_check(p, 9 /* TOK_EOF */)) {
            int tt = parser_peek_type(p);
            int hl = (tt == 1) ? parser_peek_heading(p) : 0;

            /* Stop at new section (## or # heading) */
            if (tt == 1 && (hl == 1 || hl == 2)) break;

            /* Code block */
            if (tt == 3 /* TOK_CODE_BLOCK */) {
                const char *code = parser_peek_text(p);
                const char *lang = parser_peek_lang(p);
                parser_next(p);
                parser_skip_newlines(p);

                ast_node_t *code_node = ast_make_code_injection(p, code, lang);
                if (code_node) {
                    ast_add_child(body, code_node);
                    /* Also store as function body for convenience */
                    snprintf(func_node->code_text, AST_TEXT_MAX, "%s", code);
                }
                continue;
            }

            /* List items as statements */
            if (tt == 5 /* TOK_LIST_ITEM */) {
                parser_next(p);
                continue;
            }

            /* Meta entries */
            if (tt == 6 /* TOK_META_KEY */) {
                const char *key = parser_peek_text(p);
                parser_next(p);
                /* Expect meta value */
                if (parser_check(p, 7 /* TOK_META_VALUE */)) {
                    const char *value = parser_peek_text(p);
                    parser_next(p);
                    ast_add_child(func_node, ast_make_meta(p, key, value));
                }
                continue;
            }

            /* Directives */
            if (tt == 18 /* TOK_DIRECTIVE */) {
                parser_next(p);
                continue;
            }

            parser_next(p);
        }

        ast_add_child(func_node, body);
        p->current_function = NULL;
    }

    return func_node;
}

/* Parse an agent section: ## agent: name */
static ast_node_t *parse_agent_section(flux_parser_t *p, const char *heading_text) {
    (void)heading_text;
    const char *text = parser_peek_text(p);
    parser_next(p);
    parser_skip_newlines(p);

    /* Extract agent name */
    const char *agent_marker = strstr(text, "agent:");
    if (!agent_marker) agent_marker = strstr(text, "agent ");
    if (!agent_marker) agent_marker = text;

    /* Skip "agent:" or "agent " */
    while (*agent_marker && (*agent_marker == ':' || *agent_marker == ' ' ||
           *agent_marker == '#')) agent_marker++;

    char agent_name[AST_NAME_MAX] = {0};
    int ni = 0;
    while (*agent_marker && *agent_marker != '\n' && ni < AST_NAME_MAX - 1) {
        agent_name[ni++] = *agent_marker++;
    }
    agent_name[ni] = '\0';
    str_trim(agent_name, agent_name, AST_NAME_MAX);

    ast_node_t *agent = ast_make_agent_op(p, agent_name, "agent");

    /* Collect agent body (code blocks, etc.) */
    ast_node_t *body = ast_make_block(p);
    if (body) {
        while (!parser_check(p, 9 /* TOK_EOF */)) {
            int tt = parser_peek_type(p);
            int hl = (tt == 1) ? parser_peek_heading(p) : 0;
            if (tt == 1 && (hl == 1 || hl == 2)) break;
            if (tt == 3 /* CODE_BLOCK */) {
                const char *code = parser_peek_text(p);
                const char *lang = parser_peek_lang(p);
                parser_next(p);
                ast_add_child(body, ast_make_code_injection(p, code, lang));
            } else if (tt == 6 /* META_KEY */) {
                const char *key = parser_peek_text(p);
                parser_next(p);
                if (parser_check(p, 7 /* META_VALUE */)) {
                    const char *val = parser_peek_text(p);
                    parser_next(p);
                    ast_add_child(agent, ast_make_meta(p, key, val));
                }
            } else {
                parser_next(p);
            }
        }
        ast_add_child(agent, body);
    }

    return agent;
}

/* Parse a tile section: ## tile: name */
static ast_node_t *parse_tile_section(flux_parser_t *p, const char *heading_text) {
    (void)heading_text;
    const char *text = parser_peek_text(p);
    parser_next(p);
    parser_skip_newlines(p);

    const char *tile_marker = strstr(text, "tile:");
    if (!tile_marker) tile_marker = strstr(text, "tile ");
    if (!tile_marker) tile_marker = text;
    while (*tile_marker && (*tile_marker == ':' || *tile_marker == ' ' ||
           *tile_marker == '#')) tile_marker++;

    char tile_name[AST_NAME_MAX] = {0};
    int ni = 0;
    while (*tile_marker && *tile_marker != '\n' && ni < AST_NAME_MAX - 1) {
        tile_name[ni++] = *tile_marker++;
    }
    tile_name[ni] = '\0';
    str_trim(tile_name, tile_name, AST_NAME_MAX);

    ast_node_t *tile = ast_make_tile(p, tile_name, "compute");

    /* Collect tile body */
    ast_node_t *body = ast_make_block(p);
    if (body) {
        while (!parser_check(p, 9)) {
            int tt = parser_peek_type(p);
            int hl = (tt == 1) ? parser_peek_heading(p) : 0;
            if (tt == 1 && (hl == 1 || hl == 2)) break;
            if (tt == 3) {
                const char *code = parser_peek_text(p);
                const char *lang = parser_peek_lang(p);
                parser_next(p);
                ast_add_child(body, ast_make_code_injection(p, code, lang));
            } else if (tt == 6) {
                const char *key = parser_peek_text(p);
                parser_next(p);
                if (parser_check(p, 7)) {
                    const char *val = parser_peek_text(p);
                    parser_next(p);
                    if (strcmp(key, "category") == 0) {
                        snprintf(tile->tile_category, 32, "%s", val);
                    }
                    ast_add_child(tile, ast_make_meta(p, key, val));
                }
            } else {
                parser_next(p);
            }
        }
        ast_add_child(tile, body);
    }

    return tile;
}

/* Parse a region section: ## region: name */
static ast_node_t *parse_region_section(flux_parser_t *p, const char *heading_text) {
    (void)heading_text;
    const char *text = parser_peek_text(p);
    parser_next(p);
    parser_skip_newlines(p);

    const char *region_marker = strstr(text, "region:");
    if (!region_marker) region_marker = strstr(text, "region ");
    if (!region_marker) region_marker = text;
    while (*region_marker && (*region_marker == ':' || *region_marker == ' ' ||
           *region_marker == '#')) region_marker++;

    char region_name[AST_NAME_MAX] = {0};
    int ni = 0;
    while (*region_marker && *region_marker != '\n' && ni < AST_NAME_MAX - 1) {
        region_name[ni++] = *region_marker++;
    }
    region_name[ni] = '\0';
    str_trim(region_name, region_name, AST_NAME_MAX);

    ast_node_t *region = ast_make_region(p, region_name, 4096, "rw");

    /* Collect region body (meta for size, permissions) */
    while (!parser_check(p, 9)) {
        int tt = parser_peek_type(p);
        int hl = (tt == 1) ? parser_peek_heading(p) : 0;
        if (tt == 1 && (hl == 1 || hl == 2)) break;
        if (tt == 6) {
            const char *key = parser_peek_text(p);
            parser_next(p);
            if (parser_check(p, 7)) {
                const char *val = parser_peek_text(p);
                parser_next(p);
                if (strcmp(key, "size") == 0) {
                    region->region_size = atoi(val);
                } else if (strcmp(key, "perm") == 0) {
                    snprintf(region->region_perm, 16, "%s", val);
                }
                ast_add_child(region, ast_make_meta(p, key, val));
            }
        } else {
            parser_next(p);
        }
    }

    return region;
}

/* Parse an import section: ## import: module */
static ast_node_t *parse_import_section(flux_parser_t *p, const char *heading_text) {
    (void)heading_text;
    const char *text = parser_peek_text(p);
    parser_next(p);

    const char *import_marker = strstr(text, "import:");
    if (!import_marker) import_marker = strstr(text, "import ");
    if (!import_marker) import_marker = text;
    while (*import_marker && (*import_marker == ':' || *import_marker == ' ' ||
           *import_marker == '#')) import_marker++;

    char path[256] = {0};
    int pi = 0;
    while (*import_marker && *import_marker != '\n' && pi < 255) {
        path[pi++] = *import_marker++;
    }
    path[pi] = '\0';
    str_trim(path, path, 256);

    return ast_make_import(p, path);
}

/* Parse an export section: ## export: symbol */
static ast_node_t *parse_export_section(flux_parser_t *p, const char *heading_text) {
    (void)heading_text;
    const char *text = parser_peek_text(p);
    parser_next(p);

    const char *export_marker = strstr(text, "export:");
    if (!export_marker) export_marker = strstr(text, "export ");
    if (!export_marker) export_marker = text;
    while (*export_marker && (*export_marker == ':' || *export_marker == ' ' ||
           *export_marker == '#')) export_marker++;

    char symbol[AST_NAME_MAX] = {0};
    int si = 0;
    while (*export_marker && *export_marker != '\n' && si < AST_NAME_MAX - 1) {
        symbol[si++] = *export_marker++;
    }
    symbol[si] = '\0';
    str_trim(symbol, symbol, AST_NAME_MAX);

    return ast_make_export(p, symbol);
}

/* ========================================================================
 * Main Parse Entry
 * ======================================================================== */

ast_node_t *flux_parser_parse(void *parser) {
    flux_parser_t *p = (flux_parser_t *)parser;
    if (!p) return NULL;

    memset(&p->pool, 0, sizeof(ast_pool_t));
    p->num_errors = 0;
    p->current_function = NULL;
    p->in_code_block = 0;

    /* Prime the first token */
    parser_next(p);

    /* Expect module heading (# name) */
    const char *module_name = "unnamed";
    if (parser_check(p, 1 /* TOK_HEADING */) && parser_peek_heading(p) == 1) {
        const char *heading = parser_peek_text(p);
        /* Extract module name (text after # ) */
        const char *name_start = heading;
        while (*name_start == '#') name_start++;
        while (*name_start == ' ') name_start++;
        char name_buf[AST_NAME_MAX] = {0};
        int ni = 0;
        while (*name_start && *name_start != '\n' && ni < AST_NAME_MAX - 1) {
            name_buf[ni++] = *name_start++;
        }
        name_buf[ni] = '\0';
        str_trim(name_buf, name_buf, AST_NAME_MAX);
        module_name = name_buf;
        parser_next(p);
        parser_skip_newlines(p);
    }

    ast_node_t *module = ast_make_module(p, module_name);
    if (!module) return NULL;

    /* Parse top-level sections */
    while (!parser_check(p, 9 /* TOK_EOF */)) {
        parser_skip_newlines(p);

        if (parser_check(p, 9 /* TOK_EOF */)) break;

        int tt = parser_peek_type(p);

        /* Heading-based sections */
        if (tt == 1 /* TOK_HEADING */) {
            int level = parser_peek_heading(p);
            const char *heading = parser_peek_text(p);

            if (level == 1) {
                /* Another module heading — skip */
                parser_next(p);
                continue;
            }

            if (level == 2) {
                /* ## section */
                if (strstr(heading, "fn ") || strstr(heading, "function ")) {
                    ast_node_t *func = parse_function_section(p, heading);
                    if (func) ast_add_child(module, func);
                } else if (strstr(heading, "agent") != NULL) {
                    ast_node_t *agent = parse_agent_section(p, heading);
                    if (agent) ast_add_child(module, agent);
                } else if (strstr(heading, "tile") != NULL) {
                    ast_node_t *tile = parse_tile_section(p, heading);
                    if (tile) ast_add_child(module, tile);
                } else if (strstr(heading, "region") != NULL) {
                    ast_node_t *region = parse_region_section(p, heading);
                    if (region) ast_add_child(module, region);
                } else if (strstr(heading, "import") != NULL) {
                    ast_node_t *imp = parse_import_section(p, heading);
                    if (imp) ast_add_child(module, imp);
                } else if (strstr(heading, "export") != NULL) {
                    ast_node_t *exp = parse_export_section(p, heading);
                    if (exp) ast_add_child(module, exp);
                } else {
                    /* Unknown section — skip */
                    parser_next(p);
                    parser_skip_to_section(p);
                }
                continue;
            }

            if (level >= 3) {
                /* ### subheading — could be meta or struct/enum def */
                if (strstr(heading, "struct ") || strstr(heading, "struct:")) {
                    parser_next(p);
                    parser_skip_to_section(p);
                } else if (strstr(heading, "enum ") || strstr(heading, "enum:")) {
                    parser_next(p);
                    parser_skip_to_section(p);
                } else if (strstr(heading, "meta") || strstr(heading, "Meta")) {
                    parser_next(p);
                    parser_skip_to_section(p);
                } else {
                    parser_next(p);
                    parser_skip_to_section(p);
                }
                continue;
            }
        }

        /* Code blocks at module level (not inside a function) */
        if (tt == 3 /* TOK_CODE_BLOCK */) {
            const char *code = parser_peek_text(p);
            const char *lang = parser_peek_lang(p);
            parser_next(p);
            ast_add_child(module, ast_make_code_injection(p, code, lang));
            continue;
        }

        /* Metadata at module level */
        if (tt == 6 /* TOK_META_KEY */) {
            const char *key = parser_peek_text(p);
            parser_next(p);
            if (parser_check(p, 7 /* TOK_META_VALUE */)) {
                const char *value = parser_peek_text(p);
                parser_next(p);
                ast_add_child(module, ast_make_meta(p, key, value));
            }
            continue;
        }

        /* List items */
        if (tt == 5 /* TOK_LIST_ITEM */) {
            parser_next(p);
            continue;
        }

        /* Directives */
        if (tt == 18 /* TOK_DIRECTIVE */) {
            parser_next(p);
            continue;
        }

        /* Skip unknown tokens */
        parser_next(p);
    }

    p->root = module;
    return module;
}

/* ========================================================================
 * Parser Public API
 * ======================================================================== */

size_t flux_parser_size(void) {
    return sizeof(flux_parser_t);
}

void flux_parser_init(void *parser, const char *source, flux_size_t len) {
    flux_parser_t *p = (flux_parser_t *)parser;
    if (!p) return;
    memset(p, 0, sizeof(flux_parser_t));
    p->max_errors = 32;

    /* Allocate token buffers */
    /* We need to allocate lexer and token buffers */
    /* The caller is responsible for providing these via set_lexer */
}

/* Set the lexer for this parser */
void flux_parser_set_lexer(void *parser, void *lexer,
                           void (*next_fn)(void *, void *),
                           void (*peek_fn)(void *, void *),
                           int (*line_fn)(void *),
                           int (*col_fn)(void *)) {
    flux_parser_t *p = (flux_parser_t *)parser;
    if (!p) return;
    p->lexer = lexer;
    p->next_fn = next_fn;
    p->peek_fn = peek_fn;
    p->line_fn = line_fn;
    p->col_fn = col_fn;
}

/* Get error count from parser */
int flux_parser_error_count(void *parser) {
    return ((flux_parser_t *)parser)->num_errors;
}

/* Get errors from parser */
const flux_compile_error_t *flux_parser_errors(void *parser) {
    return ((flux_parser_t *)parser)->errors;
}

/* Free parser resources */
void flux_parser_free(void *parser) {
    /* AST nodes are in the pool — no individual frees needed */
    flux_parser_t *p = (flux_parser_t *)parser;
    if (p) {
        p->root = NULL;
        p->num_errors = 0;
    }
}
