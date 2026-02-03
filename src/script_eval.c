/*
 * script evaluator - Runtime execution of AST
 */

#include "script.h"
#include "dos_hooks.h"
#include "dos.h"
#include "dbg.h"
#include "emu.h"
#include "keyb.h"
#include "timer.h"
#include "video.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>
#include <fnmatch.h>
#include <regex.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ========================================================================
 * Value allocation and management
 * ======================================================================== */

static script_val *val_alloc(script_val_type type) {
    script_val *val = calloc(1, sizeof(script_val));
    val->type = type;
    val->refcount = 1;
    return val;
}

script_val *script_val_null(void) {
    return val_alloc(VAL_NULL);
}

script_val *script_val_bool(bool b) {
    script_val *val = val_alloc(VAL_BOOL);
    val->boolean = b;
    return val;
}

script_val *script_val_int(int64_t i) {
    script_val *val = val_alloc(VAL_INT);
    val->integer = i;
    return val;
}

script_val *script_val_float(double f) {
    script_val *val = val_alloc(VAL_FLOAT);
    val->floating = f;
    return val;
}

script_val *script_val_string(const char *s) {
    return script_val_string_len(s, s ? strlen(s) : 0);
}

script_val *script_val_string_len(const char *s, int len) {
    script_val *val = val_alloc(VAL_STRING);
    val->string.data = malloc(len + 1);
    if (s) memcpy(val->string.data, s, len);
    val->string.data[len] = '\0';
    val->string.length = len;
    return val;
}

script_val *script_val_list(void) {
    script_val *val = val_alloc(VAL_LIST);
    val->list.items = NULL;
    val->list.count = 0;
    val->list.capacity = 0;
    return val;
}

script_val *script_val_bytes(const uint8_t *data, size_t len) {
    script_val *val = val_alloc(VAL_BYTES);
    val->bytes.data = malloc(len);
    memcpy(val->bytes.data, data, len);
    val->bytes.length = len;
    return val;
}

void script_val_ref(script_val *val) {
    if (val) val->refcount++;
}

void script_val_unref(script_val *val) {
    if (!val) return;
    if (--val->refcount > 0) return;

    switch (val->type) {
        case VAL_STRING:
            free(val->string.data);
            break;
        case VAL_LIST:
            for (int i = 0; i < val->list.count; i++)
                script_val_unref(val->list.items[i]);
            free(val->list.items);
            break;
        case VAL_DICT:
            for (int i = 0; i < val->dict.count; i++) {
                free(val->dict.keys[i]);
                script_val_unref(val->dict.values[i]);
            }
            free(val->dict.keys);
            free(val->dict.values);
            break;
        case VAL_FUNC:
            free(val->func.name);
            for (int i = 0; i < val->func.param_count; i++)
                free(val->func.params[i]);
            free(val->func.params);
            // Don't free body (owned by AST) or closure (owned elsewhere)
            break;
        case VAL_RULE:
            free(val->rule.name);
            free(val->rule.match_type);
            free(val->rule.match_value);
            free(val->rule.resolved_path);
            script_val_unref(val->rule.handler);
            break;
        case VAL_BYTES:
            free(val->bytes.data);
            break;
        default:
            break;
    }
    free(val);
}

bool script_val_truthy(script_val *val) {
    if (!val) return false;
    switch (val->type) {
        case VAL_NULL: return false;
        case VAL_BOOL: return val->boolean;
        case VAL_INT: return val->integer != 0;
        case VAL_FLOAT: return val->floating != 0.0;
        case VAL_STRING: return val->string.length > 0;
        case VAL_LIST: return val->list.count > 0;
        default: return true;
    }
}

char *script_val_to_string(script_val *val) {
    if (!val) return strdup("null");

    char buf[64];
    switch (val->type) {
        case VAL_NULL: return strdup("null");
        case VAL_BOOL: return strdup(val->boolean ? "true" : "false");
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)val->integer);
            return strdup(buf);
        case VAL_FLOAT:
            snprintf(buf, sizeof(buf), "%g", val->floating);
            return strdup(buf);
        case VAL_STRING:
            return strdup(val->string.data);
        case VAL_LIST:
            return strdup("[list]");
        case VAL_FUNC:
            return strdup("[function]");
        case VAL_BUILTIN:
            return strdup("[builtin]");
        default:
            return strdup("[value]");
    }
}

void script_list_append(script_val *list, script_val *item) {
    if (list->type != VAL_LIST) return;

    if (list->list.count >= list->list.capacity) {
        int new_cap = list->list.capacity ? list->list.capacity * 2 : 8;
        list->list.items = realloc(list->list.items,
            new_cap * sizeof(script_val *));
        list->list.capacity = new_cap;
    }
    script_val_ref(item);
    list->list.items[list->list.count++] = item;
}

/* ========================================================================
 * Environment
 * ======================================================================== */

script_env *script_env_new(script_env *parent) {
    script_env *env = calloc(1, sizeof(script_env));
    env->parent = parent;
    env->returning = false;
    env->return_value = NULL;
    return env;
}

void script_env_free(script_env *env) {
    if (!env) return;

    for (int i = 0; i < env->var_count; i++) {
        free(env->names[i]);
        script_val_unref(env->values[i]);
    }
    free(env->names);
    free(env->values);

    free(env->script_path);
    free(env->script_dir);
    free(env->embedded_binary);

    script_val_unref(env->opts);
    script_val_unref(env->return_value);

    for (int i = 0; i < env->read_rule_count; i++)
        script_val_unref(env->read_rules[i]);
    free(env->read_rules);

    for (int i = 0; i < env->write_rule_count; i++)
        script_val_unref(env->write_rules[i]);
    free(env->write_rules);

    for (int i = 0; i < env->create_rule_count; i++)
        script_val_unref(env->create_rules[i]);
    free(env->create_rules);

    free(env);
}

void script_env_set(script_env *env, const char *name, script_val *val) {
    // First check if exists in current or parent scope
    for (script_env *e = env; e; e = e->parent) {
        for (int i = 0; i < e->var_count; i++) {
            if (strcmp(e->names[i], name) == 0) {
                script_val_unref(e->values[i]);
                script_val_ref(val);
                e->values[i] = val;
                return;
            }
        }
    }

    // Add new in current env
    if (env->var_count >= env->var_capacity) {
        int new_cap = env->var_capacity ? env->var_capacity * 2 : 8;
        env->names = realloc(env->names, new_cap * sizeof(char *));
        env->values = realloc(env->values, new_cap * sizeof(script_val *));
        env->var_capacity = new_cap;
    }
    env->names[env->var_count] = strdup(name);
    script_val_ref(val);
    env->values[env->var_count] = val;
    env->var_count++;
}

script_val *script_env_get(script_env *env, const char *name) {
    for (script_env *e = env; e; e = e->parent) {
        for (int i = 0; i < e->var_count; i++) {
            if (strcmp(e->names[i], name) == 0)
                return e->values[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * Built-in functions
 * ======================================================================== */

// Forward declarations
static script_val *builtin_join(script_env *env, script_val **args, int argc);
static script_val *builtin_dirname(script_env *env, script_val **args, int argc);
static script_val *builtin_basename(script_env *env, script_val **args, int argc);
static script_val *builtin_extname(script_env *env, script_val **args, int argc);
static script_val *builtin_stripext(script_env *env, script_val **args, int argc);
static script_val *builtin_withext(script_env *env, script_val **args, int argc);
static script_val *builtin_find(script_env *env, script_val **args, int argc);
static script_val *builtin_exists(script_env *env, script_val **args, int argc);
static script_val *builtin_isdir(script_env *env, script_val **args, int argc);
static script_val *builtin_cwd(script_env *env, script_val **args, int argc);
static script_val *builtin_which(script_env *env, script_val **args, int argc);
static script_val *builtin_print(script_env *env, script_val **args, int argc);
static script_val *builtin_eprint(script_env *env, script_val **args, int argc);
static script_val *builtin_die(script_env *env, script_val **args, int argc);
static script_val *builtin_warn(script_env *env, script_val **args, int argc);
static script_val *builtin_fail(script_env *env, script_val **args, int argc);
static script_val *builtin_chr(script_env *env, script_val **args, int argc);
static script_val *builtin_int(script_env *env, script_val **args, int argc);
static script_val *builtin_len(script_env *env, script_val **args, int argc);
static script_val *builtin_arg(script_env *env, script_val **args, int argc);
static script_val *builtin_split(script_env *env, script_val **args, int argc);
static script_val *builtin_send(script_env *env, script_val **args, int argc);
static script_val *builtin_sendline(script_env *env, script_val **args, int argc);
static script_val *builtin_run(script_env *env, script_val **args, int argc);
static script_val *builtin_parse_args(script_env *env, script_val **args, int argc);
static script_val *builtin_exit(script_env *env, script_val **args, int argc);
static script_val *builtin_chdir(script_env *env, script_val **args, int argc);
static script_val *builtin_abspath(script_env *env, script_val **args, int argc);

static struct {
    const char *name;
    script_builtin_fn fn;
} builtins[] = {
    {"join", builtin_join},
    {"dirname", builtin_dirname},
    {"basename", builtin_basename},
    {"extname", builtin_extname},
    {"stripext", builtin_stripext},
    {"withext", builtin_withext},
    {"find", builtin_find},
    {"exists", builtin_exists},
    {"isdir", builtin_isdir},
    {"cwd", builtin_cwd},
    {"which", builtin_which},
    {"print", builtin_print},
    {"eprint", builtin_eprint},
    {"die", builtin_die},
    {"warn", builtin_warn},
    {"fail", builtin_fail},
    {"chr", builtin_chr},
    {"int", builtin_int},
    {"len", builtin_len},
    {"arg", builtin_arg},
    {"split", builtin_split},
    {"send", builtin_send},
    {"sendline", builtin_sendline},
    {"run", builtin_run},
    {"parse_args", builtin_parse_args},
    {"exit", builtin_exit},
    {"chdir", builtin_chdir},
    {"abspath", builtin_abspath},
    {NULL, NULL}
};

static script_val *builtin_join(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 2) return script_val_string("");

    char *s1 = script_val_to_string(args[0]);
    char *s2 = script_val_to_string(args[1]);

    int len = strlen(s1) + strlen(s2) + 2;
    char *result = malloc(len);
    snprintf(result, len, "%s/%s", s1, s2);

    free(s1);
    free(s2);

    script_val *val = script_val_string(result);
    free(result);
    return val;
}

static script_val *builtin_dirname(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_string(".");

    char *path = script_val_to_string(args[0]);
    char *dir = dirname(path);  // Note: modifies path
    script_val *val = script_val_string(dir);
    free(path);
    return val;
}

static script_val *builtin_basename(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_string("");

    char *path = script_val_to_string(args[0]);
    char *base = basename(path);  // Note: may modify path
    script_val *val = script_val_string(base);
    free(path);
    return val;
}

static script_val *builtin_extname(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_string("");

    char *path = script_val_to_string(args[0]);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    script_val *val;

    // Only consider dot after last slash
    if (dot && (!slash || dot > slash))
        val = script_val_string(dot);
    else
        val = script_val_string("");

    free(path);
    return val;
}

static script_val *builtin_stripext(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_string("");

    char *path = script_val_to_string(args[0]);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');

    // Only strip dot after last slash
    if (dot && (!slash || dot > slash))
        *dot = '\0';

    script_val *val = script_val_string(path);
    free(path);
    return val;
}

static script_val *builtin_withext(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 2) return script_val_string("");

    char *path = script_val_to_string(args[0]);
    char *newext = script_val_to_string(args[1]);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');

    // Only strip dot after last slash
    if (dot && (!slash || dot > slash))
        *dot = '\0';

    int len = strlen(path) + strlen(newext) + 1;
    char *result = malloc(len);
    snprintf(result, len, "%s%s", path, newext);

    script_val *val = script_val_string(result);
    free(path);
    free(newext);
    free(result);
    return val;
}

static script_val *builtin_find(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 2) return script_val_null();

    char *filename = script_val_to_string(args[0]);
    script_val *paths = args[1];

    if (paths->type != VAL_LIST) {
        free(filename);
        return script_val_null();
    }

    for (int i = 0; i < paths->list.count; i++) {
        char *dir = script_val_to_string(paths->list.items[i]);
        if (!dir || !*dir) {
            free(dir);
            continue;
        }

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, filename);
        free(dir);

        struct stat st;
        if (stat(full, &st) == 0) {
            free(filename);
            return script_val_string(full);
        }

        // Try lowercase
        char *p = strrchr(full, '/');
        if (p) {
            for (char *c = p + 1; *c; c++)
                if (*c >= 'A' && *c <= 'Z')
                    *c = *c - 'A' + 'a';
        }
        if (stat(full, &st) == 0) {
            free(filename);
            return script_val_string(full);
        }
    }

    free(filename);
    return script_val_null();
}

static script_val *builtin_exists(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_bool(false);

    char *path = script_val_to_string(args[0]);
    struct stat st;
    bool exists = (stat(path, &st) == 0);
    free(path);
    return script_val_bool(exists);
}

static script_val *builtin_isdir(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_bool(false);

    char *path = script_val_to_string(args[0]);
    struct stat st;
    bool isdir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    free(path);
    return script_val_bool(isdir);
}

static script_val *builtin_cwd(script_env *env, script_val **args, int argc) {
    (void)env; (void)args; (void)argc;
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)))
        return script_val_string(buf);
    return script_val_string(".");
}

static script_val *builtin_chdir(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_bool(false);

    char *path = script_val_to_string(args[0]);
    bool success = (chdir(path) == 0);
    free(path);
    return script_val_bool(success);
}

static script_val *builtin_abspath(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_string("");

    char *path = script_val_to_string(args[0]);
    char resolved[PATH_MAX];

    if (realpath(path, resolved)) {
        free(path);
        return script_val_string(resolved);
    }

    // If realpath fails (file doesn't exist), construct path manually
    if (path[0] == '/') {
        script_val *result = script_val_string(path);
        free(path);
        return result;
    }

    // Get current directory and join
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", cwd, path);
        free(path);
        return script_val_string(full);
    }

    script_val *result = script_val_string(path);
    free(path);
    return result;
}

static script_val *builtin_which(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_null();

    char *name = script_val_to_string(args[0]);
    char *path = getenv("PATH");
    if (!path) {
        free(name);
        return script_val_null();
    }

    char *path_copy = strdup(path);
    char *dir = strtok(path_copy, ":");

    while (dir) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, name);

        struct stat st;
        if (stat(full, &st) == 0 && (st.st_mode & S_IXUSR)) {
            free(name);
            free(path_copy);
            return script_val_string(full);
        }
        dir = strtok(NULL, ":");
    }

    free(name);
    free(path_copy);
    return script_val_null();
}

static script_val *builtin_print(script_env *env, script_val **args, int argc) {
    (void)env;
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        char *s = script_val_to_string(args[i]);
        printf("%s", s);
        free(s);
    }
    printf("\n");
    return script_val_null();
}

static script_val *builtin_eprint(script_env *env, script_val **args, int argc) {
    (void)env;
    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(stderr, " ");
        char *s = script_val_to_string(args[i]);
        fprintf(stderr, "%s", s);
        free(s);
    }
    fprintf(stderr, "\n");
    return script_val_null();
}

static script_val *builtin_die(script_env *env, script_val **args, int argc) {
    (void)env;
    for (int i = 0; i < argc; i++) {
        char *s = script_val_to_string(args[i]);
        fprintf(stderr, "%s", s);
        free(s);
    }
    fprintf(stderr, "\n");
    exit(1);
    return script_val_null();  // Never reached
}

static script_val *builtin_warn(script_env *env, script_val **args, int argc) {
    (void)env;
    fprintf(stderr, "Warning: ");
    for (int i = 0; i < argc; i++) {
        char *s = script_val_to_string(args[i]);
        fprintf(stderr, "%s", s);
        free(s);
    }
    fprintf(stderr, "\n");
    return script_val_null();
}

static script_val *builtin_chr(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1 || args[0]->type != VAL_INT)
        return script_val_string("");

    char buf[2] = {(char)args[0]->integer, 0};
    return script_val_string(buf);
}

static script_val *builtin_int(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_int(0);

    script_val *v = args[0];
    switch (v->type) {
        case VAL_INT: return script_val_int(v->integer);
        case VAL_STRING: return script_val_int(atoi(v->string.data));
        default: return script_val_int(0);
    }
}

static script_val *builtin_len(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 1) return script_val_int(0);

    script_val *v = args[0];
    switch (v->type) {
        case VAL_STRING: return script_val_int(v->string.length);
        case VAL_LIST: return script_val_int(v->list.count);
        default: return script_val_int(0);
    }
}

static script_val *builtin_fail(script_env *env, script_val **args, int argc) {
    (void)env;
    // Returns a special failure value for file interception
    // The message is stored for error reporting
    script_val *val = val_alloc(VAL_NULL);
    if (argc > 0) {
        char *msg = script_val_to_string(args[0]);
        fprintf(stderr, "Fail: %s\n", msg);
        free(msg);
    }
    return val;
}

static script_val *builtin_split(script_env *env, script_val **args, int argc) {
    (void)env;
    if (argc < 2) return script_val_list();

    char *str = script_val_to_string(args[0]);
    char *delim = script_val_to_string(args[1]);
    script_val *result = script_val_list();

    if (strlen(delim) == 0) {
        // Split into characters
        for (int i = 0; str[i]; i++) {
            char buf[2] = {str[i], 0};
            script_val *item = script_val_string(buf);
            script_list_append(result, item);
            script_val_unref(item);
        }
    } else {
        char *copy = strdup(str);
        char *tok = strtok(copy, delim);
        while (tok) {
            script_val *item = script_val_string(tok);
            script_list_append(result, item);
            script_val_unref(item);
            tok = strtok(NULL, delim);
        }
        free(copy);
    }

    free(str);
    free(delim);
    return result;
}

/* ========================================================================
 * Argument parsing
 * ======================================================================== */

// Storage for argument definitions during script setup
typedef struct {
    char *short_flag;    // e.g., "-v"
    char *long_flag;     // e.g., "--verbose"
    char *dest;          // destination name in opts
    char *help;          // help text
    bool is_flag;        // boolean flag (no value)
    bool multi;          // can repeat
    bool positional;     // positional argument
    bool required;       // required positional
    char *default_val;   // default value
} arg_def;

static arg_def *arg_defs = NULL;
static int arg_def_count = 0;
static int arg_def_capacity = 0;

static void add_arg_def(arg_def def) {
    if (arg_def_count >= arg_def_capacity) {
        int new_cap = arg_def_capacity ? arg_def_capacity * 2 : 8;
        arg_defs = realloc(arg_defs, new_cap * sizeof(arg_def));
        arg_def_capacity = new_cap;
    }
    arg_defs[arg_def_count++] = def;
}

// arg() builtin - called from script to define an argument
// arg("-v", "--verbose", flag=true, help="...")
// arg("-I", dest="includes", multi=true, help="...")
// arg("source", positional=true, required=true, help="...")
static script_val *builtin_arg(script_env *env, script_val **args, int argc) {
    (void)env;
    arg_def def = {0};

    // Process positional string arguments
    int str_idx = 0;
    for (int i = 0; i < argc; i++) {
        if (args[i]->type == VAL_STRING) {
            char *s = args[i]->string.data;
            if (str_idx == 0) {
                // First string - could be short flag, long flag, or positional name
                if (s[0] == '-' && s[1] == '-') {
                    def.long_flag = strdup(s);
                    def.dest = strdup(s + 2);
                } else if (s[0] == '-') {
                    def.short_flag = strdup(s);
                } else {
                    // Positional argument
                    def.dest = strdup(s);
                    def.positional = true;
                }
            } else if (str_idx == 1 && !def.positional) {
                // Second string - could be long flag
                if (s[0] == '-' && s[1] == '-') {
                    def.long_flag = strdup(s);
                    if (!def.dest) def.dest = strdup(s + 2);
                }
            }
            str_idx++;
        }
    }

    // TODO: Process keyword arguments for flag, multi, positional, required, default, help, dest
    // For now, this is handled through the call node's kw_count and kwnames

    add_arg_def(def);
    return script_val_null();
}

/* ========================================================================
 * Forward declarations
 * ======================================================================== */

static script_val *eval_node(script_env *env, script_node *node);
static void register_rule(script_env *env, script_node *decorator, script_val *func);
static script_val *script_val_dict(void);
static void script_dict_set(script_val *dict, const char *key, script_val *value);

/* ========================================================================
 * Evaluator
 * ========================================================================*/

static script_val *eval_binary(script_env *env, script_node *node) {
    script_val *left = eval_node(env, node->binary.left);
    script_val *right = eval_node(env, node->binary.right);
    script_val *result = NULL;

    switch (node->binary.op) {
        case TOK_PLUS:
            if (left->type == VAL_STRING || right->type == VAL_STRING) {
                char *s1 = script_val_to_string(left);
                char *s2 = script_val_to_string(right);
                int len = strlen(s1) + strlen(s2) + 1;
                char *s = malloc(len);
                snprintf(s, len, "%s%s", s1, s2);
                result = script_val_string(s);
                free(s); free(s1); free(s2);
            } else if (left->type == VAL_LIST && right->type == VAL_LIST) {
                result = script_val_list();
                for (int i = 0; i < left->list.count; i++)
                    script_list_append(result, left->list.items[i]);
                for (int i = 0; i < right->list.count; i++)
                    script_list_append(result, right->list.items[i]);
            } else {
                result = script_val_int(left->integer + right->integer);
            }
            break;

        case TOK_MINUS:
            result = script_val_int(left->integer - right->integer);
            break;

        case TOK_STAR:
            result = script_val_int(left->integer * right->integer);
            break;

        case TOK_SLASH:
            if (right->integer == 0)
                result = script_val_int(0);
            else
                result = script_val_int(left->integer / right->integer);
            break;

        case TOK_PERCENT:
            if (right->integer == 0)
                result = script_val_int(0);
            else
                result = script_val_int(left->integer % right->integer);
            break;

        case TOK_EQ: {
            bool eq = false;
            if (left->type == VAL_NULL && right->type == VAL_NULL)
                eq = true;
            else if (left->type == VAL_STRING && right->type == VAL_STRING)
                eq = strcmp(left->string.data, right->string.data) == 0;
            else if (left->type == VAL_INT && right->type == VAL_INT)
                eq = left->integer == right->integer;
            else if (left->type == VAL_BOOL && right->type == VAL_BOOL)
                eq = left->boolean == right->boolean;
            result = script_val_bool(eq);
            break;
        }

        case TOK_NE: {
            bool eq = false;
            if (left->type == VAL_NULL && right->type == VAL_NULL)
                eq = true;
            else if (left->type == VAL_STRING && right->type == VAL_STRING)
                eq = strcmp(left->string.data, right->string.data) == 0;
            else if (left->type == VAL_INT && right->type == VAL_INT)
                eq = left->integer == right->integer;
            result = script_val_bool(!eq);
            break;
        }

        case TOK_LT:
            result = script_val_bool(left->integer < right->integer);
            break;
        case TOK_LE:
            result = script_val_bool(left->integer <= right->integer);
            break;
        case TOK_GT:
            result = script_val_bool(left->integer > right->integer);
            break;
        case TOK_GE:
            result = script_val_bool(left->integer >= right->integer);
            break;

        case TOK_AND:
            result = script_val_bool(script_val_truthy(left) &&
                                     script_val_truthy(right));
            break;

        case TOK_OR:
            result = script_val_bool(script_val_truthy(left) ||
                                     script_val_truthy(right));
            break;

        default:
            result = script_val_null();
            break;
    }

    script_val_unref(left);
    script_val_unref(right);
    return result;
}

// Helper to get keyword argument value from call node
static const char *get_kw_string(script_node *node, script_env *env,
                                  const char *name, const char *def) {
    for (int i = 0; i < node->call.kw_count; i++) {
        if (strcmp(node->call.kwnames[i], name) == 0) {
            script_val *v = eval_node(env, node->call.kwvalues[i]);
            if (v->type == VAL_STRING) {
                const char *s = strdup(v->string.data);
                script_val_unref(v);
                return s;
            }
            script_val_unref(v);
        }
    }
    return def ? strdup(def) : NULL;
}

static bool get_kw_bool(script_node *node, script_env *env,
                        const char *name, bool def) {
    for (int i = 0; i < node->call.kw_count; i++) {
        if (strcmp(node->call.kwnames[i], name) == 0) {
            script_val *v = eval_node(env, node->call.kwvalues[i]);
            bool result = script_val_truthy(v);
            script_val_unref(v);
            return result;
        }
    }
    return def;
}

// Special handling for arg() calls with keyword arguments
static script_val *eval_arg_call(script_env *env, script_node *node) {
    arg_def def = {0};

    // Process positional string arguments
    int str_idx = 0;
    for (int i = 0; i < node->call.arg_count; i++) {
        script_val *arg = eval_node(env, node->call.args[i]);
        if (arg->type == VAL_STRING) {
            char *s = arg->string.data;
            if (str_idx == 0) {
                if (s[0] == '-' && s[1] == '-') {
                    def.long_flag = strdup(s);
                    def.dest = strdup(s + 2);
                } else if (s[0] == '-') {
                    def.short_flag = strdup(s);
                } else {
                    def.dest = strdup(s);
                    def.positional = true;
                }
            } else if (str_idx == 1 && !def.positional) {
                if (s[0] == '-' && s[1] == '-') {
                    def.long_flag = strdup(s);
                    if (!def.dest) def.dest = strdup(s + 2);
                }
            }
            str_idx++;
        }
        script_val_unref(arg);
    }

    // Process keyword arguments
    const char *dest = get_kw_string(node, env, "dest", NULL);
    if (dest) {
        free(def.dest);
        def.dest = (char *)dest;
    }
    def.help = (char *)get_kw_string(node, env, "help", NULL);
    def.default_val = (char *)get_kw_string(node, env, "default", NULL);
    def.is_flag = get_kw_bool(node, env, "flag", false);
    def.multi = get_kw_bool(node, env, "multi", false);
    def.positional = get_kw_bool(node, env, "positional", def.positional);
    def.required = get_kw_bool(node, env, "required", false);

    add_arg_def(def);
    return script_val_null();
}

static script_val *eval_call(script_env *env, script_node *node) {
    // Check for special arg() call first
    if (node->call.callee->type == NODE_IDENT &&
        strcmp(node->call.callee->ident.name, "arg") == 0) {
        return eval_arg_call(env, node);
    }

    script_val *callee = eval_node(env, node->call.callee);

    // Evaluate positional arguments
    script_val **args = malloc((node->call.arg_count + 1) * sizeof(script_val *));
    for (int i = 0; i < node->call.arg_count; i++)
        args[i] = eval_node(env, node->call.args[i]);

    script_val *result = NULL;

    if (callee->type == VAL_BUILTIN) {
        result = callee->builtin.fn(env, args, node->call.arg_count);
    } else if (callee->type == VAL_FUNC) {
        // Create new environment
        script_env *func_env = script_env_new(callee->func.closure);

        // Bind parameters
        for (int i = 0; i < callee->func.param_count && i < node->call.arg_count; i++)
            script_env_set(func_env, callee->func.params[i], args[i]);

        // Execute body
        result = eval_node(func_env, callee->func.body);

        // Get return value if set
        if (func_env->returning && func_env->return_value) {
            script_val_unref(result);
            result = func_env->return_value;
            script_val_ref(result);
        }

        // Clear return flag (don't propagate to caller)
        func_env->returning = false;
        script_val_unref(func_env->return_value);
        func_env->return_value = NULL;

        script_env_free(func_env);
    } else {
        result = script_val_null();
    }

    // Cleanup
    for (int i = 0; i < node->call.arg_count; i++)
        script_val_unref(args[i]);
    free(args);
    script_val_unref(callee);

    return result;
}

static script_val *eval_node(script_env *env, script_node *node) {
    if (!node) return script_val_null();

    switch (node->type) {
        case NODE_NULL:
            return script_val_null();

        case NODE_BOOL:
            return script_val_bool(node->boolean.value);

        case NODE_INTEGER:
            return script_val_int(node->integer.value);

        case NODE_FLOAT:
            return script_val_float(node->floating.value);

        case NODE_STRING:
            return script_val_string(node->string.value);

        case NODE_LIST: {
            script_val *list = script_val_list();
            for (int i = 0; i < node->list.count; i++) {
                script_val *item = eval_node(env, node->list.items[i]);
                script_list_append(list, item);
                script_val_unref(item);
            }
            return list;
        }

        case NODE_IDENT: {
            const char *name = node->ident.name;

            // Special built-in objects
            if (strcmp(name, "env") == 0) {
                // Return a special marker for environment access
                script_val *v = val_alloc(VAL_DICT);
                v->dict.keys = NULL;
                v->dict.values = NULL;
                v->dict.count = -1;  // Special marker for env
                return v;
            }
            if (strcmp(name, "opts") == 0) {
                // Return the opts object from root environment
                script_env *root = env;
                while (root->parent) root = root->parent;
                if (root->opts) {
                    script_val_ref(root->opts);
                    return root->opts;
                }
                return script_val_null();
            }
            if (strcmp(name, "__file__") == 0) {
                script_env *root = env;
                while (root->parent) root = root->parent;
                if (root->script_path)
                    return script_val_string(root->script_path);
                return script_val_null();
            }
            if (strcmp(name, "__dir__") == 0) {
                script_env *root = env;
                while (root->parent) root = root->parent;
                if (root->script_dir)
                    return script_val_string(root->script_dir);
                return script_val_null();
            }
            if (strcmp(name, "__embedded__") == 0) {
                script_env *root = env;
                while (root->parent) root = root->parent;
                if (root->embedded_binary)
                    return script_val_bytes(root->embedded_binary, root->embedded_size);
                return script_val_null();
            }

            script_val *val = script_env_get(env, name);
            if (val) {
                script_val_ref(val);
                return val;
            }
            // Check builtins
            for (int i = 0; builtins[i].name; i++) {
                if (strcmp(builtins[i].name, name) == 0) {
                    script_val *v = val_alloc(VAL_BUILTIN);
                    v->builtin.name = builtins[i].name;
                    v->builtin.fn = builtins[i].fn;
                    return v;
                }
            }
            return script_val_null();
        }

        case NODE_BINARY:
            return eval_binary(env, node);

        case NODE_UNARY: {
            script_val *operand = eval_node(env, node->unary.operand);
            script_val *result;
            if (node->unary.op == TOK_MINUS) {
                result = script_val_int(-operand->integer);
            } else if (node->unary.op == TOK_NOT) {
                result = script_val_bool(!script_val_truthy(operand));
            } else {
                result = script_val_null();
            }
            script_val_unref(operand);
            return result;
        }

        case NODE_CALL:
            return eval_call(env, node);

        case NODE_INDEX: {
            script_val *obj = eval_node(env, node->index.object);
            script_val *idx = eval_node(env, node->index.index);
            script_val *result = script_val_null();

            if (obj->type == VAL_LIST && idx->type == VAL_INT) {
                int i = (int)idx->integer;
                if (i >= 0 && i < obj->list.count) {
                    result = obj->list.items[i];
                    script_val_ref(result);
                }
            } else if (obj->type == VAL_STRING && idx->type == VAL_INT) {
                int i = (int)idx->integer;
                if (i >= 0 && i < obj->string.length) {
                    char buf[2] = {obj->string.data[i], 0};
                    result = script_val_string(buf);
                }
            }

            script_val_unref(obj);
            script_val_unref(idx);
            return result;
        }

        case NODE_ATTR: {
            script_val *obj = eval_node(env, node->attr.object);
            script_val *result = script_val_null();
            const char *attr = node->attr.attr;

            // Special handling for env object (dict with count == -1)
            if (obj->type == VAL_DICT && obj->dict.count == -1) {
                // Access environment variable
                const char *val = getenv(attr);
                if (val)
                    result = script_val_string(val);
                script_val_unref(obj);
                return result;
            }

            // Handle rule .path access (resolved path from rule)
            if (obj->type == VAL_RULE) {
                if (strcmp(attr, "path") == 0 && obj->rule.resolved_path)
                    result = script_val_string(obj->rule.resolved_path);
                script_val_unref(obj);
                return result;
            }

            // Handle string methods
            if (obj->type == VAL_STRING) {
                if (strcmp(attr, "split") == 0) {
                    // Return a bound method - for now just the string
                    script_val_ref(obj);
                    return obj;
                }
                script_val_unref(obj);
                return result;
            }

            // Regular dict access
            if (obj->type == VAL_DICT) {
                for (int i = 0; i < obj->dict.count; i++) {
                    if (strcmp(obj->dict.keys[i], attr) == 0) {
                        result = obj->dict.values[i];
                        script_val_ref(result);
                        break;
                    }
                }
            }

            script_val_unref(obj);
            return result;
        }

        case NODE_ASSIGN: {
            script_val *val = eval_node(env, node->assign.value);
            script_env_set(env, node->assign.name, val);
            return val;
        }

        case NODE_IF: {
            script_val *cond = eval_node(env, node->if_stmt.condition);
            bool is_true = script_val_truthy(cond);
            script_val_unref(cond);

            script_val *result;
            if (is_true)
                result = eval_node(env, node->if_stmt.then_block);
            else if (node->if_stmt.else_block)
                result = eval_node(env, node->if_stmt.else_block);
            else
                result = script_val_null();
            return result;
        }

        case NODE_FOR: {
            script_val *iterable = eval_node(env, node->for_stmt.iterable);
            script_val *result = script_val_null();

            if (iterable->type == VAL_LIST) {
                for (int i = 0; i < iterable->list.count; i++) {
                    script_env_set(env, node->for_stmt.var, iterable->list.items[i]);
                    script_val_unref(result);
                    result = eval_node(env, node->for_stmt.body);
                }
            }

            script_val_unref(iterable);
            return result;
        }

        case NODE_DEF: {
            script_val *func = val_alloc(VAL_FUNC);
            func->func.name = strdup(node->def.name);
            func->func.params = malloc((node->def.param_count + 1) * sizeof(char *));
            for (int i = 0; i < node->def.param_count; i++)
                func->func.params[i] = strdup(node->def.params[i]);
            func->func.param_count = node->def.param_count;
            func->func.body = node->def.body;
            func->func.closure = env;

            // Check for decorator
            if (node->def.decorator) {
                register_rule(env, node->def.decorator, func);
            } else {
                script_env_set(env, node->def.name, func);
            }
            return func;
        }

        case NODE_DECORATOR: {
            // The decorator node wraps around a function
            // Evaluate the target (function definition)
            script_val *target = eval_node(env, node->decorator.target);
            if (target->type == VAL_FUNC) {
                register_rule(env, node->decorator.expr, target);
            }
            return target;
        }

        case NODE_RETURN: {
            script_val *val;
            if (node->return_stmt.value)
                val = eval_node(env, node->return_stmt.value);
            else
                val = script_val_null();
            // Set return flag on the current environment
            env->returning = true;
            env->return_value = val;
            script_val_ref(val);
            return val;
        }

        case NODE_EXPR_STMT: {
            // Expression statement - just evaluate and return
            if (node->list.count > 0)
                return eval_node(env, node->list.items[0]);
            return script_val_null();
        }

        case NODE_BLOCK:
        case NODE_PROGRAM: {
            script_val *result = script_val_null();
            for (int i = 0; i < node->list.count; i++) {
                script_val_unref(result);
                result = eval_node(env, node->list.items[i]);
                // Check if a return was triggered
                if (env->returning)
                    break;
            }
            return result;
        }

        default:
            return script_val_null();
    }
}

script_val *script_eval(script_env *env, script_node *node) {
    return eval_node(env, node);
}

/* ========================================================================
 * Dict helpers
 * ======================================================================== */

static script_val *script_val_dict(void) {
    script_val *val = val_alloc(VAL_DICT);
    val->dict.keys = NULL;
    val->dict.values = NULL;
    val->dict.count = 0;
    val->dict.capacity = 0;
    return val;
}

static void script_dict_set(script_val *dict, const char *key, script_val *value) {
    if (dict->type != VAL_DICT) return;

    // Check if key exists
    for (int i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.keys[i], key) == 0) {
            script_val_unref(dict->dict.values[i]);
            script_val_ref(value);
            dict->dict.values[i] = value;
            return;
        }
    }

    // Add new key
    if (dict->dict.count >= dict->dict.capacity) {
        int new_cap = dict->dict.capacity ? dict->dict.capacity * 2 : 8;
        dict->dict.keys = realloc(dict->dict.keys, new_cap * sizeof(char *));
        dict->dict.values = realloc(dict->dict.values, new_cap * sizeof(script_val *));
        dict->dict.capacity = new_cap;
    }
    dict->dict.keys[dict->dict.count] = strdup(key);
    script_val_ref(value);
    dict->dict.values[dict->dict.count] = value;
    dict->dict.count++;
}

/* ========================================================================
 * Command-line argument parsing
 * ======================================================================== */

static script_val *parse_cmdline_args(int argc, char **argv) {
    script_val *opts = script_val_dict();

    // Initialize defaults
    for (int i = 0; i < arg_def_count; i++) {
        arg_def *def = &arg_defs[i];
        if (def->dest) {
            if (def->multi) {
                script_dict_set(opts, def->dest, script_val_list());
            } else if (def->is_flag) {
                script_dict_set(opts, def->dest, script_val_bool(false));
            } else if (def->default_val) {
                script_dict_set(opts, def->dest, script_val_string(def->default_val));
            } else {
                script_dict_set(opts, def->dest, script_val_null());
            }
        }
    }

    // Track which positional we're on
    int pos_idx = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        // Check if it matches a defined option
        bool matched = false;
        for (int j = 0; j < arg_def_count; j++) {
            arg_def *def = &arg_defs[j];
            if (def->positional) continue;

            bool is_short = def->short_flag && strcmp(arg, def->short_flag) == 0;
            bool is_long = def->long_flag && strcmp(arg, def->long_flag) == 0;

            if (is_short || is_long) {
                matched = true;
                if (def->is_flag) {
                    script_dict_set(opts, def->dest, script_val_bool(true));
                } else {
                    // Consume next argument as value
                    if (i + 1 < argc) {
                        i++;
                        if (def->multi) {
                            // Find existing list and append
                            for (int k = 0; k < opts->dict.count; k++) {
                                if (strcmp(opts->dict.keys[k], def->dest) == 0) {
                                    script_val *item = script_val_string(argv[i]);
                                    script_list_append(opts->dict.values[k], item);
                                    script_val_unref(item);
                                    break;
                                }
                            }
                        } else {
                            script_dict_set(opts, def->dest, script_val_string(argv[i]));
                        }
                    }
                }
                break;
            }
        }

        if (!matched && arg[0] != '-') {
            // Positional argument
            for (int j = 0; j < arg_def_count; j++) {
                arg_def *def = &arg_defs[j];
                if (!def->positional) continue;

                // Count positional index
                int this_pos = 0;
                for (int k = 0; k < j; k++)
                    if (arg_defs[k].positional) this_pos++;

                if (this_pos == pos_idx) {
                    script_dict_set(opts, def->dest, script_val_string(arg));
                    pos_idx++;
                    matched = true;
                    break;
                }
            }
        }
    }

    return opts;
}

/* ========================================================================
 * Decorator handling
 * ======================================================================== */

static void register_rule(script_env *env, script_node *decorator, script_val *func) {
    if (decorator->type != NODE_CALL) return;
    if (decorator->call.callee->type != NODE_IDENT) return;

    const char *deco_name = decorator->call.callee->ident.name;

    script_val *rule = val_alloc(VAL_RULE);
    rule->rule.name = strdup(func->func.name);
    rule->rule.handler = func;
    script_val_ref(func);
    rule->rule.resolved_path = NULL;

    // Extract match parameters from decorator
    for (int i = 0; i < decorator->call.kw_count; i++) {
        const char *key = decorator->call.kwnames[i];
        script_val *val = eval_node(env, decorator->call.kwvalues[i]);

        if (strcmp(key, "order") == 0) {
            rule->rule.match_type = strdup("order");
            rule->rule.match_value = script_val_to_string(val);
        } else if (strcmp(key, "match") == 0) {
            rule->rule.match_type = strdup("match");
            rule->rule.match_value = script_val_to_string(val);
        } else if (strcmp(key, "regex") == 0) {
            rule->rule.match_type = strdup("regex");
            rule->rule.match_value = script_val_to_string(val);
        }
        script_val_unref(val);
    }

    // Default to match all if no pattern specified
    if (!rule->rule.match_type) {
        rule->rule.match_type = strdup("match");
        rule->rule.match_value = strdup("*");
    }

    // Register in appropriate list
    script_env *root = env;
    while (root->parent) root = root->parent;

    if (strcmp(deco_name, "on_read") == 0) {
        root->read_rules = realloc(root->read_rules,
            (root->read_rule_count + 1) * sizeof(script_val *));
        root->read_rules[root->read_rule_count++] = rule;
    } else if (strcmp(deco_name, "on_write") == 0) {
        root->write_rules = realloc(root->write_rules,
            (root->write_rule_count + 1) * sizeof(script_val *));
        root->write_rules[root->write_rule_count++] = rule;
    } else if (strcmp(deco_name, "on_create") == 0) {
        root->create_rules = realloc(root->create_rules,
            (root->create_rule_count + 1) * sizeof(script_val *));
        root->create_rules[root->create_rule_count++] = rule;
    } else if (strcmp(deco_name, "on_stdout") == 0) {
        root->stdout_handlers = realloc(root->stdout_handlers,
            (root->stdout_handler_count + 1) * sizeof(script_val *));
        root->stdout_handlers[root->stdout_handler_count++] = rule;
    } else if (strcmp(deco_name, "on_stderr") == 0) {
        root->stderr_handlers = realloc(root->stderr_handlers,
            (root->stderr_handler_count + 1) * sizeof(script_val *));
        root->stderr_handlers[root->stderr_handler_count++] = rule;
    } else {
        script_val_unref(rule);
        return;
    }

    // Also register function by name in environment
    script_env_set(env, func->func.name, rule);
}

/* ========================================================================
 * File interception interface
 * ======================================================================== */

static bool match_rule(script_val *rule, const char *filename, int order) {
    if (!rule->rule.match_type) return true;

    if (strcmp(rule->rule.match_type, "order") == 0) {
        const char *spec = rule->rule.match_value;
        // Parse order spec: "1", "2", "2+", "1-3"
        if (strchr(spec, '+')) {
            int min = atoi(spec);
            return order >= min;
        } else if (strchr(spec, '-')) {
            int min, max;
            sscanf(spec, "%d-%d", &min, &max);
            return order >= min && order <= max;
        } else {
            return order == atoi(spec);
        }
    } else if (strcmp(rule->rule.match_type, "match") == 0) {
        // Use FNM_CASEFOLD for case-insensitive matching (DOS filenames are uppercase)
        return fnmatch(rule->rule.match_value, filename, FNM_CASEFOLD) == 0;
    } else if (strcmp(rule->rule.match_type, "regex") == 0) {
        regex_t re;
        if (regcomp(&re, rule->rule.match_value, REG_EXTENDED | REG_NOSUB) != 0)
            return false;
        int result = regexec(&re, filename, 0, NULL, 0);
        regfree(&re);
        return result == 0;
    }
    return false;
}

char *script_resolve_read(script_env *env, const char *dos_name, int order) {
    for (int i = 0; i < env->read_rule_count; i++) {
        script_val *rule = env->read_rules[i];
        if (!match_rule(rule, dos_name, order)) continue;

        // Create context dict
        script_val *ctx = script_val_dict();
        script_dict_set(ctx, "filename", script_val_string(dos_name));
        script_dict_set(ctx, "mode", script_val_string("read"));

        // Call handler
        (void)ctx;  // Used below
        script_env *call_env = script_env_new(rule->rule.handler->func.closure);
        script_env_set(call_env, rule->rule.handler->func.params[0], ctx);

        script_val *result = eval_node(call_env, rule->rule.handler->func.body);
        script_env_free(call_env);
        script_val_unref(ctx);

        if (result && result->type == VAL_STRING) {
            char *path = strdup(result->string.data);
            // Store resolved path on rule
            free(rule->rule.resolved_path);
            rule->rule.resolved_path = strdup(path);
            script_val_unref(result);
            return path;
        }
        script_val_unref(result);
    }
    return NULL;
}

char *script_resolve_write(script_env *env, const char *dos_name, int order) {
    for (int i = 0; i < env->write_rule_count; i++) {
        script_val *rule = env->write_rules[i];
        if (!match_rule(rule, dos_name, order)) continue;

        script_val *ctx = script_val_dict();
        script_dict_set(ctx, "filename", script_val_string(dos_name));
        script_dict_set(ctx, "mode", script_val_string("write"));

        script_env *call_env = script_env_new(rule->rule.handler->func.closure);
        script_env_set(call_env, rule->rule.handler->func.params[0], ctx);

        script_val *result = eval_node(call_env, rule->rule.handler->func.body);
        script_env_free(call_env);
        script_val_unref(ctx);

        if (result && result->type == VAL_STRING) {
            char *path = strdup(result->string.data);
            free(rule->rule.resolved_path);
            rule->rule.resolved_path = strdup(path);
            script_val_unref(result);
            return path;
        }
        script_val_unref(result);
    }
    return NULL;
}

char *script_resolve_create(script_env *env, const char *dos_name, int order) {
    for (int i = 0; i < env->create_rule_count; i++) {
        script_val *rule = env->create_rules[i];
        if (!match_rule(rule, dos_name, order)) continue;

        script_val *ctx = script_val_dict();
        script_dict_set(ctx, "filename", script_val_string(dos_name));
        script_dict_set(ctx, "mode", script_val_string("create"));

        script_env *call_env = script_env_new(rule->rule.handler->func.closure);
        script_env_set(call_env, rule->rule.handler->func.params[0], ctx);

        script_val *result = eval_node(call_env, rule->rule.handler->func.body);
        script_env_free(call_env);
        script_val_unref(ctx);

        if (result && result->type == VAL_STRING) {
            char *path = strdup(result->string.data);
            free(rule->rule.resolved_path);
            rule->rule.resolved_path = strdup(path);
            script_val_unref(result);
            return path;
        }
        script_val_unref(result);
    }
    return NULL;
}

/* ========================================================================
 * I/O Handling
 * ======================================================================== */

// Stdin buffer for send() - will be consumed by DOS program
static char *stdin_buffer = NULL;
static size_t stdin_buffer_len = 0;
static size_t stdin_buffer_cap = 0;

static script_val *builtin_send(script_env *env, script_val **args, int argc) {
    (void)env;
    for (int i = 0; i < argc; i++) {
        char *s = script_val_to_string(args[i]);
        size_t len = strlen(s);

        // Append to stdin buffer
        if (stdin_buffer_len + len >= stdin_buffer_cap) {
            size_t new_cap = (stdin_buffer_cap + len + 1) * 2;
            stdin_buffer = realloc(stdin_buffer, new_cap);
            stdin_buffer_cap = new_cap;
        }
        memcpy(stdin_buffer + stdin_buffer_len, s, len);
        stdin_buffer_len += len;
        stdin_buffer[stdin_buffer_len] = '\0';
        free(s);
    }
    return script_val_null();
}

static script_val *builtin_sendline(script_env *env, script_val **args, int argc) {
    builtin_send(env, args, argc);
    // Append newline
    if (stdin_buffer_len + 1 >= stdin_buffer_cap) {
        stdin_buffer_cap = (stdin_buffer_cap + 2) * 2;
        stdin_buffer = realloc(stdin_buffer, stdin_buffer_cap);
    }
    stdin_buffer[stdin_buffer_len++] = '\n';
    stdin_buffer[stdin_buffer_len] = '\0';
    return script_val_null();
}

void script_send_stdin(script_env *env, const char *text) {
    (void)env;
    size_t len = strlen(text);
    if (stdin_buffer_len + len >= stdin_buffer_cap) {
        size_t new_cap = (stdin_buffer_cap + len + 1) * 2;
        stdin_buffer = realloc(stdin_buffer, new_cap);
        stdin_buffer_cap = new_cap;
    }
    memcpy(stdin_buffer + stdin_buffer_len, text, len);
    stdin_buffer_len += len;
    stdin_buffer[stdin_buffer_len] = '\0';
}

void script_handle_stdout(script_env *env, const char *text, int len) {
    // Try to match against stdout handlers
    for (int i = 0; i < env->stdout_handler_count; i++) {
        script_val *rule = env->stdout_handlers[i];
        if (!rule->rule.match_type) continue;

        bool matched = false;
        regmatch_t matches[10];
        int num_matches = 0;
        memset(matches, -1, sizeof(matches));

        if (strcmp(rule->rule.match_type, "match") == 0) {
            // Simple glob-like match
            char *pattern = rule->rule.match_value;
            if (strcmp(pattern, "*") == 0) {
                matched = true;
            } else if (strstr(text, pattern) != NULL) {
                matched = true;
            }
        } else if (strcmp(rule->rule.match_type, "regex") == 0) {
            regex_t re;
            if (regcomp(&re, rule->rule.match_value, REG_EXTENDED) == 0) {
                if (regexec(&re, text, 10, matches, 0) == 0) {
                    matched = true;
                    // Count valid matches
                    for (int j = 0; j < 10; j++) {
                        if (matches[j].rm_so >= 0)
                            num_matches = j + 1;
                        else
                            break;
                    }
                }
                regfree(&re);
            }
        }

        if (matched) {
            // Create context
            script_val *ctx = script_val_dict();
            script_dict_set(ctx, "text", script_val_string_len(text, len));

            // Add regex groups if applicable
            script_val *groups_list = script_val_list();
            for (int j = 0; j < num_matches; j++) {
                if (matches[j].rm_so >= 0) {
                    int start = matches[j].rm_so;
                    int end = matches[j].rm_eo;
                    script_val *group = script_val_string_len(text + start, end - start);
                    script_list_append(groups_list, group);
                }
            }
            script_dict_set(ctx, "groups", groups_list);

            // Call handler
            if (rule->rule.handler->func.param_count > 0) {
                script_env *call_env = script_env_new(rule->rule.handler->func.closure);
                script_env_set(call_env, rule->rule.handler->func.params[0], ctx);
                script_val *result = eval_node(call_env, rule->rule.handler->func.body);
                script_val_unref(result);
                script_env_free(call_env);
            }
            script_val_unref(ctx);
            return;  // First match wins, output handled by caller
        }
    }
    // Output handled by caller (INT 21h 40 handler)
}

void script_handle_stderr(script_env *env, const char *text, int len) {
    // Try to match against stderr handlers
    for (int i = 0; i < env->stderr_handler_count; i++) {
        script_val *rule = env->stderr_handlers[i];
        if (!rule->rule.match_type) continue;

        bool matched = false;

        if (strcmp(rule->rule.match_type, "match") == 0) {
            char *pattern = rule->rule.match_value;
            if (strcmp(pattern, "*") == 0 || strstr(text, pattern) != NULL) {
                matched = true;
            }
        } else if (strcmp(rule->rule.match_type, "regex") == 0) {
            regex_t re;
            if (regcomp(&re, rule->rule.match_value, REG_EXTENDED) == 0) {
                if (regexec(&re, text, 0, NULL, 0) == 0) {
                    matched = true;
                }
                regfree(&re);
            }
        }

        if (matched && rule->rule.handler->func.param_count > 0) {
            script_val *ctx = script_val_dict();
            script_dict_set(ctx, "text", script_val_string_len(text, len));

            script_env *call_env = script_env_new(rule->rule.handler->func.closure);
            script_env_set(call_env, rule->rule.handler->func.params[0], ctx);
            script_val *result = eval_node(call_env, rule->rule.handler->func.body);
            script_val_unref(result);
            script_env_free(call_env);
            script_val_unref(ctx);
            return;
        }
    }

    // Default to passthrough
    fwrite(text, 1, len, stderr);
}

/* ========================================================================
 * Command line arguments storage
 * ======================================================================== */

static int g_argc = 0;
static char **g_argv = NULL;

static script_val *builtin_parse_args(script_env *env, script_val **args, int argc) {
    (void)args;
    (void)argc;

    // Parse command line using registered arg definitions
    script_val *opts = parse_cmdline_args(g_argc, g_argv);

    // Store in root environment
    script_env *root = env;
    while (root->parent) root = root->parent;
    if (root->opts) {
        script_val_unref(root->opts);
    }
    root->opts = opts;
    script_val_ref(opts);

    return opts;
}

static script_val *builtin_exit(script_env *env, script_val **args, int argc) {
    (void)env;
    int code = 0;
    if (argc > 0 && args[0]->type == VAL_INT) {
        code = (int)args[0]->integer;
    }
    exit(code);
    return script_val_null();  // Never reached
}

/* ========================================================================
 * emu2 hook handler
 * ======================================================================== */

// Global environment pointer for hook callbacks
static script_env *g_current_env = NULL;

// Hook handler called by emu2's dos_open_file() and dos_find_first()
static char *script_open_hook(const char *dos_name, int mode, void *user_data) {
    (void)user_data;

    if (!g_current_env) return NULL;

    script_env *env = g_current_env;
    char *result = NULL;

    // Determine which rules to check based on mode
    switch (mode) {
        case DOS_FIND_FIRST:
            // find_first is a check, not actual read/create - use current order without incrementing
            // Try read rules first, then create rules (DOS programs often check if output exists)
            result = script_resolve_read(env, dos_name, env->read_order + 1);
            if (!result) {
                result = script_resolve_create(env, dos_name, env->create_order + 1);
            }
            break;
        case DOS_OPEN_READ:
            env->read_order++;
            result = script_resolve_read(env, dos_name, env->read_order);
            break;
        case DOS_OPEN_WRITE:
        case DOS_OPEN_RW:
            env->write_order++;
            result = script_resolve_write(env, dos_name, env->write_order);
            break;
        case DOS_CREATE:
        case DOS_CREATE_NEW:
            env->create_order++;
            result = script_resolve_create(env, dos_name, env->create_order);
            break;
    }

    return result;
}

/* ========================================================================
 * Output hook handler
 * ========================================================================*/

static void script_output_hook(const char *text, int len, int fd, void *user_data) {
    script_env *env = (script_env *)user_data;
    if (!env) return;

    // Call appropriate handler based on fd
    if (fd == 1) {
        script_handle_stdout(env, text, len);
    } else if (fd == 2) {
        script_handle_stderr(env, text, len);
    }
}

/* ========================================================================
 * Run DOS program
 * ========================================================================*/

// For capturing DOS program exit
static jmp_buf run_exit_jmp;
static int run_exit_code;
static volatile int run_exit_requested;

// Called by timer signal during run
extern volatile int exit_cpu;

static void run_timer_alarm(int x) {
    (void)x;
    exit_cpu = 1;
}

static void run_signal_handler(int x) {
    (void)x;
    run_exit_requested = 1;
    exit_cpu = 1;
}

// Exit hook - called by dos_program_exit()
static void run_exit_hook(int code, void *user_data) {
    (void)user_data;
    run_exit_code = code;
    longjmp(run_exit_jmp, 1);
}

// Initialize BIOS memory (simplified version from main.c)
static void run_init_bios_mem(void) {
    memory[0x413] = 0x80; // RAM size: 640k
    memory[0x414] = 0x02;
    // INT-19h instruction at FFFF:0000
    memory[0xFFFF0] = 0xCB;
    memory[0xFFFF1] = 0x19;
    // BIOS date at F000:FFF5
    memory[0xFFFF5] = 0x30;
    memory[0xFFFF6] = 0x31;
    memory[0xFFFF7] = 0x2F;
    memory[0xFFFF8] = 0x30;
    memory[0xFFFF9] = 0x31;
    memory[0xFFFFA] = 0x2F;
    memory[0xFFFFB] = 0x31;
    memory[0xFFFFC] = 0x37;
    update_timer();
}

static script_val *builtin_run(script_env *env, script_val **args, int argc) {
    if (argc < 1) {
        fprintf(stderr, "run() requires at least one argument (executable path)\n");
        exit(1);
    }

    // Get executable path
    char *exe_path = NULL;
    bool temp_file = false;

    if (args[0]->type == VAL_STRING) {
        exe_path = strdup(args[0]->string.data);
    } else if (args[0]->type == VAL_BYTES) {
        // Embedded binary - write to temp file
        char template[] = "/tmp/script_XXXXXX";
        int fd = mkstemp(template);
        if (fd < 0) {
            fprintf(stderr, "Failed to create temp file for embedded binary\n");
            exit(1);
        }
        write(fd, args[0]->bytes.data, args[0]->bytes.length);
        close(fd);
        exe_path = strdup(template);
        temp_file = true;
    } else {
        fprintf(stderr, "run() first argument must be a path or embedded binary\n");
        exit(1);
    }

    // Build argv for init_dos
    int dos_argc = argc;
    char **dos_argv = malloc((dos_argc + 1) * sizeof(char *));
    dos_argv[0] = exe_path;
    for (int i = 1; i < argc; i++) {
        dos_argv[i] = script_val_to_string(args[i]);
    }
    dos_argv[dos_argc] = NULL;

    // Find root environment
    script_env *root = env;
    while (root->parent) root = root->parent;

    // Reset order counters for this run
    root->read_order = 0;
    root->write_order = 0;
    root->create_order = 0;

    // Set up global env for hooks
    g_current_env = root;

    // Set up file open hook
    dos_open_hook = script_open_hook;
    dos_open_hook_data = root;

    // Set up exit hook to catch DOS program termination
    dos_exit_hook = run_exit_hook;
    dos_exit_hook_data = NULL;

    // Set up output hook for stdout/stderr handlers
    dos_output_hook = script_output_hook;
    dos_output_hook_data = root;

    // Print command if verbose
    script_val *verbose = script_env_get(env, "verbose");
    if (verbose && script_val_truthy(verbose)) {
        fprintf(stderr, "Running:");
        for (int i = 0; i < dos_argc; i++) {
            fprintf(stderr, " %s", dos_argv[i]);
        }
        fprintf(stderr, "\n");
    }

    // Initialize emulator subsystems
    init_debug(dos_argv[0]);
    init_cpu();
    init_dos(dos_argc, dos_argv);

    // Set up signal handlers
    struct sigaction timer_action, sig_action;
    sig_action.sa_handler = run_signal_handler;
    timer_action.sa_handler = run_timer_alarm;
    sigemptyset(&sig_action.sa_mask);
    sigemptyset(&timer_action.sa_mask);
    sig_action.sa_flags = timer_action.sa_flags = 0;
    sigaction(SIGALRM, &timer_action, NULL);
    sigaction(SIGHUP, &sig_action, NULL);
    sigaction(SIGINT, &sig_action, NULL);
    sigaction(SIGQUIT, &sig_action, NULL);
    sigaction(SIGPIPE, &sig_action, NULL);
    sigaction(SIGTERM, &sig_action, NULL);

    // Set up timer for ~18.2 Hz tick
    struct itimerval itv;
    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 54925;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 54925;
    setitimer(ITIMER_REAL, &itv, 0);

    // Initialize BIOS and video memory
    run_init_bios_mem();
    video_init_mem();

    // Set up exit point and run emulation
    run_exit_code = 0;
    run_exit_requested = 0;

    if (setjmp(run_exit_jmp) == 0) {
        // Main emulation loop
        while (!run_exit_requested) {
            exit_cpu = 0;
            execute();
            emulator_update();
        }
    }

    // Disable timer
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &itv, 0);

    // Clear hooks
    dos_open_hook = NULL;
    dos_open_hook_data = NULL;
    dos_exit_hook = NULL;
    dos_exit_hook_data = NULL;
    dos_output_hook = NULL;
    dos_output_hook_data = NULL;
    g_current_env = NULL;

    // Cleanup
    for (int i = 1; i < argc; i++) {
        free(dos_argv[i]);
    }
    free(dos_argv);

    // Remove temp file if we created one
    if (temp_file) {
        unlink(exe_path);
    }
    free(exe_path);

    return script_val_int(run_exit_code);
}

/* ========================================================================
 * Base64 decoding
 * ======================================================================== */

// Base64 alphabet (for reference in decode function)

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *b64_decode(const char *input, size_t *out_len) {
    size_t in_len = strlen(input);
    size_t out_size = (in_len / 4) * 3 + 3;
    uint8_t *output = malloc(out_size);
    size_t out_idx = 0;

    for (size_t i = 0; i < in_len; ) {
        // Skip whitespace
        while (i < in_len && (input[i] == '\n' || input[i] == '\r' ||
                              input[i] == ' ' || input[i] == '\t'))
            i++;
        if (i >= in_len) break;

        int v[4];
        for (int j = 0; j < 4 && i < in_len; j++, i++) {
            while (i < in_len && (input[i] == '\n' || input[i] == '\r' ||
                                  input[i] == ' ' || input[i] == '\t'))
                i++;
            if (i >= in_len) { v[j] = -1; continue; }
            v[j] = (input[i] == '=') ? -1 : b64_decode_char(input[i]);
        }

        if (v[0] >= 0 && v[1] >= 0) {
            output[out_idx++] = (v[0] << 2) | (v[1] >> 4);
            if (v[2] >= 0) {
                output[out_idx++] = ((v[1] & 0xF) << 4) | (v[2] >> 2);
                if (v[3] >= 0) {
                    output[out_idx++] = ((v[2] & 0x3) << 6) | v[3];
                }
            }
        }
    }

    *out_len = out_idx;
    return output;
}

/* ========================================================================
 * Main entry point
 * ======================================================================== */

// Read file contents
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

script_val *script_eval_file(const char *path) {
    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "script: cannot read '%s'\n", path);
        return NULL;
    }

    // Parse
    script_parser parser;
    script_parser_init(&parser, source);
    script_node *ast = script_parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "script: parse error: %s\n", parser.error);
        free(source);
        return NULL;
    }

    // Create environment
    script_env *env = script_env_new(NULL);
    env->script_path = strdup(path);

    // Get directory
    char *path_copy = strdup(path);
    env->script_dir = strdup(dirname(path_copy));
    free(path_copy);

    // Look for __END__ and extract embedded binary
    const char *end_marker = strstr(source, "\n__END__\n");
    if (end_marker) {
        const char *b64_start = end_marker + 9;  // Skip "\n__END__\n"
        size_t b64_len;
        env->embedded_binary = b64_decode(b64_start, &b64_len);
        env->embedded_size = b64_len;
    }

    // Evaluate
    script_val *result = script_eval(env, ast);

    // Cleanup (except env which may be needed)
    script_node_free(ast);
    free(source);

    return result;
}

int script_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: script <script> [args...]\n");
        return 1;
    }

    // Store argc/argv for parse_args() builtin
    g_argc = argc - 1;  // Skip script itself
    g_argv = argv + 1;  // argv[0] becomes script name

    const char *script_path = argv[1];
    char *source = read_file(script_path);
    if (!source) {
        fprintf(stderr, "script: cannot read '%s'\n", script_path);
        return 1;
    }

    // Parse
    script_parser parser;
    script_parser_init(&parser, source);
    script_node *ast = script_parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "script: %s\n", parser.error);
        free(source);
        return 1;
    }

    // Create environment
    script_env *env = script_env_new(NULL);
    env->script_path = strdup(script_path);

    char *path_copy = strdup(script_path);
    env->script_dir = strdup(dirname(path_copy));
    free(path_copy);

    // Extract embedded binary
    const char *end_marker = strstr(source, "\n__END__\n");
    if (end_marker) {
        const char *b64_start = end_marker + 9;
        size_t b64_len;
        env->embedded_binary = b64_decode(b64_start, &b64_len);
        env->embedded_size = b64_len;
    }

    // Reset arg definitions
    arg_def_count = 0;

    // Evaluate script (script calls parse_args() when ready)
    script_val *result = script_eval(env, ast);

    script_val_unref(result);
    script_node_free(ast);
    script_env_free(env);
    free(source);

    return 0;
}
