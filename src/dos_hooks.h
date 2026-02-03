/*
 * dos_hooks.h - Hook interface for DOS operations
 *
 * This provides an optional hook mechanism for intercepting DOS
 * operations. When hooks are NULL (the default), emu2 behaves normally.
 * When set, hooks can override behavior for file and program operations.
 *
 * This enables tools like script to intercept operations without
 * modifying emu2's core behavior.
 */

#ifndef DOS_HOOKS_H
#define DOS_HOOKS_H

#include <stdint.h>
#include "os.h"  /* For NORETURN */

/*
 * File open modes passed to the hook:
 */
#define DOS_OPEN_READ    0   /* Open existing file for reading */
#define DOS_OPEN_WRITE   1   /* Open existing file for writing */
#define DOS_OPEN_RW      2   /* Open existing file for read/write */
#define DOS_CREATE       3   /* Create new file (truncate if exists) */
#define DOS_CREATE_NEW   4   /* Create new file (fail if exists) */
#define DOS_FIND_FIRST   5   /* Find first matching file */

/*
 * Special return value to deny/fail the operation
 */
#define DOS_HOOK_DENY ((char*)(intptr_t)-1)

/*
 * Hook function type for file open/create operations.
 *
 * Parameters:
 *   dos_name  - The DOS filename being accessed (e.g., "TEST.SRC")
 *   mode      - One of DOS_OPEN_* or DOS_CREATE*
 *   user_data - Opaque pointer passed through from dos_open_hook_data
 *
 * Returns:
 *   - Allocated Unix path string to use (caller frees)
 *   - NULL to use default path resolution
 *   - DOS_HOOK_DENY to fail the operation with "access denied"
 */
typedef char* (*dos_open_hook_fn)(const char *dos_name, int mode, void *user_data);

/*
 * Hook function type for program exit.
 *
 * Parameters:
 *   exit_code - The DOS program's exit code (0-255)
 *   user_data - Opaque pointer passed through from dos_exit_hook_data
 *
 * If this hook is set, it is called instead of exit().
 * The hook should NOT return - it must either call exit() or longjmp().
 */
typedef void (*dos_exit_hook_fn)(int exit_code, void *user_data);

/*
 * Hook function type for console output.
 *
 * Parameters:
 *   text      - The text being written
 *   len       - Length of text
 *   fd        - File descriptor (1=stdout, 2=stderr)
 *   user_data - Opaque pointer passed through from dos_output_hook_data
 *
 * This hook is called for each line of console output.
 */
typedef void (*dos_output_hook_fn)(const char *text, int len, int fd, void *user_data);

/*
 * Global hook pointers.
 * These default to NULL, meaning no hook is active.
 * Set these before running DOS programs to intercept operations.
 */
extern dos_open_hook_fn dos_open_hook;
extern void *dos_open_hook_data;

extern dos_exit_hook_fn dos_exit_hook;
extern void *dos_exit_hook_data;

extern dos_output_hook_fn dos_output_hook;
extern void *dos_output_hook_data;

/*
 * Call this instead of exit() to allow hook interception.
 * If no hook is set, this calls exit() directly.
 */
NORETURN void dos_program_exit(int code);

#endif /* DOS_HOOKS_H */
