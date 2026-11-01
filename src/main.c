#include "application.h"
#include "cli.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    Application app;
    ApplicationStatus status;

    memset(&app, 0, sizeof(app));

    /* --- Initialize the application lifecycle --- */
    status = application_init(&app);
    if (status != APPLICATION_SUCCESS) {
        fprintf(stderr, "application_init failed: %d\n", (int)status);
        return 1;
    }

    /* --- Run the customer-selection CLI --- */
    cli_run(&app);

    /* --- Graceful shutdown --- */
    status = application_shutdown(&app);
    if (status != APPLICATION_SUCCESS) {
        fprintf(stderr, "application_shutdown failed: %d\n", (int)status);
        return 1;
    }

    printf("BankSync shutdown complete.\n");
    return 0;
}
