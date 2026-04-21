#include "engine.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --pipe-name <name>    IPC pipe/socket name\n"
        "                        (default: " WAVREC_PIPE_NAME_WIN " / "
                                            WAVREC_PIPE_NAME_POSIX ")\n"
        "  --help\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *pipe_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe-name") == 0 && i + 1 < argc) {
            pipe_name = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    (void)pipe_name; /* TODO: pass to ipc_init */

    /* Force unbuffered stderr so diagnostics appear even if we crash */
    setvbuf(stderr, NULL, _IONBF, 0);

    WavRecEngine *eng = engine_create();
    if (!eng) {
        fprintf(stderr, "engine_create failed\n");
        return 1;
    }

    if (!engine_start(eng)) {
        fprintf(stderr, "engine_start failed\n");
        engine_destroy(eng);
        return 1;
    }

    engine_run(eng);   /* blocks until CMD_SHUTDOWN */

    engine_destroy(eng);
    return 0;
}
