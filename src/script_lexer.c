/*
 * script lexer - Tokenizer for the wrapper script language
 */

#include "script.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

static bool is_at_end(script_lexer *lex) {
    return *lex->current == '\0';
}

static char peek(script_lexer *lex) {
    return *lex->current;
}

static char peek_next(script_lexer *lex) {
    if (is_at_end(lex)) return '\0';
    return lex->current[1];
}

static char advance(script_lexer *lex) {
    lex->current++;
    return lex->current[-1];
}

static bool match(script_lexer *lex, char expected) {
    if (is_at_end(lex)) return false;
    if (*lex->current != expected) return false;
    lex->current++;
    return true;
}

static script_token make_token(script_lexer *lex, script_token_type type,
                               const char *start) {
    script_token tok;
    tok.type = type;
    tok.start = start;
    tok.length = (int)(lex->current - start);
    tok.line = lex->line;
    tok.column = (int)(start - lex->line_start) + 1;
    return tok;
}

static script_token error_token(script_lexer *lex, const char *message) {
    script_token tok;
    tok.type = TOK_ERROR;
    tok.start = message;
    tok.length = strlen(message);
    tok.line = lex->line;
    tok.column = (int)(lex->current - lex->line_start) + 1;
    return tok;
}

static void skip_whitespace(script_lexer *lex) {
    for (;;) {
        char c = peek(lex);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(lex);
                break;
            case '#':
                // Comment until end of line
                while (peek(lex) != '\n' && !is_at_end(lex))
                    advance(lex);
                break;
            default:
                return;
        }
    }
}

static int count_indent(script_lexer *lex) {
    int spaces = 0;
    const char *p = lex->current;
    while (*p == ' ' || *p == '\t') {
        if (*p == ' ') spaces++;
        else spaces += 4;  // Tab = 4 spaces
        p++;
    }
    // Don't count indent for blank lines or comment-only lines
    if (*p == '\n' || *p == '\r' || *p == '#' || *p == '\0')
        return -1;
    return spaces;
}

static script_token_type check_keyword(const char *start, int length) {
    // Check for keywords
    static const struct {
        const char *name;
        int length;
        script_token_type type;
    } keywords[] = {
        {"if", 2, TOK_IF},
        {"elif", 4, TOK_ELIF},
        {"else", 4, TOK_ELSE},
        {"for", 3, TOK_FOR},
        {"in", 2, TOK_IN},
        {"def", 3, TOK_DEF},
        {"return", 6, TOK_RETURN},
        {"and", 3, TOK_AND},
        {"or", 2, TOK_OR},
        {"not", 3, TOK_NOT},
        {"true", 4, TOK_TRUE},
        {"false", 5, TOK_FALSE},
        {"null", 4, TOK_NULL},
        {"__END__", 7, TOK_END_MARKER},
        {NULL, 0, TOK_IDENT}
    };

    for (int i = 0; keywords[i].name != NULL; i++) {
        if (keywords[i].length == length &&
            memcmp(start, keywords[i].name, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static script_token identifier(script_lexer *lex) {
    const char *start = lex->current - 1;
    while (isalnum(peek(lex)) || peek(lex) == '_')
        advance(lex);

    int length = (int)(lex->current - start);
    script_token_type type = check_keyword(start, length);
    return make_token(lex, type, start);
}

static script_token number(script_lexer *lex) {
    const char *start = lex->current - 1;
    bool is_float = false;

    // Handle hex: 0x...
    if (start[0] == '0' && (peek(lex) == 'x' || peek(lex) == 'X')) {
        advance(lex);
        while (isxdigit(peek(lex)))
            advance(lex);
        script_token tok = make_token(lex, TOK_INTEGER, start);
        tok.int_val = strtol(start, NULL, 16);
        return tok;
    }

    while (isdigit(peek(lex)))
        advance(lex);

    // Decimal part
    if (peek(lex) == '.' && isdigit(peek_next(lex))) {
        is_float = true;
        advance(lex);
        while (isdigit(peek(lex)))
            advance(lex);
    }

    // Exponent
    if (peek(lex) == 'e' || peek(lex) == 'E') {
        is_float = true;
        advance(lex);
        if (peek(lex) == '+' || peek(lex) == '-')
            advance(lex);
        while (isdigit(peek(lex)))
            advance(lex);
    }

    script_token tok = make_token(lex, is_float ? TOK_FLOAT : TOK_INTEGER, start);
    if (is_float) {
        tok.float_val = strtod(start, NULL);
    } else {
        tok.int_val = strtol(start, NULL, 10);
    }
    return tok;
}

static script_token string(script_lexer *lex, char quote) {
    const char *start = lex->current - 1;
    bool triple = false;

    // Check for triple-quoted string
    if (peek(lex) == quote && peek_next(lex) == quote) {
        triple = true;
        advance(lex);
        advance(lex);
    }

    while (!is_at_end(lex)) {
        if (triple) {
            if (peek(lex) == quote && peek_next(lex) == quote &&
                lex->current[2] == quote) {
                advance(lex);
                advance(lex);
                advance(lex);
                return make_token(lex, TOK_STRING, start);
            }
            if (peek(lex) == '\n') {
                lex->line++;
                lex->line_start = lex->current + 1;
            }
        } else {
            if (peek(lex) == quote) {
                advance(lex);
                return make_token(lex, TOK_STRING, start);
            }
            if (peek(lex) == '\n') {
                return error_token(lex, "Unterminated string");
            }
        }
        if (peek(lex) == '\\' && peek_next(lex) != '\0') {
            advance(lex);  // Skip backslash
        }
        advance(lex);
    }

    return error_token(lex, "Unterminated string");
}

void script_lexer_init(script_lexer *lex, const char *source) {
    lex->source = source;
    lex->current = source;
    lex->line_start = source;
    lex->line = 1;

    lex->indent_stack[0] = 0;
    lex->indent_depth = 0;
    lex->pending_dedents = 0;
    lex->at_line_start = true;

    lex->error[0] = '\0';
}

script_token script_lexer_next(script_lexer *lex) {
    // Return pending dedents
    if (lex->pending_dedents > 0) {
        lex->pending_dedents--;
        return make_token(lex, TOK_DEDENT, lex->current);
    }

    // Handle indentation at start of line
    if (lex->at_line_start && !is_at_end(lex)) {
        lex->at_line_start = false;
        int indent = count_indent(lex);

        if (indent >= 0) {  // Not a blank/comment line
            int current_indent = lex->indent_stack[lex->indent_depth];

            if (indent > current_indent) {
                // Indent
                if (lex->indent_depth >= 63)
                    return error_token(lex, "Too much indentation");
                lex->indent_depth++;
                lex->indent_stack[lex->indent_depth] = indent;
                // Skip the whitespace
                while (peek(lex) == ' ' || peek(lex) == '\t')
                    advance(lex);
                return make_token(lex, TOK_INDENT, lex->current);
            } else if (indent < current_indent) {
                // Dedent (possibly multiple)
                while (lex->indent_depth > 0 &&
                       lex->indent_stack[lex->indent_depth] > indent) {
                    lex->indent_depth--;
                    lex->pending_dedents++;
                }
                if (lex->indent_stack[lex->indent_depth] != indent) {
                    return error_token(lex, "Inconsistent indentation");
                }
                // Skip the whitespace
                while (peek(lex) == ' ' || peek(lex) == '\t')
                    advance(lex);
                // Return first dedent, rest are pending
                if (lex->pending_dedents > 0) {
                    lex->pending_dedents--;
                    return make_token(lex, TOK_DEDENT, lex->current);
                }
            } else {
                // Same indent level, just skip whitespace
                while (peek(lex) == ' ' || peek(lex) == '\t')
                    advance(lex);
            }
        }
    }

    skip_whitespace(lex);

    if (is_at_end(lex)) {
        // Return any remaining dedents
        if (lex->indent_depth > 0) {
            lex->indent_depth--;
            return make_token(lex, TOK_DEDENT, lex->current);
        }
        return make_token(lex, TOK_EOF, lex->current);
    }

    const char *start = lex->current;
    char c = advance(lex);

    // Identifiers and keywords
    if (isalpha(c) || c == '_')
        return identifier(lex);

    // Numbers
    if (isdigit(c))
        return number(lex);

    switch (c) {
        case '\n':
            lex->line++;
            lex->line_start = lex->current;
            lex->at_line_start = true;
            return make_token(lex, TOK_NEWLINE, start);

        case '"':
        case '\'':
            return string(lex, c);

        case '(': return make_token(lex, TOK_LPAREN, start);
        case ')': return make_token(lex, TOK_RPAREN, start);
        case '[': return make_token(lex, TOK_LBRACKET, start);
        case ']': return make_token(lex, TOK_RBRACKET, start);
        case ',': return make_token(lex, TOK_COMMA, start);
        case '.': return make_token(lex, TOK_DOT, start);
        case ':': return make_token(lex, TOK_COLON, start);
        case '@': return make_token(lex, TOK_AT, start);
        case '+': return make_token(lex, TOK_PLUS, start);
        case '-': return make_token(lex, TOK_MINUS, start);
        case '*': return make_token(lex, TOK_STAR, start);
        case '/': return make_token(lex, TOK_SLASH, start);
        case '%': return make_token(lex, TOK_PERCENT, start);

        case '=':
            return make_token(lex, match(lex, '=') ? TOK_EQ : TOK_ASSIGN, start);
        case '!':
            if (match(lex, '='))
                return make_token(lex, TOK_NE, start);
            return error_token(lex, "Expected '=' after '!'");
        case '<':
            return make_token(lex, match(lex, '=') ? TOK_LE : TOK_LT, start);
        case '>':
            return make_token(lex, match(lex, '=') ? TOK_GE : TOK_GT, start);
    }

    return error_token(lex, "Unexpected character");
}

const char *script_token_name(script_token_type type) {
    static const char *names[] = {
        [TOK_EOF] = "EOF",
        [TOK_NEWLINE] = "NEWLINE",
        [TOK_INDENT] = "INDENT",
        [TOK_DEDENT] = "DEDENT",
        [TOK_STRING] = "STRING",
        [TOK_INTEGER] = "INTEGER",
        [TOK_FLOAT] = "FLOAT",
        [TOK_TRUE] = "TRUE",
        [TOK_FALSE] = "FALSE",
        [TOK_NULL] = "NULL",
        [TOK_IDENT] = "IDENT",
        [TOK_IF] = "IF",
        [TOK_ELIF] = "ELIF",
        [TOK_ELSE] = "ELSE",
        [TOK_FOR] = "FOR",
        [TOK_IN] = "IN",
        [TOK_DEF] = "DEF",
        [TOK_RETURN] = "RETURN",
        [TOK_AND] = "AND",
        [TOK_OR] = "OR",
        [TOK_NOT] = "NOT",
        [TOK_ASSIGN] = "ASSIGN",
        [TOK_EQ] = "EQ",
        [TOK_NE] = "NE",
        [TOK_LT] = "LT",
        [TOK_LE] = "LE",
        [TOK_GT] = "GT",
        [TOK_GE] = "GE",
        [TOK_PLUS] = "PLUS",
        [TOK_MINUS] = "MINUS",
        [TOK_STAR] = "STAR",
        [TOK_SLASH] = "SLASH",
        [TOK_PERCENT] = "PERCENT",
        [TOK_DOT] = "DOT",
        [TOK_COMMA] = "COMMA",
        [TOK_COLON] = "COLON",
        [TOK_LPAREN] = "LPAREN",
        [TOK_RPAREN] = "RPAREN",
        [TOK_LBRACKET] = "LBRACKET",
        [TOK_RBRACKET] = "RBRACKET",
        [TOK_AT] = "AT",
        [TOK_END_MARKER] = "END_MARKER",
        [TOK_ERROR] = "ERROR"
    };

    if (type >= 0 && type <= TOK_ERROR)
        return names[type];
    return "UNKNOWN";
}
