#include "race_experiment.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    RaceExperiment *exp;
    int64_t worker_index;
} WorkerArg;

static void *worker_func(void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    RaceExperiment *exp = wa->exp;

    if (exp->mode == RACE_MODE_UNSAFE) {
        for (int64_t i = 0; i < exp->increments_per_worker; i++) {
            pthread_barrier_wait(&exp->barrier_load);

            int64_t current = atomic_load_explicit(
                &exp->shared_value, memory_order_relaxed);

            pthread_barrier_wait(&exp->barrier_store);

            int64_t updated = current + 1;
            atomic_store_explicit(
                &exp->shared_value, updated, memory_order_relaxed);
        }
    } else {
        for (int64_t i = 0; i < exp->increments_per_worker; i++) {
            pthread_mutex_lock(&exp->mutex);
            int64_t current = exp->shared_value;
            int64_t updated = current + 1;
            exp->shared_value = updated;
            pthread_mutex_unlock(&exp->mutex);
        }
    }

    return NULL;
}

RaceExperimentStatus race_experiment_init(
    RaceExperiment *exp,
    RaceMode mode,
    int64_t initial_value,
    int64_t num_workers,
    int64_t increments_per_worker)
{
    if (exp == NULL) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }
    if (exp->initialized) {
        return RACE_EXPERIMENT_ERR_ALREADY_INITIALIZED;
    }
    if (num_workers <= 0 || num_workers > RACE_EXPERIMENT_MAX_WORKERS) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }
    if (increments_per_worker <= 0 ||
        increments_per_worker > RACE_EXPERIMENT_MAX_INCREMENTS) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }

    if (increments_per_worker > INT64_MAX / num_workers) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }
    int64_t total_increments = num_workers * increments_per_worker;
    if (total_increments > 0 &&
        initial_value > INT64_MAX - total_increments) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }
    if (total_increments < 0 &&
        initial_value < INT64_MIN - total_increments) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }

    memset(exp, 0, sizeof(*exp));
    exp->mode = mode;
    exp->initial_value = initial_value;
    exp->num_workers = num_workers;
    exp->increments_per_worker = increments_per_worker;

    atomic_store_explicit(
        &exp->shared_value, initial_value, memory_order_relaxed);

    if (pthread_mutex_init(&exp->mutex, NULL) != 0) {
        return RACE_EXPERIMENT_ERR_MUTEX_INIT_FAILED;
    }
    exp->mutex_initialized = 1;

    if (pthread_barrier_init(
            &exp->barrier_load, NULL, (unsigned)num_workers) != 0) {
        pthread_mutex_destroy(&exp->mutex);
        exp->mutex_initialized = 0;
        return RACE_EXPERIMENT_ERR_BARRIER_INIT_FAILED;
    }
    if (pthread_barrier_init(
            &exp->barrier_store, NULL, (unsigned)num_workers) != 0) {
        pthread_barrier_destroy(&exp->barrier_load);
        pthread_mutex_destroy(&exp->mutex);
        exp->mutex_initialized = 0;
        return RACE_EXPERIMENT_ERR_BARRIER_INIT_FAILED;
    }
    exp->barrier_initialized = 1;

    exp->initialized = 1;
    return RACE_EXPERIMENT_SUCCESS;
}

RaceExperimentStatus race_experiment_run(
    RaceExperiment *exp, RaceExperimentResult *result)
{
    if (exp == NULL || !exp->initialized) {
        return RACE_EXPERIMENT_ERR_NOT_INITIALIZED;
    }
    if (result == NULL) {
        return RACE_EXPERIMENT_ERR_INVALID_ARGUMENT;
    }

    atomic_store_explicit(
        &exp->shared_value, exp->initial_value, memory_order_relaxed);

    exp->workers = calloc(
        (size_t)exp->num_workers, sizeof(pthread_t));
    if (exp->workers == NULL) {
        return RACE_EXPERIMENT_ERR_ALLOC_FAILED;
    }
    exp->workers_created = 0;

    WorkerArg *wa = calloc(
        (size_t)exp->num_workers, sizeof(WorkerArg));
    if (wa == NULL) {
        free(exp->workers);
        exp->workers = NULL;
        return RACE_EXPERIMENT_ERR_ALLOC_FAILED;
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int create_failed = 0;
    for (int64_t i = 0; i < exp->num_workers; i++) {
        wa[i].exp = exp;
        wa[i].worker_index = i;
        if (pthread_create(
                &exp->workers[i], NULL, worker_func, &wa[i]) != 0) {
            create_failed = 1;
            exp->workers_created = i;
            break;
        }
        exp->workers_created++;
    }

    if (create_failed) {
        for (int64_t i = 0; i < exp->workers_created; i++) {
            pthread_join(exp->workers[i], NULL);
        }
        free(exp->workers);
        free(wa);
        exp->workers = NULL;
        exp->workers_created = 0;
        return RACE_EXPERIMENT_ERR_THREAD_CREATE_FAILED;
    }

    for (int64_t i = 0; i < exp->num_workers; i++) {
        pthread_join(exp->workers[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    free(wa);

    int64_t actual = atomic_load_explicit(
        &exp->shared_value, memory_order_relaxed);
    int64_t expected =
        exp->initial_value + exp->num_workers * exp->increments_per_worker;
    int64_t lost = expected - actual;

    double duration = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                      (double)(ts_end.tv_nsec - ts_start.tv_nsec) /
                          1000000000.0;

    memset(result, 0, sizeof(*result));
    result->mode = exp->mode;
    result->initial_value = exp->initial_value;
    result->expected_final_value = expected;
    result->actual_final_value = actual;
    result->num_workers = exp->num_workers;
    result->increments_per_worker = exp->increments_per_worker;
    result->num_operations =
        exp->num_workers * exp->increments_per_worker;
    result->lost_updates = lost;
    result->correct = (actual == expected) ? 1 : 0;
    result->duration_seconds = duration;

    free(exp->workers);
    exp->workers = NULL;
    exp->workers_created = 0;

    return RACE_EXPERIMENT_SUCCESS;
}

void race_experiment_destroy(RaceExperiment *exp)
{
    if (exp == NULL) {
        return;
    }
    if (!exp->initialized) {
        return;
    }

    if (exp->workers != NULL) {
        free(exp->workers);
        exp->workers = NULL;
    }
    exp->workers_created = 0;

    if (exp->barrier_initialized) {
        pthread_barrier_destroy(&exp->barrier_load);
        pthread_barrier_destroy(&exp->barrier_store);
        exp->barrier_initialized = 0;
    }

    if (exp->mutex_initialized) {
        pthread_mutex_destroy(&exp->mutex);
        exp->mutex_initialized = 0;
    }

    exp->initialized = 0;
}
