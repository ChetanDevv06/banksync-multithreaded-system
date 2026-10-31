#ifndef DEADLOCK_EXPERIMENT_H
#define DEADLOCK_EXPERIMENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE DeadlockProcessHandle;
#define DEADLOCK_INVALID_HANDLE ((DeadlockProcessHandle)NULL)
#else
#include <sys/types.h>
#include <signal.h>
typedef pid_t DeadlockProcessHandle;
#define DEADLOCK_INVALID_HANDLE ((DeadlockProcessHandle)-1)
#endif

#define DEADLOCK_MAX_TIMEOUT_MS 300000
#define DEADLOCK_MIN_TIMEOUT_MS 100
#define DEADLOCK_DEFAULT_TIMEOUT_MS 5000

typedef enum DeadlockStatus {
    DEADLOCK_SUCCESS = 0,
    DEADLOCK_ERR_INVALID_ARGUMENT = -1,
    DEADLOCK_ERR_NOT_INITIALIZED = -2,
    DEADLOCK_ERR_ALREADY_INITIALIZED = -3,
    DEADLOCK_ERR_ALLOC_FAILED = -4,
    DEADLOCK_ERR_MUTEX_INIT_FAILED = -5,
    DEADLOCK_ERR_THREAD_CREATE_FAILED = -6,
    DEADLOCK_ERR_BARRIER_INIT_FAILED = -7,
    DEADLOCK_ERR_PROCESS_LAUNCH_FAILED = -8,
    DEADLOCK_ERR_PROCESS_WAIT_FAILED = -9,
    DEADLOCK_ERR_PROCESS_TERMINATE_FAILED = -10,
    DEADLOCK_ERR_PROCESS_CLEANUP_FAILED = -11,
    DEADLOCK_ERR_UNEXPECTED_EXIT = -12,
} DeadlockStatus;

typedef enum DeadlockOutcome {
    DEADLOCK_OUTCOME_NOT_RUN = 0,
    DEADLOCK_OUTCOME_DEADLOCK_DETECTED = 1,
    DEADLOCK_OUTCOME_UNEXPECTED_EXIT = 2,
    DEADLOCK_OUTCOME_LAUNCH_FAILED = 3,
    DEADLOCK_OUTCOME_TERMINATE_FAILED = 4,
} DeadlockOutcome;

typedef struct DeadlockResult {
    int helper_started;
    int deadlock_detected;
    int watchdog_terminated;
    int cleanup_succeeded;
    int timeout_ms;
    int exit_code;
    int error_code;
    DeadlockOutcome outcome;
    double duration_seconds;
} DeadlockResult;

typedef struct DeadlockExperiment {
    int timeout_ms;
    int initialized;
    DeadlockProcessHandle process_handle;
    int process_handle_valid;
#ifdef _WIN32
    DWORD process_pid;
#else
    pid_t process_pid;
#endif
} DeadlockExperiment;

DeadlockStatus deadlock_experiment_init(
    DeadlockExperiment *exp,
    int timeout_ms);

DeadlockStatus deadlock_experiment_run(
    DeadlockExperiment *exp,
    DeadlockResult *result);

void deadlock_experiment_destroy(DeadlockExperiment *exp);

#endif
