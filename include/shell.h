#ifndef MINIFS_SHELL_H
#define MINIFS_SHELL_H

#include "common.h"
#include "vfs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    vfs_t v;
    char img_path[512];
    bool is_mounted;
    bool exit_requested;
    int last_status;
    bool interactive;
} shell_t;

/* Initialize shell context */
void shell_init(shell_t *sh);

/* Cleanup shell context, unmounting if mounted */
void shell_cleanup(shell_t *sh);

/* Execute a single command line (tokenizes and dispatches) */
int shell_execute_line(shell_t *sh, const char *line);

/* Run an interactive Read-Eval-Print Loop */
int shell_run_repl(shell_t *sh);

/* Run commands from a script file line-by-line */
int shell_run_script(shell_t *sh, const char *script_path);

#ifdef __cplusplus
}
#endif

#endif /* MINIFS_SHELL_H */
