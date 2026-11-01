#ifndef DEADLOCK_PREVENTION_H
#define DEADLOCK_PREVENTION_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define DEADLOCK_PREVENTION_RESOURCE_A_ID 100
#define DEADLOCK_PREVENTION_RESOURCE_B_ID 200
#define DEADLOCK_PREVENTION_MAX_TIMEOUT_MS 300000
#define DEADLOCK_PREVENTION_MIN_TIMEOUT_MS 100
#define DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS 5000
#define DEADLOCK_PREVENTION_NUM_TRANSFERS 1000
#define DEADLOCK_PREVENTION_TRANSFER_AMOUNT 10

typedef enum DeadlockPreventionMode {
    DEADLOCK_PREVENTION_MODE_SAFE_ORDERED = 0,
    DEADLOCK_PREVENTION_MODE_UNSAFE_UNORDERED = 1
} DeadlockPreventionMode;

typedef enum DeadlockPreventionStatus {
    DEADLOCK_PREVENTION_SUCCESS = 0,
    DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT = -1,
    DEADLOCK_PREVENTION_ERR_NOT_INITIALIZED = -2,
    DEADLOCK_PREVENTION_ERR_ALREADY_INITIALIZED = -3,
    DEADLOCK_PREVENTION_ERR_ALLOC_FAILED = -4,
    DEADLOCK_PREVENTION_ERR_MUTEX_INIT_FAILED = -5,
    DEADLOCK_PREVENTION_ERR_BARRIER_INIT_FAILED = -6,
    DEADLOCK_PREVENTION_ERR_THREAD_CREATE_FAILED = -7,
    DEADLOCK_PREVENTION_ERR_LOCK_FAILED = -8,
    DEADLOCK_PREVENTION_ERR_INSUFFICIENT_FUNDS = -9
} DeadlockPreventionStatus;

typedef struct DeadlockPreventionResource {
    int id;
    pthread_mutex_t mutex;
    int mutex_initialized;
    long long balance;
} DeadlockPreventionResource;

typedef struct DeadlockPreventionResult {
    DeadlockPreventionMode mode;
    int resource_a_id;
    int resource_b_id;
    int operations_attempted;
    int operations_completed;
    int operations_failed;
    int lower_id_acquired_first;
    int both_operations_completed;
    int final_state_consistent;
    int total_balance_preserved;
    double duration_seconds;
} DeadlockPreventionResult;

typedef struct DeadlockPreventionExperiment {
    DeadlockPreventionResource resource_a;
    DeadlockPreventionResource resource_b;
    int resources_initialized;
    int timeout_ms;

    pthread_barrier_t barrier;
    int barrier_initialized;

    pthread_t worker1;
    pthread_t worker2;
    int workers_created;

    _Atomic int first_acquired_id;

    DeadlockPreventionResult result;
    int initialized;
} DeadlockPreventionExperiment;

DeadlockPreventionStatus deadlock_prevention_init(
    DeadlockPreventionExperiment *exp,
    int timeout_ms);

DeadlockPreventionStatus deadlock_prevention_run(
    DeadlockPreventionExperiment *exp,
    DeadlockPreventionResult *result);

void deadlock_prevention_destroy(DeadlockPreventionExperiment *exp);

/* Public test-only wrappers for edge-case verification. */
DeadlockPreventionStatus deadlock_prevention_transfer_safe(
    DeadlockPreventionExperiment *exp,
    DeadlockPreventionResource *from,
    DeadlockPreventionResource *to,
    long long amount);

#endif
