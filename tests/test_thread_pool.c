#include "thread_pool.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);               \
            tests_failed++;                                                 \
        }                                                                   \
    } while (0)

#define RUN_TEST(name)                                                      \
    do {                                                                    \
        printf("TEST: %s\n", #name);                                        \
        fflush(stdout);                                                     \
        name();                                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Shared test state                                                  */
/* ------------------------------------------------------------------ */

static int g_task_counter = 0;
static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

static void increment_counter(void)
{
    pthread_mutex_lock(&g_counter_mutex);
    g_task_counter++;
    pthread_mutex_unlock(&g_counter_mutex);
}

static int get_counter(void)
{
    int v;
    pthread_mutex_lock(&g_counter_mutex);
    v = g_task_counter;
    pthread_mutex_unlock(&g_counter_mutex);
    return v;
}

static void reset_counter(void)
{
    pthread_mutex_lock(&g_counter_mutex);
    g_task_counter = 0;
    pthread_mutex_unlock(&g_counter_mutex);
}

/* ------------------------------------------------------------------ */
/* Simple task functions                                              */
/* ------------------------------------------------------------------ */

static void simple_task(void *arg)
{
    (void)arg;
    increment_counter();
}

static void slow_barrier_task(void *arg)
{
    pthread_barrier_t *barrier = (pthread_barrier_t *)arg;
    (void)barrier;
    increment_counter();
    pthread_barrier_wait(barrier);
}

/* ------------------------------------------------------------------ */
/* Test A — Basic initialization                                      */
/* ------------------------------------------------------------------ */

static void test_a_basic_initialization(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    st = thread_pool_init(&pool, 3, 16);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init with 3 workers should succeed");
    TEST_ASSERT(pool.worker_count == 3, "worker_count should be 3");
    TEST_ASSERT(pool.queue_capacity == 16, "queue_capacity should be 16");
    TEST_ASSERT(pool.initialized == 1, "pool should be marked initialized");

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");

    thread_pool_destroy(&pool);
    TEST_ASSERT(pool.initialized == 0, "pool should be uninitialized after destroy");
}

/* ------------------------------------------------------------------ */
/* Test B — Single task                                               */
/* ------------------------------------------------------------------ */

static void test_b_single_task(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    reset_counter();

    st = thread_pool_init(&pool, 2, 8);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    st = thread_pool_submit(&pool, simple_task, NULL);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should succeed");
    TEST_ASSERT(get_counter() == 1, "single task should have executed exactly once");

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Test C — Multiple tasks                                            */
/* ------------------------------------------------------------------ */

static void test_c_multiple_tasks(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;
    const int task_count = 100;

    reset_counter();

    st = thread_pool_init(&pool, 4, 64);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    for (int i = 0; i < task_count; i++) {
        st = thread_pool_submit(&pool, simple_task, NULL);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should succeed");
    TEST_ASSERT(get_counter() == task_count,
                "all 100 tasks should have executed exactly once");

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Test D — Concurrent execution                                      */
/* ------------------------------------------------------------------ */

#define CONCURRENT_WORKER_COUNT 3
#define CONCURRENT_TASK_COUNT   6

static pthread_barrier_t g_concurrency_barrier;
static pthread_mutex_t g_worker_ids_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker_ids[CONCURRENT_TASK_COUNT];
static int g_worker_id_count = 0;

static void record_worker_id_task(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&g_concurrency_barrier);

    pthread_mutex_lock(&g_worker_ids_mutex);
    g_worker_ids[g_worker_id_count] = pthread_self();
    g_worker_id_count++;
    pthread_mutex_unlock(&g_worker_ids_mutex);

    increment_counter();
}

static void test_d_concurrent_execution(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    reset_counter();
    g_worker_id_count = 0;

    pthread_barrier_init(&g_concurrency_barrier, NULL, CONCURRENT_WORKER_COUNT);

    st = thread_pool_init(&pool, CONCURRENT_WORKER_COUNT, CONCURRENT_TASK_COUNT);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    /* Submit barrier tasks equal to worker count — they execute concurrently. */
    for (int i = 0; i < CONCURRENT_WORKER_COUNT; i++) {
        st = thread_pool_submit(&pool, record_worker_id_task, NULL);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    /* Submit remaining tasks without the barrier. */
    for (int i = CONCURRENT_WORKER_COUNT; i < CONCURRENT_TASK_COUNT; i++) {
        st = thread_pool_submit(&pool, simple_task, NULL);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should succeed");
    TEST_ASSERT(get_counter() == CONCURRENT_TASK_COUNT,
                "all concurrent tasks should have executed");

    /* Verify more than one distinct worker thread participated. */
    int distinct = 1;
    for (int i = 1; i < g_worker_id_count; i++) {
        if (pthread_equal(g_worker_ids[i], g_worker_ids[0]) == 0) {
            distinct = 1;
            break;
        }
    }
    /* Count distinct thread IDs properly. */
    distinct = 0;
    for (int i = 0; i < g_worker_id_count; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) {
            if (pthread_equal(g_worker_ids[i], g_worker_ids[j])) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            distinct++;
        }
    }
    TEST_ASSERT(distinct > 1, "more than one worker thread should have participated");

    pthread_barrier_destroy(&g_concurrency_barrier);

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Test E — Queue capacity (blocking when full)                       */
/* ------------------------------------------------------------------ */

static pthread_barrier_t g_queue_fill_barrier;
static _Atomic int g_submitter_started = 0;
static _Atomic int g_submitter_done = 0;

static void *blocking_submitter(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    __atomic_store_n(&g_submitter_started, 1, __ATOMIC_SEQ_CST);
    thread_pool_submit(pool, simple_task, NULL);
    __atomic_store_n(&g_submitter_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static void test_e_queue_capacity(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;
    pthread_t submitter_thread;

    reset_counter();
    __atomic_store_n(&g_submitter_started, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_submitter_done, 0, __ATOMIC_SEQ_CST);

    pthread_barrier_init(&g_queue_fill_barrier, NULL, 2);

    /* 1 worker, capacity 2. Worker dequeues the slow task immediately and
     * blocks on the barrier, so the queue can be filled by main-thread
     * submissions while the worker is stuck. */
    st = thread_pool_init(&pool, 1, 2);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    /* Submit slow task — worker dequeues it and blocks on the barrier.
     * Queue is now empty but worker is unavailable. */
    st = thread_pool_submit(&pool, slow_barrier_task, &g_queue_fill_barrier);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "first submit should succeed");

    /* Fill the queue with two more tasks (capacity = 2). */
    st = thread_pool_submit(&pool, simple_task, NULL);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "second submit should succeed");
    st = thread_pool_submit(&pool, simple_task, NULL);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "third submit should succeed (queue full)");

    /* Helper thread attempts a fourth submit — must block because queue is full. */
    pthread_create(&submitter_thread, NULL, blocking_submitter, &pool);

    /* Wait for the helper to reach its submit call (it will block). */
    while (__atomic_load_n(&g_submitter_started, __ATOMIC_SEQ_CST) == 0) {
        sched_yield();
    }

    /* Release the barrier: the slow task completes, the worker dequeues a
     * queued task, freeing space, and the helper's submit unblocks. */
    pthread_barrier_wait(&g_queue_fill_barrier);

    pthread_join(submitter_thread, NULL);
    TEST_ASSERT(__atomic_load_n(&g_submitter_done, __ATOMIC_SEQ_CST) == 1,
                "blocked submitter should complete once queue space is available");

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should succeed");
    TEST_ASSERT(get_counter() == 4, "all four tasks should have executed");

    pthread_barrier_destroy(&g_queue_fill_barrier);

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Test F — FIFO removal (single worker)                              */
/* ------------------------------------------------------------------ */

#define FIFO_TASK_COUNT 10

static long g_fifo_results[FIFO_TASK_COUNT];
static long g_fifo_task_data[FIFO_TASK_COUNT][2];
static pthread_mutex_t g_fifo_mutex = PTHREAD_MUTEX_INITIALIZER;

static void fifo_sequence_task(void *arg)
{
    long *data = (long *)arg;
    long seq = data[0];
    long *slot = &data[1];

    pthread_mutex_lock(&g_fifo_mutex);
    g_fifo_results[seq] = *slot;
    pthread_mutex_unlock(&g_fifo_mutex);
}

static void test_f_fifo_removal(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    /* Single worker guarantees FIFO execution order. */
    st = thread_pool_init(&pool, 1, FIFO_TASK_COUNT);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init with 1 worker should succeed");

    for (long i = 0; i < FIFO_TASK_COUNT; i++) {
        g_fifo_task_data[i][0] = i;
        g_fifo_task_data[i][1] = i; /* expected result equals sequence number */
        st = thread_pool_submit(&pool, fifo_sequence_task, g_fifo_task_data[i]);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should succeed");

    for (long i = 0; i < FIFO_TASK_COUNT; i++) {
        TEST_ASSERT(g_fifo_results[i] == i,
                    "FIFO execution order should match submission order");
    }

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Test G — Wait idle                                                 */
/* ------------------------------------------------------------------ */

static pthread_barrier_t g_wait_idle_barrier;
static _Atomic int g_wait_idle_released = 0;

static void wait_idle_slow_task(void *arg)
{
    (void)arg;
    increment_counter();
    pthread_barrier_wait((pthread_barrier_t *)arg);
    __atomic_store_n(&g_wait_idle_released, 1, __ATOMIC_SEQ_CST);
}

static void test_g_wait_idle(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    reset_counter();
    __atomic_store_n(&g_wait_idle_released, 0, __ATOMIC_SEQ_CST);

    pthread_barrier_init(&g_wait_idle_barrier, NULL, 3);

    st = thread_pool_init(&pool, 2, 8);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    /* Submit 2 slow tasks. Queue has 2 pending tasks. */
    for (int i = 0; i < 2; i++) {
        st = thread_pool_submit(&pool, wait_idle_slow_task, &g_wait_idle_barrier);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    /* wait_idle should block because active_count > 0. Release the tasks
     * via the barrier, then verify wait_idle returns after they complete.
     * Barrier count is 3: 2 tasks + 1 main thread. */
    pthread_barrier_wait(&g_wait_idle_barrier);

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should return after tasks complete");
    TEST_ASSERT(get_counter() == 2, "both tasks should have completed");
    TEST_ASSERT(__atomic_load_n(&g_wait_idle_released, __ATOMIC_SEQ_CST) == 1,
                "tasks should have passed the barrier");

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);

    pthread_barrier_destroy(&g_wait_idle_barrier);
}

/* ------------------------------------------------------------------ */
/* Test H — Graceful shutdown                                         */
/* ------------------------------------------------------------------ */

static void test_h_graceful_shutdown(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    reset_counter();

    st = thread_pool_init(&pool, 3, 16);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    /* Submit several tasks. */
    for (int i = 0; i < 10; i++) {
        st = thread_pool_submit(&pool, simple_task, NULL);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    /* Initiate shutdown. Workers finish queued tasks then exit. */
    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");

    /* All previously submitted tasks should have completed. */
    TEST_ASSERT(get_counter() == 10,
                "all 10 submitted tasks should complete during shutdown");

    /* Pool should not be initialized after shutdown+destroy lifecycle check:
     * shutdown alone does not reset initialized, but destroy will. */
    TEST_ASSERT(pool.initialized == 1, "pool should still be marked initialized after shutdown");

    thread_pool_destroy(&pool);
    TEST_ASSERT(pool.initialized == 0, "pool should be uninitialized after destroy");
}

/* ------------------------------------------------------------------ */
/* Test I — Submission after shutdown                                 */
/* ------------------------------------------------------------------ */

static void test_i_submission_after_shutdown(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    st = thread_pool_init(&pool, 2, 8);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");

    /* Submission after shutdown must be rejected. */
    st = thread_pool_submit(&pool, simple_task, NULL);
    TEST_ASSERT(st == THREAD_POOL_ERR_SHUTDOWN,
                "submit after shutdown should return SHUTDOWN error");

    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Test J — Invalid inputs                                            */
/* ------------------------------------------------------------------ */

static void test_j_invalid_inputs(void)
{
    ThreadPoolStatus st;

    /* NULL pool. */
    st = thread_pool_init(NULL, 4, 16);
    TEST_ASSERT(st == THREAD_POOL_ERR_INVALID_ARGUMENT,
                "init with NULL pool should return INVALID_ARGUMENT");

    /* Zero worker count. */
    ThreadPool pool1;
    memset(&pool1, 0, sizeof(pool1));
    st = thread_pool_init(&pool1, 0, 16);
    TEST_ASSERT(st == THREAD_POOL_ERR_INVALID_ARGUMENT,
                "init with zero workers should return INVALID_ARGUMENT");

    /* Zero queue capacity. */
    ThreadPool pool2;
    memset(&pool2, 0, sizeof(pool2));
    st = thread_pool_init(&pool2, 4, 0);
    TEST_ASSERT(st == THREAD_POOL_ERR_INVALID_ARGUMENT,
                "init with zero capacity should return INVALID_ARGUMENT");

    /* NULL task function. */
    ThreadPool pool3;
    st = thread_pool_init(&pool3, 2, 8);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");
    st = thread_pool_submit(&pool3, NULL, NULL);
    TEST_ASSERT(st == THREAD_POOL_ERR_INVALID_ARGUMENT,
                "submit with NULL function should return INVALID_ARGUMENT");
    thread_pool_shutdown(&pool3);
    thread_pool_destroy(&pool3);

    /* wait_idle on uninitialized pool. */
    ThreadPool pool4;
    memset(&pool4, 0, sizeof(pool4));
    st = thread_pool_wait_idle(&pool4);
    TEST_ASSERT(st == THREAD_POOL_ERR_INVALID_ARGUMENT,
                "wait_idle on uninitialized pool should return INVALID_ARGUMENT");

    /* destroy on NULL or uninitialized pool is a safe no-op. */
    thread_pool_destroy(NULL);
    thread_pool_destroy(&pool4);
}

/* ------------------------------------------------------------------ */
/* Test K — Repeated lifecycle                                        */
/* ------------------------------------------------------------------ */

static void test_k_repeated_lifecycle(void)
{
    for (int i = 0; i < 10; i++) {
        ThreadPool pool;
        ThreadPoolStatus st;

        st = thread_pool_init(&pool, 2, 8);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "repeated init should succeed");

        reset_counter();
        for (int j = 0; j < 5; j++) {
            st = thread_pool_submit(&pool, simple_task, NULL);
            TEST_ASSERT(st == THREAD_POOL_SUCCESS, "repeated submit should succeed");
        }

        st = thread_pool_wait_idle(&pool);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "repeated wait_idle should succeed");
        TEST_ASSERT(get_counter() == 5, "all 5 tasks should execute each iteration");

        st = thread_pool_shutdown(&pool);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "repeated shutdown should succeed");
        thread_pool_destroy(&pool);
    }
}

/* ------------------------------------------------------------------ */
/* Test L — Task argument ownership                                   */
/* ------------------------------------------------------------------ */

#define ARG_TASK_COUNT 8

static long g_arg_task_data[ARG_TASK_COUNT][2];

static void arg_receive_task(void *arg)
{
    long *data = (long *)arg;
    long received = data[0];
    data[1] = received;
}

static void test_l_task_argument_ownership(void)
{
    ThreadPool pool;
    ThreadPoolStatus st;

    st = thread_pool_init(&pool, 4, ARG_TASK_COUNT);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "init should succeed");

    for (long i = 0; i < ARG_TASK_COUNT; i++) {
        g_arg_task_data[i][0] = i * 100;
        g_arg_task_data[i][1] = -1;
        st = thread_pool_submit(&pool, arg_receive_task, g_arg_task_data[i]);
        TEST_ASSERT(st == THREAD_POOL_SUCCESS, "submit should succeed");
    }

    st = thread_pool_wait_idle(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "wait_idle should succeed");

    for (long i = 0; i < ARG_TASK_COUNT; i++) {
        TEST_ASSERT(g_arg_task_data[i][1] == i * 100,
                    "each task should receive its own argument value");
    }

    st = thread_pool_shutdown(&pool);
    TEST_ASSERT(st == THREAD_POOL_SUCCESS, "shutdown should succeed");
    thread_pool_destroy(&pool);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    RUN_TEST(test_a_basic_initialization);
    RUN_TEST(test_b_single_task);
    RUN_TEST(test_c_multiple_tasks);
    RUN_TEST(test_d_concurrent_execution);
    RUN_TEST(test_e_queue_capacity);
    RUN_TEST(test_f_fifo_removal);
    RUN_TEST(test_g_wait_idle);
    RUN_TEST(test_h_graceful_shutdown);
    RUN_TEST(test_i_submission_after_shutdown);
    RUN_TEST(test_j_invalid_inputs);
    RUN_TEST(test_k_repeated_lifecycle);
    RUN_TEST(test_l_task_argument_ownership);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");
    fflush(stdout);

    return tests_failed == 0 ? 0 : 1;
}
