#ifndef CLI_H
#define CLI_H

#include "application.h"
#include "session_manager.h"

/*
 * CliReadLineFn — reads a line of input into buf (at most size bytes,
 * including NUL). Returns the number of characters stored (excluding NUL),
 * 0 on empty line, or -1 on EOF/error.
 */
typedef int (*CliReadLineFn)(char *buf, size_t size, void *ctx);

/*
 * CliWriteFn — writes text to the output sink.
 */
typedef void (*CliWriteFn)(const char *text, void *ctx);

/*
 * CliIO — injectable input/output sink for the CLI. Production uses
 * stdin/stdout; tests provide mock implementations.
 */
typedef struct CliIO {
    CliReadLineFn read_line;
    CliWriteFn write;
    void *ctx;
} CliIO;

/*
 * cli_run — run the BankSync customer-selection CLI using stdin/stdout.
 */
void cli_run(Application *app);

/*
 * cli_run_with_io — run the CLI with an injected IO sink. Used by tests.
 */
void cli_run_with_io(Application *app, const CliIO *io);

#endif /* CLI_H */
