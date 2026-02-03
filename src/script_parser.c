/*
 * script parser - Converts tokens to AST
 */

#include "script.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Node allocation
 * ======================================================================== */

static script_node *node_new(script_node_type type, int line) {
    script_node *node = calloc(1, sizeof(script_node));
    node->type = type;
    node->line = line;
    return node;
}

void script_node_free(script_node *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_STRING:
            free(node->string.value);
            break;
        case NODE_IDENT:
            free(node->ident.name);
            break;
        case NODE_LIST:
        case NODE_BLOCK:
        case NODE_PROGRAM:
            for (int i = 0; i < node->list.count; i++)
                script_node_free(node->list.items[i]);
            free(node->list.items);
            break;
        case NODE_BINARY:
            script_node_free(node->binary.left);
            script_node_free(node->binary.right);
            break;
        case NODE_UNARY:
            script_node_free(node->unary.operand);
            break;
        case NODE_CALL:
            script_node_free(node->call.callee);
            for (int i = 0; i < node->call.arg_count; i++)
                script_node_free(node->call.args[i]);
            free(node->call.args);
            for (int i = 0; i < node->call.kw_count; i++) {
                free(node->call.kwnames[i]);
                script_node_free(node->call.kwvalues[i]);
            }
            free(node->call.kwnames);
            free(node->call.kwvalues);
            break;
        case NODE_INDEX:
            script_node_free(node->index.object);
            script_node_free(node->index.index);
            break;
        case NODE_ATTR:
            script_node_free(node->attr.object);
            free(node->attr.attr);
            break;
        case NODE_ASSIGN:
            free(node->assign.name);
            script_node_free(node->assign.value);
            break;
        case NODE_IF:
            script_node_free(node->if_stmt.condition);
            script_node_free(node->if_stmt.then_block);
            script_node_free(node->if_stmt.else_block);
            break;
        case NODE_FOR:
            free(node->for_stmt.var);
            script_node_free(node->for_stmt.iterable);
            script_node_free(node->for_stmt.body);
            break;
        case NODE_DEF:
            free(node->def.name);
            for (int i = 0; i < node->def.param_count; i++)
                free(node->def.params[i]);
            free(node->def.params);
            script_node_free(node->def.body);
            script_node_free(node->def.decorator);
            break;
        case NODE_RETURN:
            script_node_free(node->return_stmt.value);
            break;
        case NODE_DECORATOR:
            script_node_free(node->decorator.expr);
            script_node_free(node->decorator.target);
            break;
        default:
            break;
    }
    free(node);
}

/* ========================================================================
 * Parser helpers
 * ======================================================================== */

static void advance_token(script_parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = script_lexer_next(&p->lexer);
        if (p->current.type != TOK_ERROR) break;

        p->had_error = true;
        snprintf(p->error, sizeof(p->error), "Line %d: %.*s",
                 p->current.line, p->current.length, p->current.start);
    }
}

static bool check(script_parser *p, script_token_type type) {
    return p->current.type == type;
}

static bool match_token(script_parser *p, script_token_type type) {
    if (!check(p, type)) return false;
    advance_token(p);
    return true;
}

static void skip_newlines(script_parser *p) {
    while (match_token(p, TOK_NEWLINE))
        ;
}

static bool expect(script_parser *p, script_token_type type, const char *msg) {
    if (check(p, type)) {
        advance_token(p);
        return true;
    }
    p->had_error = true;
    snprintf(p->error, sizeof(p->error), "Line %d: %s (got %s)",
             p->current.line, msg, script_token_name(p->current.type));
    return false;
}

static char *token_string(script_token *tok) {
    // Extract string content (without quotes, handle escapes)
    const char *start = tok->start;
    int len = tok->length;

    // Skip quotes
    bool triple = (len >= 6 && start[0] == start[1] && start[1] == start[2]);
    if (triple) {
        start += 3;
        len -= 6;
    } else {
        start += 1;
        len -= 2;
    }

    char *result = malloc(len + 1);
    char *out = result;

    for (int i = 0; i < len; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            i++;
            switch (start[i]) {
                case 'n': *out++ = '\n'; break;
                case 't': *out++ = '\t'; break;
                case 'r': *out++ = '\r'; break;
                case '\\': *out++ = '\\'; break;
                case '"': *out++ = '"'; break;
                case '\'': *out++ = '\''; break;
                default: *out++ = start[i]; break;
            }
        } else {
            *out++ = start[i];
        }
    }
    *out = '\0';
    return result;
}

static char *token_ident(script_token *tok) {
    return strndup(tok->start, tok->length);
}

/* ========================================================================
 * Expression parsing (precedence climbing)
 * ======================================================================== */

static script_node *parse_expression(script_parser *p);
static script_node *parse_block(script_parser *p);

static script_node *parse_primary(script_parser *p) {
    if (match_token(p, TOK_INTEGER)) {
        script_node *node = node_new(NODE_INTEGER, p->previous.line);
        node->integer.value = p->previous.int_val;
        return node;
    }

    if (match_token(p, TOK_FLOAT)) {
        script_node *node = node_new(NODE_FLOAT, p->previous.line);
        node->floating.value = p->previous.float_val;
        return node;
    }

    if (match_token(p, TOK_STRING)) {
        script_node *node = node_new(NODE_STRING, p->previous.line);
        node->string.value = token_string(&p->previous);
        return node;
    }

    if (match_token(p, TOK_TRUE)) {
        script_node *node = node_new(NODE_BOOL, p->previous.line);
        node->boolean.value = true;
        return node;
    }

    if (match_token(p, TOK_FALSE)) {
        script_node *node = node_new(NODE_BOOL, p->previous.line);
        node->boolean.value = false;
        return node;
    }

    if (match_token(p, TOK_NULL)) {
        return node_new(NODE_NULL, p->previous.line);
    }

    if (match_token(p, TOK_IDENT)) {
        script_node *node = node_new(NODE_IDENT, p->previous.line);
        node->ident.name = token_ident(&p->previous);
        return node;
    }

    if (match_token(p, TOK_LPAREN)) {
        script_node *expr = parse_expression(p);
        expect(p, TOK_RPAREN, "Expected ')' after expression");
        return expr;
    }

    if (match_token(p, TOK_LBRACKET)) {
        // List literal
        script_node *node = node_new(NODE_LIST, p->previous.line);
        node->list.items = NULL;
        node->list.count = 0;

        if (!check(p, TOK_RBRACKET)) {
            do {
                skip_newlines(p);
                script_node *item = parse_expression(p);
                node->list.items = realloc(node->list.items,
                    (node->list.count + 1) * sizeof(script_node *));
                node->list.items[node->list.count++] = item;
                skip_newlines(p);
            } while (match_token(p, TOK_COMMA));
        }
        expect(p, TOK_RBRACKET, "Expected ']' after list");
        return node;
    }

    p->had_error = true;
    snprintf(p->error, sizeof(p->error), "Line %d: Expected expression, got %s",
             p->current.line, script_token_name(p->current.type));
    return node_new(NODE_NULL, p->current.line);
}

static script_node *parse_postfix(script_parser *p) {
    script_node *expr = parse_primary(p);

    for (;;) {
        if (match_token(p, TOK_LPAREN)) {
            // Function call
            script_node *call = node_new(NODE_CALL, p->previous.line);
            call->call.callee = expr;
            call->call.args = NULL;
            call->call.arg_count = 0;
            call->call.kwnames = NULL;
            call->call.kwvalues = NULL;
            call->call.kw_count = 0;

            if (!check(p, TOK_RPAREN)) {
                do {
                    skip_newlines(p);

                    // Check for keyword argument
                    if (check(p, TOK_IDENT) && p->lexer.current[0] == '=') {
                        // Lookahead for name=value
                        advance_token(p);
                        char *name = token_ident(&p->previous);
                        if (match_token(p, TOK_ASSIGN)) {
                            script_node *val = parse_expression(p);
                            call->call.kwnames = realloc(call->call.kwnames,
                                (call->call.kw_count + 1) * sizeof(char *));
                            call->call.kwvalues = realloc(call->call.kwvalues,
                                (call->call.kw_count + 1) * sizeof(script_node *));
                            call->call.kwnames[call->call.kw_count] = name;
                            call->call.kwvalues[call->call.kw_count] = val;
                            call->call.kw_count++;
                            skip_newlines(p);
                            continue;
                        }
                        // Not a keyword arg, treat as identifier
                        script_node *arg = node_new(NODE_IDENT, p->previous.line);
                        arg->ident.name = name;
                        call->call.args = realloc(call->call.args,
                            (call->call.arg_count + 1) * sizeof(script_node *));
                        call->call.args[call->call.arg_count++] = arg;
                    } else {
                        script_node *arg = parse_expression(p);
                        call->call.args = realloc(call->call.args,
                            (call->call.arg_count + 1) * sizeof(script_node *));
                        call->call.args[call->call.arg_count++] = arg;
                    }
                    skip_newlines(p);
                } while (match_token(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN, "Expected ')' after arguments");
            expr = call;
        } else if (match_token(p, TOK_LBRACKET)) {
            // Index
            script_node *idx = node_new(NODE_INDEX, p->previous.line);
            idx->index.object = expr;
            idx->index.index = parse_expression(p);
            expect(p, TOK_RBRACKET, "Expected ']' after index");
            expr = idx;
        } else if (match_token(p, TOK_DOT)) {
            // Attribute access
            expect(p, TOK_IDENT, "Expected attribute name after '.'");
            script_node *attr = node_new(NODE_ATTR, p->previous.line);
            attr->attr.object = expr;
            attr->attr.attr = token_ident(&p->previous);
            expr = attr;
        } else {
            break;
        }
    }

    return expr;
}

static script_node *parse_unary(script_parser *p) {
    if (match_token(p, TOK_MINUS) || match_token(p, TOK_NOT)) {
        script_token_type op = p->previous.type;
        script_node *node = node_new(NODE_UNARY, p->previous.line);
        node->unary.op = op;
        node->unary.operand = parse_unary(p);
        return node;
    }
    return parse_postfix(p);
}

static script_node *parse_factor(script_parser *p) {
    script_node *left = parse_unary(p);

    while (match_token(p, TOK_STAR) || match_token(p, TOK_SLASH) ||
           match_token(p, TOK_PERCENT)) {
        script_token_type op = p->previous.type;
        script_node *node = node_new(NODE_BINARY, p->previous.line);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = parse_unary(p);
        left = node;
    }
    return left;
}

static script_node *parse_term(script_parser *p) {
    script_node *left = parse_factor(p);

    while (match_token(p, TOK_PLUS) || match_token(p, TOK_MINUS)) {
        script_token_type op = p->previous.type;
        script_node *node = node_new(NODE_BINARY, p->previous.line);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = parse_factor(p);
        left = node;
    }
    return left;
}

static script_node *parse_comparison(script_parser *p) {
    script_node *left = parse_term(p);

    while (match_token(p, TOK_LT) || match_token(p, TOK_LE) ||
           match_token(p, TOK_GT) || match_token(p, TOK_GE) ||
           match_token(p, TOK_EQ) || match_token(p, TOK_NE)) {
        script_token_type op = p->previous.type;
        script_node *node = node_new(NODE_BINARY, p->previous.line);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = parse_term(p);
        left = node;
    }
    return left;
}

static script_node *parse_and(script_parser *p) {
    script_node *left = parse_comparison(p);

    while (match_token(p, TOK_AND)) {
        script_node *node = node_new(NODE_BINARY, p->previous.line);
        node->binary.op = TOK_AND;
        node->binary.left = left;
        node->binary.right = parse_comparison(p);
        left = node;
    }
    return left;
}

static script_node *parse_or(script_parser *p) {
    script_node *left = parse_and(p);

    while (match_token(p, TOK_OR)) {
        script_node *node = node_new(NODE_BINARY, p->previous.line);
        node->binary.op = TOK_OR;
        node->binary.left = left;
        node->binary.right = parse_and(p);
        left = node;
    }
    return left;
}

static script_node *parse_expression(script_parser *p) {
    return parse_or(p);
}

/* ========================================================================
 * Statement parsing
 * ======================================================================== */

static script_node *parse_statement(script_parser *p);

static script_node *parse_block(script_parser *p) {
    script_node *block = node_new(NODE_BLOCK, p->current.line);
    block->list.items = NULL;
    block->list.count = 0;

    if (!match_token(p, TOK_INDENT)) {
        p->had_error = true;
        snprintf(p->error, sizeof(p->error),
                 "Line %d: Expected indented block", p->current.line);
        return block;
    }

    while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT) || check(p, TOK_EOF)) break;

        script_node *stmt = parse_statement(p);
        block->list.items = realloc(block->list.items,
            (block->list.count + 1) * sizeof(script_node *));
        block->list.items[block->list.count++] = stmt;
    }

    expect(p, TOK_DEDENT, "Expected dedent after block");
    return block;
}

static script_node *parse_if(script_parser *p) {
    script_node *node = node_new(NODE_IF, p->previous.line);
    node->if_stmt.condition = parse_expression(p);
    expect(p, TOK_COLON, "Expected ':' after if condition");
    skip_newlines(p);
    node->if_stmt.then_block = parse_block(p);

    skip_newlines(p);
    if (match_token(p, TOK_ELIF)) {
        node->if_stmt.else_block = parse_if(p);
    } else if (match_token(p, TOK_ELSE)) {
        expect(p, TOK_COLON, "Expected ':' after else");
        skip_newlines(p);
        node->if_stmt.else_block = parse_block(p);
    } else {
        node->if_stmt.else_block = NULL;
    }
    return node;
}

static script_node *parse_for(script_parser *p) {
    script_node *node = node_new(NODE_FOR, p->previous.line);
    expect(p, TOK_IDENT, "Expected variable name after 'for'");
    node->for_stmt.var = token_ident(&p->previous);
    expect(p, TOK_IN, "Expected 'in' after for variable");
    node->for_stmt.iterable = parse_expression(p);
    expect(p, TOK_COLON, "Expected ':' after for expression");
    skip_newlines(p);
    node->for_stmt.body = parse_block(p);
    return node;
}

static script_node *parse_def(script_parser *p, script_node *decorator) {
    script_node *node = node_new(NODE_DEF, p->previous.line);
    node->def.decorator = decorator;

    expect(p, TOK_IDENT, "Expected function name");
    node->def.name = token_ident(&p->previous);

    expect(p, TOK_LPAREN, "Expected '(' after function name");
    node->def.params = NULL;
    node->def.param_count = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            expect(p, TOK_IDENT, "Expected parameter name");
            node->def.params = realloc(node->def.params,
                (node->def.param_count + 1) * sizeof(char *));
            node->def.params[node->def.param_count++] =
                token_ident(&p->previous);
        } while (match_token(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "Expected ')' after parameters");
    expect(p, TOK_COLON, "Expected ':' after function signature");
    skip_newlines(p);
    node->def.body = parse_block(p);
    return node;
}

static script_node *parse_return(script_parser *p) {
    script_node *node = node_new(NODE_RETURN, p->previous.line);
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
        node->return_stmt.value = parse_expression(p);
    } else {
        node->return_stmt.value = NULL;
    }
    return node;
}

static script_node *parse_statement(script_parser *p) {
    // Decorator
    if (match_token(p, TOK_AT)) {
        script_node *deco_expr = parse_expression(p);
        skip_newlines(p);
        expect(p, TOK_DEF, "Expected 'def' after decorator");
        return parse_def(p, deco_expr);
    }

    if (match_token(p, TOK_IF))
        return parse_if(p);

    if (match_token(p, TOK_FOR))
        return parse_for(p);

    if (match_token(p, TOK_DEF))
        return parse_def(p, NULL);

    if (match_token(p, TOK_RETURN))
        return parse_return(p);

    // Assignment or expression statement
    script_node *expr = parse_expression(p);

    // Check for assignment: name = value
    if (expr->type == NODE_IDENT && match_token(p, TOK_ASSIGN)) {
        script_node *node = node_new(NODE_ASSIGN, expr->line);
        node->assign.name = expr->ident.name;
        expr->ident.name = NULL;  // Transfer ownership
        script_node_free(expr);
        node->assign.value = parse_expression(p);
        return node;
    }

    // Expression statement
    script_node *node = node_new(NODE_EXPR_STMT, expr->line);
    node->list.items = malloc(sizeof(script_node *));
    node->list.items[0] = expr;
    node->list.count = 1;
    return node;
}

/* ========================================================================
 * Top-level parsing
 * ======================================================================== */

void script_parser_init(script_parser *p, const char *source) {
    script_lexer_init(&p->lexer, source);
    p->had_error = false;
    p->error[0] = '\0';
    advance_token(p);
}

script_node *script_parse(script_parser *p) {
    script_node *program = node_new(NODE_PROGRAM, 1);
    program->list.items = NULL;
    program->list.count = 0;

    skip_newlines(p);

    while (!check(p, TOK_EOF) && !check(p, TOK_END_MARKER)) {
        script_node *stmt = parse_statement(p);
        program->list.items = realloc(program->list.items,
            (program->list.count + 1) * sizeof(script_node *));
        program->list.items[program->list.count++] = stmt;

        if (p->had_error) break;

        // Block statements (if, for, def) consume their own newlines,
        // so we only require newline after non-block statements
        bool is_block_stmt = (stmt->type == NODE_IF ||
                              stmt->type == NODE_FOR ||
                              stmt->type == NODE_DEF);

        if (!is_block_stmt &&
            !check(p, TOK_EOF) && !check(p, TOK_END_MARKER) &&
            !check(p, TOK_NEWLINE) && !check(p, TOK_DEDENT)) {
            p->had_error = true;
            snprintf(p->error, sizeof(p->error),
                     "Line %d: Expected newline after statement",
                     p->current.line);
            break;
        }
        skip_newlines(p);
    }

    return program;
}
