#include "deadlock_experiment.h"

#include <stdio.h>
#include <pthread.h>
#include <string.h>

#define DEADLOCK_HELPER_RESOURCE_COUNT 2
#define DEADLOCK_HELPER_THREAD_COUNT 2

typedef struct {
    pthread_mutex_t *first_resource;
    pthread_mutex_t *second_resource;
    pthread_barrier_t *gate;
    const char *thread_name;
    int acquired_first;
} HelperWorkerArg;

static pthread_mutex_t g_resource_a;
static pthread_mutex_t g_resource_b;
static pthread_barrier_t g_gate;
static int g_resource_a_initialized = 0;
static int g_resource_b_initialized = 0;
static int g_gate_initialized = 0;

static void *helper_worker_func(void *arg)
{
    HelperWorkerArg *wa = (HelperWorkerArg *)arg;

    /* Acquire first resource (hold). */
    if (pthread_mutex_lock(wa->first_resource) != 0) {
        fprintf(stderr, "%s: failed to acquire first resource\n", wa->thread_name);
        return NULL;
    }
    wa->acquired_first = 1;
    fprintf(stdout, "%s: acquired first resource, waiting at gate\n", wa->thread_name);
    fflush(stdout);

    /* Deterministic coordination: both threads must hold their first
       resource before either attempts the second. This forces the
       circular-wait condition reliably, independent of scheduler timing. */
    pthread_barrier_wait(wa->gate);

    /* Attempt second resource (wait while holding first) -> deadlock. */
    fprintf(stdout, "%s: attempting second resource (will deadlock)\n", wa->thread_name);
    fflush(stdout);
    pthread_mutex_lock(wa->second_resource); /* blocks forever */

    /* Unreachable in deadlock. */
    fprintf(stdout, "%s: acquired second resource (unexpected)\n", wa->thread_name);
    fflush(stdout);
    return NULL;
}

int main(void)
{
    pthread_t threads[DEADLOCK_HELPER_THREAD_COUNT];
    HelperWorkerArg args[DEADLOCK_HELPER_THREAD_COUNT];
    int threads_created = 0;
    int rc;

    fprintf(stdout, "DEADLOCK_HELPER: starting deadlock demonstration\n");
    fflush(stdout);

    /* Initialize two independent mutex-protected resources. */
    rc = pthread_mutex_init(&g_resource_a, NULL);
    if (rc != 0) {
        fprintf(stderr, "DEADLOCK_HELPER: failed to init resource A mutex: %d\n", rc);
        return 1;
    }
    g_resource_a_initialized = 1;

    rc = pthread_mutex_init(&g_resource_b, NULL);
    if (rc != 0) {
        fprintf(stderr, "DEADLOCK_HELPER: failed to init resource B mutex: %d\n", rc);
        pthread_mutex_destroy(&g_resource_a);
        return 1;
    }
    g_resource_b_initialized = 1;

    /* Gate barrier: both threads must reach it before either proceeds. */
    rc = pthread_barrier_init(&g_gate, NULL, DEADLOCK_HELPER_THREAD_COUNT);
    if (rc != 0) {
        fprintf(stderr, "DEADLOCK_HELPER: failed to init gate barrier: %d\n", rc);
        pthread_mutex_destroy(&g_resource_a);
        pthread_mutex_destroy(&g_resource_b);
        return 1;
    }
    g_gate_initialized = 1;

    /* Thread 1: acquire A first, then B. */
    memset(&args[0], 0, sizeof(args[0]));
    args[0].first_resource = &g_resource_a;
    args[0].second_resource = &g_resource_b;
    args[0].gate = &g_gate;
    args[0].thread_name = "thread-1";
    if (pthread_create(&threads[0], NULL, helper_worker_func, &args[0]) != 0) {
        fprintf(stderr, "DEADLOCK_HELPER: failed to create thread-1\n");
        pthread_barrier_destroy(&g_gate);
        pthread_mutex_destroy(&g_resource_a);
        pthread_mutex_destroy(&g_resource_b);
        return 1;
    }
    threads_created++;

    /* Thread 2: acquire B first, then A. */
    memset(&args[1], 0, sizeof(args[1]));
    args[1].first_resource = &g_resource_b;
    args[1].second_resource = &g_resource_a;
    args[1].gate = &g_gate;
    args[1].thread_name = "thread-2";
    if (pthread_create(&threads[1], NULL, helper_worker_func, &args[1]) != 0) {
        fprintf(stderr, "DEADLOCK_HELPER: failed to create thread-2\n");
        pthread_barrier_destroy(&g_gate);
        pthread_mutex_destroy(&g_resource_a);
        pthread_mutex_destroy(&g_resource_b);
        return 1;
    }
    threads_created++;

    fprintf(stdout, "DEADLOCK_HELPER: both workers started, awaiting circular wait\n");
    fflush(stdout);

    /* Intentional deadlock: threads block here forever. The helper never
       joins threads or destroys mutexes. The parent process is responsible
       for terminating this process via the watchdog. */
    for (int i = 0; i < threads_created; i++) {
        pthread_join(threads[i], NULL); /* blocks until watchdog kills process */
    }

    /* Unreachable in normal deadlock operation. */
    pthread_barrier_destroy(&g_gate);
    pthread_mutex_destroy(&g_resource_a);
    pthread_mutex_destroy(&g_resource_b);
    return 0;
}
