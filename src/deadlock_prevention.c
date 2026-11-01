#include "deadlock_prevention.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESOURCE_A_ID DEADLOCK_PREVENTION_RESOURCE_A_ID
#define RESOURCE_B_ID DEADLOCK_PREVENTION_RESOURCE_B_ID
#define INITIAL_BALANCE 1000000LL
#define TOTAL_BALANCE (INITIAL_BALANCE * 2)

typedef struct {
    int operations_attempted;
    int operations_completed;
    int operations_failed;
} WorkerStats;

typedef struct {
    DeadlockPreventionExperiment *exp;
    int worker_index;
    WorkerStats *stats;
} WorkerArg;

static double now_seconds(void)
{
#ifdef _WIN32
    return (double)clock() / (double)CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

/*
 * Lock both resources in ascending resource-ID order.
 *
 * This is the deadlock-prevention rule: every thread, regardless of the
 * logical direction of its operation, always acquires the lower-ID resource's
 * mutex first, then the higher-ID resource's mutex. Because all threads follow
 * the same global order, a circular wait (the Coffman condition required for
 * deadlock) can never arise.
 */
static int lock_resources_ordered(DeadlockPreventionResource *r1,
                                  DeadlockPreventionResource *r2)
{
    if (r1 == r2 || r1->id == r2->id) {
        /* Same resource: lock once, no self-deadlock possible. */
        return pthread_mutex_lock(&r1->mutex);
    }

    DeadlockPreventionResource *lower =
        (r1->id < r2->id) ? r1 : r2;
    DeadlockPreventionResource *higher =
        (r1->id < r2->id) ? r2 : r1;

    if (pthread_mutex_lock(&lower->mutex) != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&higher->mutex) != 0) {
        pthread_mutex_unlock(&lower->mutex);
        return -1;
    }
    return 0;
}

/* Unlock in reverse acquisition order: higher-ID first, then lower-ID. */
static int unlock_resources_ordered(DeadlockPreventionResource *r1,
                                    DeadlockPreventionResource *r2)
{
    if (r1 == r2 || r1->id == r2->id) {
        return pthread_mutex_unlock(&r1->mutex);
    }

    DeadlockPreventionResource *lower =
        (r1->id < r2->id) ? r1 : r2;
    DeadlockPreventionResource *higher =
        (r1->id < r2->id) ? r2 : r1;

    pthread_mutex_unlock(&higher->mutex);
    pthread_mutex_unlock(&lower->mutex);
    return 0;
}

/* Transfer amount from 'from' to 'to' using ordered locking. */
static int transfer_ordered(DeadlockPreventionExperiment *exp,
                            DeadlockPreventionResource *from,
                            DeadlockPreventionResource *to,
                            long long amount)
{
    if (lock_resources_ordered(from, to) != 0) {
        return -1;
    }

    /* Record which resource ID was locked first (test observation only). */
    if (from == to || from->id == to->id) {
        atomic_store(&exp->first_acquired_id, from->id);
    } else {
        DeadlockPreventionResource *lower =
            (from->id < to->id) ? from : to;
        atomic_store(&exp->first_acquired_id, lower->id);
    }

    if (from->balance < amount) {
        unlock_resources_ordered(from, to);
        return -1; /* insufficient funds */
    }

    from->balance -= amount;
    to->balance += amount;

    unlock_resources_ordered(from, to);
    return 0;
}

static void *worker_func(void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    DeadlockPreventionExperiment *exp = wa->exp;

    /*
     * Deterministic coordination: both workers wait at the barrier before
     * starting. This creates meaningful contention on both resources without
     * relying on scheduler timing or arbitrary sleeps.
     */
    int rc = pthread_barrier_wait(&exp->barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        return NULL;
    }

    DeadlockPreventionResource *from =
        wa->worker_index == 0 ? &exp->resource_a : &exp->resource_b;
    DeadlockPreventionResource *to =
        wa->worker_index == 0 ? &exp->resource_b : &exp->resource_a;

    for (int i = 0; i < DEADLOCK_PREVENTION_NUM_TRANSFERS; i++) {
        wa->stats->operations_attempted++;
        if (transfer_ordered(exp, from, to, DEADLOCK_PREVENTION_TRANSFER_AMOUNT) == 0) {
            wa->stats->operations_completed++;
        } else {
            wa->stats->operations_failed++;
        }
    }

    return NULL;
}

DeadlockPreventionStatus deadlock_prevention_init(
    DeadlockPreventionExperiment *exp,
    int timeout_ms)
{
    if (exp == NULL) {
        return DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT;
    }
    if (exp->initialized) {
        return DEADLOCK_PREVENTION_ERR_ALREADY_INITIALIZED;
    }
    if (timeout_ms < DEADLOCK_PREVENTION_MIN_TIMEOUT_MS ||
        timeout_ms > DEADLOCK_PREVENTION_MAX_TIMEOUT_MS) {
        return DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT;
    }

    memset(exp, 0, sizeof(*exp));

    exp->resource_a.id = RESOURCE_A_ID;
    exp->resource_a.balance = INITIAL_BALANCE;
    exp->resource_b.id = RESOURCE_B_ID;
    exp->resource_b.balance = INITIAL_BALANCE;

    int rc = pthread_mutex_init(&exp->resource_a.mutex, NULL);
    if (rc != 0) {
        return DEADLOCK_PREVENTION_ERR_MUTEX_INIT_FAILED;
    }
    exp->resource_a.mutex_initialized = 1;

    rc = pthread_mutex_init(&exp->resource_b.mutex, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&exp->resource_a.mutex);
        return DEADLOCK_PREVENTION_ERR_MUTEX_INIT_FAILED;
    }
    exp->resource_b.mutex_initialized = 1;

    rc = pthread_barrier_init(&exp->barrier, NULL, 2);
    if (rc != 0) {
        pthread_mutex_destroy(&exp->resource_a.mutex);
        pthread_mutex_destroy(&exp->resource_b.mutex);
        return DEADLOCK_PREVENTION_ERR_BARRIER_INIT_FAILED;
    }
    exp->barrier_initialized = 1;

    exp->timeout_ms = timeout_ms;
    exp->initialized = 1;
    return DEADLOCK_PREVENTION_SUCCESS;
}

DeadlockPreventionStatus deadlock_prevention_run(
    DeadlockPreventionExperiment *exp,
    DeadlockPreventionResult *result)
{
    if (exp == NULL || !exp->initialized) {
        return DEADLOCK_PREVENTION_ERR_NOT_INITIALIZED;
    }
    if (result == NULL) {
        return DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));
    result->mode = DEADLOCK_PREVENTION_MODE_SAFE_ORDERED;
    result->resource_a_id = exp->resource_a.id;
    result->resource_b_id = exp->resource_b.id;

    WorkerStats stats1 = {0, 0, 0};
    WorkerStats stats2 = {0, 0, 0};

    WorkerArg arg1 = {exp, 0, &stats1};
    WorkerArg arg2 = {exp, 1, &stats2};

    double start = now_seconds();

    int rc1 = pthread_create(&exp->worker1, NULL, worker_func, &arg1);
    if (rc1 != 0) {
        return DEADLOCK_PREVENTION_ERR_THREAD_CREATE_FAILED;
    }
    exp->workers_created++;

    int rc2 = pthread_create(&exp->worker2, NULL, worker_func, &arg2);
    if (rc2 != 0) {
        pthread_join(exp->worker1, NULL);
        exp->workers_created = 0;
        return DEADLOCK_PREVENTION_ERR_THREAD_CREATE_FAILED;
    }
    exp->workers_created++;

    pthread_join(exp->worker1, NULL);
    pthread_join(exp->worker2, NULL);

    double end = now_seconds();
    result->duration_seconds = end - start;

    result->operations_attempted =
        stats1.operations_attempted + stats2.operations_attempted;
    result->operations_completed =
        stats1.operations_completed + stats2.operations_completed;
    result->operations_failed =
        stats1.operations_failed + stats2.operations_failed;

    result->both_operations_completed =
        (stats1.operations_completed > 0 && stats2.operations_completed > 0);

    result->lower_id_acquired_first =
        (atomic_load(&exp->first_acquired_id) == RESOURCE_A_ID);

    long long total = exp->resource_a.balance + exp->resource_b.balance;
    result->total_balance_preserved = (total == TOTAL_BALANCE);
    result->final_state_consistent = result->total_balance_preserved;

    return DEADLOCK_PREVENTION_SUCCESS;
}

void deadlock_prevention_destroy(DeadlockPreventionExperiment *exp)
{
    if (exp == NULL) {
        return;
    }
    if (!exp->initialized) {
        return;
    }

    if (exp->barrier_initialized) {
        pthread_barrier_destroy(&exp->barrier);
    }
    if (exp->resource_b.mutex_initialized) {
        pthread_mutex_destroy(&exp->resource_b.mutex);
    }
    if (exp->resource_a.mutex_initialized) {
        pthread_mutex_destroy(&exp->resource_a.mutex);
    }

    exp->initialized = 0;
}

DeadlockPreventionStatus deadlock_prevention_transfer_safe(
    DeadlockPreventionExperiment *exp,
    DeadlockPreventionResource *from,
    DeadlockPreventionResource *to,
    long long amount)
{
    if (exp == NULL || !exp->initialized || from == NULL || to == NULL) {
        return DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT;
    }
    if (!from->mutex_initialized || !to->mutex_initialized) {
        return DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT;
    }

    if (transfer_ordered(exp, from, to, amount) != 0) {
        return DEADLOCK_PREVENTION_ERR_LOCK_FAILED;
    }
    return DEADLOCK_PREVENTION_SUCCESS;
}
