/*
 * script - DOS Program Wrapper Shell
 *
 * A shebang-compatible interpreter for wrapping DOS executables with
 * modern CLI conveniences: argument parsing, path redirection, and
 * expect-like I/O handling.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ========================================================================
 * Token Types (Lexer)
 * ======================================================================== */

typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,

    // Literals
    TOK_STRING,      // "..." or """..."""
    TOK_INTEGER,     // 42
    TOK_FLOAT,       // 3.14
    TOK_TRUE,        // true
    TOK_FALSE,       // false
    TOK_NULL,        // null

    // Identifiers and keywords
    TOK_IDENT,       // name
    TOK_IF,
    TOK_ELIF,
    TOK_ELSE,
    TOK_FOR,
    TOK_IN,
    TOK_DEF,
    TOK_RETURN,
    TOK_AND,
    TOK_OR,
    TOK_NOT,

    // Operators
    TOK_ASSIGN,      // =
    TOK_EQ,          // ==
    TOK_NE,          // !=
    TOK_LT,          // <
    TOK_LE,          // <=
    TOK_GT,          // >
    TOK_GE,          // >=
    TOK_PLUS,        // +
    TOK_MINUS,       // -
    TOK_STAR,        // *
    TOK_SLASH,       // /
    TOK_PERCENT,     // %
    TOK_DOT,         // .
    TOK_COMMA,       // ,
    TOK_COLON,       // :
    TOK_LPAREN,      // (
    TOK_RPAREN,      // )
    TOK_LBRACKET,    // [
    TOK_RBRACKET,    // ]
    TOK_AT,          // @

    // Special
    TOK_END_MARKER,  // __END__
    TOK_ERROR
} script_token_type;

typedef struct {
    script_token_type type;
    const char *start;
    int length;
    int line;
    int column;

    // For literals
    union {
        int64_t int_val;
        double float_val;
    };
} script_token;

/* ========================================================================
 * Lexer
 * ======================================================================== */

typedef struct {
    const char *source;
    const char *current;
    const char *line_start;
    int line;

    // Indentation tracking
    int indent_stack[64];
    int indent_depth;
    int pending_dedents;
    bool at_line_start;

    // Error state
    char error[256];
} script_lexer;

void script_lexer_init(script_lexer *lex, const char *source);
script_token script_lexer_next(script_lexer *lex);
const char *script_token_name(script_token_type type);

/* ========================================================================
 * AST Node Types (Parser)
 * ======================================================================== */

typedef enum {
    // Literals
    NODE_STRING,
    NODE_INTEGER,
    NODE_FLOAT,
    NODE_BOOL,
    NODE_NULL,
    NODE_LIST,

    // Expressions
    NODE_IDENT,
    NODE_BINARY,     // a + b
    NODE_UNARY,      // -a, not a
    NODE_CALL,       // func(args)
    NODE_INDEX,      // list[i]
    NODE_ATTR,       // obj.attr

    // Statements
    NODE_ASSIGN,     // name = value
    NODE_IF,
    NODE_FOR,
    NODE_DEF,
    NODE_RETURN,
    NODE_EXPR_STMT,  // expression as statement
    NODE_DECORATOR,  // @decorator
    NODE_BLOCK,      // list of statements

    // Top-level
    NODE_PROGRAM
} script_node_type;

typedef struct script_node script_node;

struct script_node {
    script_node_type type;
    int line;

    union {
        // NODE_STRING
        struct { char *value; } string;

        // NODE_INTEGER
        struct { int64_t value; } integer;

        // NODE_FLOAT
        struct { double value; } floating;

        // NODE_BOOL
        struct { bool value; } boolean;

        // NODE_LIST, NODE_BLOCK, NODE_PROGRAM
        struct {
            script_node **items;
            int count;
        } list;

        // NODE_IDENT
        struct { char *name; } ident;

        // NODE_BINARY
        struct {
            script_token_type op;
            script_node *left;
            script_node *right;
        } binary;

        // NODE_UNARY
        struct {
            script_token_type op;
            script_node *operand;
        } unary;

        // NODE_CALL
        struct {
            script_node *callee;
            script_node **args;
            int arg_count;
            char **kwnames;      // keyword argument names
            script_node **kwvalues;
            int kw_count;
        } call;

        // NODE_INDEX
        struct {
            script_node *object;
            script_node *index;
        } index;

        // NODE_ATTR
        struct {
            script_node *object;
            char *attr;
        } attr;

        // NODE_ASSIGN
        struct {
            char *name;
            script_node *value;
        } assign;

        // NODE_IF
        struct {
            script_node *condition;
            script_node *then_block;
            script_node *else_block;  // may be another if (elif) or block
        } if_stmt;

        // NODE_FOR
        struct {
            char *var;
            script_node *iterable;
            script_node *body;
        } for_stmt;

        // NODE_DEF
        struct {
            char *name;
            char **params;
            int param_count;
            script_node *body;
            script_node *decorator;  // optional @decorator
        } def;

        // NODE_RETURN
        struct {
            script_node *value;  // may be NULL
        } return_stmt;

        // NODE_DECORATOR
        struct {
            script_node *expr;   // the decorator expression
            script_node *target; // the decorated function
        } decorator;
    };
};

/* ========================================================================
 * Parser
 * ======================================================================== */

typedef struct {
    script_lexer lexer;
    script_token current;
    script_token previous;

    // Error state
    bool had_error;
    char error[512];
} script_parser;

void script_parser_init(script_parser *parser, const char *source);
script_node *script_parse(script_parser *parser);
void script_node_free(script_node *node);

/* ========================================================================
 * Values (Runtime)
 * ======================================================================== */

typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_LIST,
    VAL_DICT,
    VAL_FUNC,
    VAL_BUILTIN,
    VAL_RULE,        // file interception rule
    VAL_BYTES        // for embedded binary
} script_val_type;

typedef struct script_val script_val;
typedef struct script_env script_env;
typedef script_val *(*script_builtin_fn)(script_env *env, script_val **args, int argc);

struct script_val {
    script_val_type type;
    int refcount;

    union {
        bool boolean;
        int64_t integer;
        double floating;

        struct {
            char *data;
            int length;
        } string;

        struct {
            script_val **items;
            int count;
            int capacity;
        } list;

        struct {
            char **keys;
            script_val **values;
            int count;
            int capacity;
        } dict;

        struct {
            char *name;
            char **params;
            int param_count;
            script_node *body;
            script_env *closure;
        } func;

        struct {
            const char *name;
            script_builtin_fn fn;
        } builtin;

        struct {
            char *name;
            char *match_type;    // "order", "match", "regex"
            char *match_value;
            script_val *handler; // function to call
            char *resolved_path; // last resolved path
        } rule;

        struct {
            uint8_t *data;
            size_t length;
        } bytes;
    };
};

/* ========================================================================
 * Environment (Runtime)
 * ======================================================================== */

struct script_env {
    script_env *parent;

    // Variables
    char **names;
    script_val **values;
    int var_count;
    int var_capacity;

    // Return flag (set when return statement is executed)
    bool returning;
    script_val *return_value;

    // Script metadata
    char *script_path;
    char *script_dir;
    uint8_t *embedded_binary;
    size_t embedded_size;

    // Parsed arguments (opts object)
    script_val *opts;

    // File interception rules
    script_val **read_rules;
    int read_rule_count;
    script_val **write_rules;
    int write_rule_count;
    script_val **create_rules;
    int create_rule_count;

    // I/O handlers
    script_val **stdout_handlers;
    int stdout_handler_count;
    script_val **stderr_handlers;
    int stderr_handler_count;

    // State tracking
    int read_order;
    int write_order;
    int create_order;
};

/* ========================================================================
 * Evaluator
 * ======================================================================== */

script_env *script_env_new(script_env *parent);
void script_env_free(script_env *env);
void script_env_set(script_env *env, const char *name, script_val *val);
script_val *script_env_get(script_env *env, const char *name);

script_val *script_eval(script_env *env, script_node *node);
script_val *script_eval_file(const char *path);

/* ========================================================================
 * Value Helpers
 * ======================================================================== */

script_val *script_val_null(void);
script_val *script_val_bool(bool b);
script_val *script_val_int(int64_t i);
script_val *script_val_float(double f);
script_val *script_val_string(const char *s);
script_val *script_val_string_len(const char *s, int len);
script_val *script_val_list(void);
script_val *script_val_bytes(const uint8_t *data, size_t len);

void script_val_ref(script_val *val);
void script_val_unref(script_val *val);

bool script_val_truthy(script_val *val);
char *script_val_to_string(script_val *val);

void script_list_append(script_val *list, script_val *item);

/* ========================================================================
 * File Interception Interface
 * ======================================================================== */

// Called by emu2 to resolve paths
// Returns allocated string with resolved path, or NULL to use default
char *script_resolve_read(script_env *env, const char *dos_name, int order);
char *script_resolve_write(script_env *env, const char *dos_name, int order);
char *script_resolve_create(script_env *env, const char *dos_name, int order);

/* ========================================================================
 * I/O Handling Interface
 * ======================================================================== */

// Called when DOS program writes to stdout/stderr
void script_handle_stdout(script_env *env, const char *text, int len);
void script_handle_stderr(script_env *env, const char *text, int len);

// Send input to DOS program
void script_send_stdin(script_env *env, const char *text);

/* ========================================================================
 * Main Entry Point
 * ======================================================================== */

int script_main(int argc, char **argv);
