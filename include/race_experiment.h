#ifndef RACE_EXPERIMENT_H
#define RACE_EXPERIMENT_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define RACE_EXPERIMENT_MAX_WORKERS 64
#define RACE_EXPERIMENT_MAX_INCREMENTS 1000000

typedef enum RaceMode {
    RACE_MODE_UNSAFE = 0,
    RACE_MODE_SAFE = 1
} RaceMode;

typedef enum RaceExperimentStatus {
    RACE_EXPERIMENT_SUCCESS = 0,
    RACE_EXPERIMENT_ERR_INVALID_ARGUMENT = -1,
    RACE_EXPERIMENT_ERR_NOT_INITIALIZED = -2,
    RACE_EXPERIMENT_ERR_ALREADY_INITIALIZED = -3,
    RACE_EXPERIMENT_ERR_ALLOC_FAILED = -4,
    RACE_EXPERIMENT_ERR_MUTEX_INIT_FAILED = -5,
    RACE_EXPERIMENT_ERR_BARRIER_INIT_FAILED = -6,
    RACE_EXPERIMENT_ERR_THREAD_CREATE_FAILED = -7
} RaceExperimentStatus;

typedef struct RaceExperimentResult {
    RaceMode mode;
    int64_t initial_value;
    int64_t expected_final_value;
    int64_t actual_final_value;
    int64_t num_workers;
    int64_t increments_per_worker;
    int64_t num_operations;
    int64_t lost_updates;
    int correct;
    double duration_seconds;
} RaceExperimentResult;

typedef struct RaceExperiment {
    RaceMode mode;
    _Atomic int64_t shared_value;
    int64_t initial_value;
    int64_t num_workers;
    int64_t increments_per_worker;

    pthread_mutex_t mutex;
    int mutex_initialized;

    pthread_barrier_t barrier_load;
    pthread_barrier_t barrier_store;
    int barrier_initialized;

    pthread_t *workers;
    int workers_created;

    int initialized;

    RaceExperimentResult result;
} RaceExperiment;

RaceExperimentStatus race_experiment_init(
    RaceExperiment *exp,
    RaceMode mode,
    int64_t initial_value,
    int64_t num_workers,
    int64_t increments_per_worker);

RaceExperimentStatus race_experiment_run(
    RaceExperiment *exp,
    RaceExperimentResult *result);

void race_experiment_destroy(RaceExperiment *exp);

#endif
