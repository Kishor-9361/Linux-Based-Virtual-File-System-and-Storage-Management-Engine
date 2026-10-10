#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "minifs - Virtual File System Shell & Administration Tool\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <image>                     Enter interactive REPL\n", prog);
    fprintf(stderr, "  %s <image> -c \"<command>\"       Execute a single command and exit\n", prog);
    fprintf(stderr, "  %s <image> -f <script.txt>     Execute commands from script file\n", prog);
    fprintf(stderr, "  %s --help                      Display this help message\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    const char *img_path = argv[1];
    const char *cmd_to_run = NULL;
    const char *script_file = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cmd_to_run = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            script_file = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    shell_t sh;
    shell_init(&sh);
    strncpy(sh.img_path, img_path, sizeof(sh.img_path) - 1);

    /* Try mounting if image exists */
    vfs_mount(&sh.v, img_path);
    if (sh.v.is_mounted) {
        sh.is_mounted = true;
    }

    int rc = 0;
    if (cmd_to_run) {
        rc = shell_execute_line(&sh, cmd_to_run);
    } else if (script_file) {
        rc = shell_run_script(&sh, script_file);
    } else {
        sh.interactive = true;
        printf("Entering minifs interactive shell for image '%s'\n", img_path);
        printf("Type 'help' for command summary, 'exit' to quit.\n\n");
        rc = shell_run_repl(&sh);
    }

    shell_cleanup(&sh);
    return (rc == 0) ? 0 : 1;
}
