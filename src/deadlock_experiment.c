#include "deadlock_experiment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#else
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#endif

static const char *get_helper_path(void)
{
    const char *override = getenv("DEADLOCK_HELPER_OVERRIDE");
    if (override != NULL && override[0] != '\0') {
        return override;
    }
    return DEADLOCK_HELPER_PATH;
}

static double now_seconds(void)
{
#ifdef _WIN32
    /* Fallback: clock() is sufficient for measuring durations. */
    return (double)clock() / (double)CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

DeadlockStatus deadlock_experiment_init(
    DeadlockExperiment *exp,
    int timeout_ms)
{
    if (exp == NULL) {
        return DEADLOCK_ERR_INVALID_ARGUMENT;
    }
    if (exp->initialized) {
        return DEADLOCK_ERR_ALREADY_INITIALIZED;
    }
    if (timeout_ms < DEADLOCK_MIN_TIMEOUT_MS ||
        timeout_ms > DEADLOCK_MAX_TIMEOUT_MS) {
        return DEADLOCK_ERR_INVALID_ARGUMENT;
    }

    memset(exp, 0, sizeof(*exp));
    exp->timeout_ms = timeout_ms;
    exp->process_handle = DEADLOCK_INVALID_HANDLE;
    exp->process_handle_valid = 0;
#ifdef _WIN32
    exp->process_pid = 0;
#else
    exp->process_pid = DEADLOCK_INVALID_HANDLE;
#endif
    exp->initialized = 1;
    return DEADLOCK_SUCCESS;
}

DeadlockStatus deadlock_experiment_run(
    DeadlockExperiment *exp, DeadlockResult *result)
{
    if (exp == NULL || !exp->initialized) {
        return DEADLOCK_ERR_NOT_INITIALIZED;
    }
    if (result == NULL) {
        return DEADLOCK_ERR_INVALID_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));
    result->timeout_ms = exp->timeout_ms;
    result->outcome = DEADLOCK_OUTCOME_NOT_RUN;
    result->error_code = DEADLOCK_SUCCESS;

#ifdef _WIN32
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);

        const char *helper_path = get_helper_path();
        size_t path_len = strlen(helper_path) + 1;
        char *cmdline = (char *)malloc(path_len);
        if (cmdline == NULL) {
            result->helper_started = 0;
            result->outcome = DEADLOCK_OUTCOME_LAUNCH_FAILED;
            result->error_code = DEADLOCK_ERR_ALLOC_FAILED;
            return DEADLOCK_ERR_PROCESS_LAUNCH_FAILED;
        }
        memcpy(cmdline, helper_path, path_len);

        /* Redirect the helper's stdout/stderr to NUL so its status
           messages do not pollute the parent test-runner output. */
        HANDLE h_null =
            CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h_null == INVALID_HANDLE_VALUE) {
            free(cmdline);
            result->helper_started = 0;
            result->outcome = DEADLOCK_OUTCOME_LAUNCH_FAILED;
            result->error_code = (int)GetLastError();
            return DEADLOCK_ERR_PROCESS_LAUNCH_FAILED;
        }

        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = h_null;
        si.hStdError = h_null;
        ZeroMemory(&pi, sizeof(pi));

        BOOL created = CreateProcessA(
            NULL,   /* application name (use command line) */
            cmdline,/* writable command line */
            NULL,                        /* process security */
            NULL,                        /* thread security */
            FALSE,                       /* do not inherit parent handles */
            CREATE_NO_WINDOW,            /* no new console window */
            NULL,                        /* environment */
            NULL,                        /* current directory */
            &si,
            &pi);

        CloseHandle(h_null);
        free(cmdline);

        if (!created) {
            result->helper_started = 0;
            result->outcome = DEADLOCK_OUTCOME_LAUNCH_FAILED;
            result->error_code = (int)GetLastError();
            return DEADLOCK_ERR_PROCESS_LAUNCH_FAILED;
        }

        result->helper_started = 1;
        exp->process_handle = pi.hProcess;
        exp->process_handle_valid = 1;
        exp->process_pid = pi.dwProcessId;
        CloseHandle(pi.hThread); /* thread handle not needed */

        double start = now_seconds();
        DWORD wait_result =
            WaitForSingleObject(pi.hProcess, (DWORD)exp->timeout_ms);
        double end = now_seconds();
        result->duration_seconds = end - start;

        DWORD exit_code = 0;

        if (wait_result == WAIT_OBJECT_0) {
            /* Helper exited before the timeout — unexpected. */
            if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
                result->exit_code = (int)exit_code;
            }
            result->outcome = DEADLOCK_OUTCOME_UNEXPECTED_EXIT;
            result->error_code = DEADLOCK_ERR_UNEXPECTED_EXIT;
        } else if (wait_result == WAIT_TIMEOUT) {
            /* Expected deadlock: helper still running after timeout. */
            result->deadlock_detected = 1;
            result->outcome = DEADLOCK_OUTCOME_DEADLOCK_DETECTED;

            /* Terminate the deadlocked helper process. */
            if (TerminateProcess(pi.hProcess, 1)) {
                result->watchdog_terminated = 1;
            } else {
                result->watchdog_terminated = 0;
                result->error_code = (int)GetLastError();
                CloseHandle(pi.hProcess);
                exp->process_handle = DEADLOCK_INVALID_HANDLE;
                exp->process_handle_valid = 0;
                return DEADLOCK_ERR_PROCESS_TERMINATE_FAILED;
            }

            /* Wait for termination to complete, then fetch exit code. */
            WaitForSingleObject(pi.hProcess, 5000);
            if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
                result->exit_code = (int)exit_code;
            }

            /* Close the process handle. */
            if (CloseHandle(pi.hProcess)) {
                result->cleanup_succeeded = 1;
            } else {
                result->cleanup_succeeded = 0;
                result->error_code = (int)GetLastError();
            }
            exp->process_handle = DEADLOCK_INVALID_HANDLE;
            exp->process_handle_valid = 0;
        } else {
            /* Wait failed. */
            result->error_code = (int)GetLastError();
            CloseHandle(pi.hProcess);
            exp->process_handle = DEADLOCK_INVALID_HANDLE;
            exp->process_handle_valid = 0;
            return DEADLOCK_ERR_PROCESS_WAIT_FAILED;
        }

        return DEADLOCK_SUCCESS;
    }
#else
    {
        pid_t pid = fork();
        if (pid < 0) {
            result->helper_started = 0;
            result->outcome = DEADLOCK_OUTCOME_LAUNCH_FAILED;
            result->error_code = errno;
            return DEADLOCK_ERR_PROCESS_LAUNCH_FAILED;
        }

        if (pid == 0) {
            /* Child: exec the helper. */
            execl(get_helper_path(), get_helper_path(), (char *)NULL);
            /* If execl returns, it failed. */
            _exit(127);
        }

        /* Parent. */
        result->helper_started = 1;
        exp->process_handle = (DeadlockProcessHandle)pid;
        exp->process_handle_valid = 1;
        exp->process_pid = pid;

        double start = now_seconds();
        int status;
        pid_t waited = 0;
        int expired = 0;

        /* Poll waitpid(WNOHANG) until timeout or child exits. */
        for (;;) {
            waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                break; /* child exited */
            }
            if (waited == -1 && errno != EINTR) {
                break; /* error */
            }
            double elapsed = (now_seconds() - start) * 1000.0;
            if (elapsed >= (double)exp->timeout_ms) {
                expired = 1;
                break;
            }
            struct timespec req = {0, 1000000L}; /* 1 ms poll */
            nanosleep(&req, NULL);
        }
        double end = now_seconds();
        result->duration_seconds = end - start;

        if (waited == pid) {
            /* Helper exited before timeout — unexpected. */
            if (WIFEXITED(status)) {
                result->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result->exit_code = -WTERMSIG(status);
            }
            result->outcome = DEADLOCK_OUTCOME_UNEXPECTED_EXIT;
            result->error_code = DEADLOCK_ERR_UNEXPECTED_EXIT;
            exp->process_handle_valid = 0;
            return DEADLOCK_SUCCESS;
        }

        if (expired) {
            /* Expected deadlock. */
            result->deadlock_detected = 1;
            result->outcome = DEADLOCK_OUTCOME_DEADLOCK_DETECTED;

            /* Terminate the deadlocked helper. */
            if (kill(pid, SIGKILL) == 0) {
                result->watchdog_terminated = 1;
            } else {
                result->watchdog_terminated = 0;
                result->error_code = errno;
                exp->process_handle_valid = 0;
                return DEADLOCK_ERR_PROCESS_TERMINATE_FAILED;
            }

            /* Wait for reaping. */
            waitpid(pid, &status, 0);
            if (WIFSIGNALED(status)) {
                result->exit_code = -WTERMSIG(status);
            }

            result->cleanup_succeeded = 1;
            exp->process_handle_valid = 0;
            return DEADLOCK_SUCCESS;
        }

        /* Unexpected waitpid error. */
        result->error_code = errno;
        exp->process_handle_valid = 0;
        return DEADLOCK_ERR_PROCESS_WAIT_FAILED;
    }
#endif
}

void deadlock_experiment_destroy(DeadlockExperiment *exp)
{
    if (exp == NULL) {
        return;
    }
    if (!exp->initialized) {
        return;
    }

    if (exp->process_handle_valid) {
#ifdef _WIN32
        if (exp->process_handle != DEADLOCK_INVALID_HANDLE) {
            CloseHandle(exp->process_handle);
        }
#else
        /* On Linux the process should already be reaped; nothing to close. */
#endif
        exp->process_handle = DEADLOCK_INVALID_HANDLE;
        exp->process_handle_valid = 0;
    }

    exp->initialized = 0;
}
