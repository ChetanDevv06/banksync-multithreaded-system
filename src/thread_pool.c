#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>

/*
 * worker_routine — main loop for each worker thread.
 *
 * Dequeues tasks from the FIFO queue and executes them outside the queue
 * mutex. On shutdown with an empty queue, the worker exits cleanly.
 */
static void *worker_routine(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    for (;;) {
        pthread_mutex_lock(&pool->queue_mutex);

        /* Wait while queue is empty and shutdown has not been requested.
         * While loop handles spurious wakeups. */
        while (pool->queue_count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->work_available_cond,
                              &pool->queue_mutex);
        }

        /* If shutting down and no queued tasks remain, exit. */
        if (pool->shutdown && pool->queue_count == 0) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        /* Dequeue one task from the head of the FIFO queue. */
        ThreadTask task = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;
        pool->active_count++;

        /* Notify blocked submitters that queue space is available. */
        pthread_cond_signal(&pool->queue_space_cond);

        pthread_mutex_unlock(&pool->queue_mutex);

        /* Execute the task outside the queue mutex. */
        task.function(task.arg);

        /* Reacquire the mutex to update active-task count. */
        pthread_mutex_lock(&pool->queue_mutex);
        pool->active_count--;

        /* If nothing is queued and nothing is active, notify wait_idle. */
        if (pool->queue_count == 0 && pool->active_count == 0) {
            pthread_cond_broadcast(&pool->all_done_cond);
        }

        pthread_mutex_unlock(&pool->queue_mutex);
    }

    return NULL;
}

ThreadPoolStatus thread_pool_init(ThreadPool *pool,
                                  size_t worker_count,
                                  size_t queue_capacity)
{
    if (pool == NULL) {
        return THREAD_POOL_ERR_INVALID_ARGUMENT;
    }
    if (worker_count == 0 || queue_capacity == 0) {
        return THREAD_POOL_ERR_INVALID_ARGUMENT;
    }

    /* Zero-initialize the entire structure so cleanup knows what was set.
     * Callers must not pass an already-initialized pool without destroying
     * it first; re-initializing would leak resources. */
    memset(pool, 0, sizeof(*pool));

    /* Allocate the worker array. */
    pool->workers = (pthread_t *)calloc(worker_count, sizeof(pthread_t));
    if (pool->workers == NULL) {
        return THREAD_POOL_ERR_ALLOC_FAILED;
    }

    /* Allocate the task queue (circular buffer). */
    pool->queue = (ThreadTask *)calloc(queue_capacity, sizeof(ThreadTask));
    if (pool->queue == NULL) {
        free(pool->workers);
        pool->workers = NULL;
        return THREAD_POOL_ERR_ALLOC_FAILED;
    }

    pool->worker_count = worker_count;
    pool->queue_capacity = queue_capacity;

    /* Initialize synchronization primitives, tracking each for safe cleanup. */
    if (pthread_mutex_init(&pool->queue_mutex, NULL) != 0) {
        free(pool->workers);
        free(pool->queue);
        pool->workers = NULL;
        pool->queue = NULL;
        return THREAD_POOL_ERR_MUTEX_INIT_FAILED;
    }
    pool->mutex_initialized = 1;

    if (pthread_cond_init(&pool->work_available_cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->queue_mutex);
        pool->mutex_initialized = 0;
        free(pool->workers);
        free(pool->queue);
        pool->workers = NULL;
        pool->queue = NULL;
        return THREAD_POOL_ERR_COND_INIT_FAILED;
    }
    pool->work_available_cond_initialized = 1;

    if (pthread_cond_init(&pool->queue_space_cond, NULL) != 0) {
        pthread_cond_destroy(&pool->work_available_cond);
        pool->work_available_cond_initialized = 0;
        pthread_mutex_destroy(&pool->queue_mutex);
        pool->mutex_initialized = 0;
        free(pool->workers);
        free(pool->queue);
        pool->workers = NULL;
        pool->queue = NULL;
        return THREAD_POOL_ERR_COND_INIT_FAILED;
    }
    pool->queue_space_cond_initialized = 1;

    if (pthread_cond_init(&pool->all_done_cond, NULL) != 0) {
        pthread_cond_destroy(&pool->queue_space_cond);
        pool->queue_space_cond_initialized = 0;
        pthread_cond_destroy(&pool->work_available_cond);
        pool->work_available_cond_initialized = 0;
        pthread_mutex_destroy(&pool->queue_mutex);
        pool->mutex_initialized = 0;
        free(pool->workers);
        free(pool->queue);
        pool->workers = NULL;
        pool->queue = NULL;
        return THREAD_POOL_ERR_COND_INIT_FAILED;
    }
    pool->all_done_cond_initialized = 1;

    /* Create worker threads. Track how many succeed for partial failure. */
    for (size_t i = 0; i < worker_count; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_routine, pool) != 0) {
            /* Partial failure: mark shutdown, wake created workers, join them,
             * then clean up all initialized resources. */
            pool->shutdown = 1;
            pthread_mutex_lock(&pool->queue_mutex);
            pthread_cond_broadcast(&pool->work_available_cond);
            pthread_cond_broadcast(&pool->queue_space_cond);
            pthread_mutex_unlock(&pool->queue_mutex);

            for (size_t j = 0; j < i; j++) {
                pthread_join(pool->workers[j], NULL);
            }

            if (pool->all_done_cond_initialized) {
                pthread_cond_destroy(&pool->all_done_cond);
            }
            if (pool->queue_space_cond_initialized) {
                pthread_cond_destroy(&pool->queue_space_cond);
            }
            if (pool->work_available_cond_initialized) {
                pthread_cond_destroy(&pool->work_available_cond);
            }
            if (pool->mutex_initialized) {
                pthread_mutex_destroy(&pool->queue_mutex);
            }
            free(pool->workers);
            free(pool->queue);
            pool->workers = NULL;
            pool->queue = NULL;

            return THREAD_POOL_ERR_THREAD_CREATE_FAILED;
        }
        pool->workers_created++;
    }

    pool->initialized = 1;
    return THREAD_POOL_SUCCESS;
}

ThreadPoolStatus thread_pool_submit(ThreadPool *pool,
                                    ThreadTaskFunction function,
                                    void *arg)
{
    if (pool == NULL || !pool->initialized) {
        return THREAD_POOL_ERR_INVALID_ARGUMENT;
    }
    if (function == NULL) {
        return THREAD_POOL_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&pool->queue_mutex);

    /* Wait while the queue is full and shutdown has not been requested. */
    while (pool->queue_count == pool->queue_capacity && !pool->shutdown) {
        pthread_cond_wait(&pool->queue_space_cond, &pool->queue_mutex);
    }

    /* Reject submission if shutdown has started. */
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->queue_mutex);
        return THREAD_POOL_ERR_SHUTDOWN;
    }

    /* Insert the task at the queue tail (FIFO). */
    pool->queue[pool->queue_tail].function = function;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;

    /* Signal a waiting worker that work is available. */
    pthread_cond_signal(&pool->work_available_cond);

    pthread_mutex_unlock(&pool->queue_mutex);

    return THREAD_POOL_SUCCESS;
}

ThreadPoolStatus thread_pool_wait_idle(ThreadPool *pool)
{
    if (pool == NULL || !pool->initialized) {
        return THREAD_POOL_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&pool->queue_mutex);

    /* Block until no queued and no active tasks remain. */
    while (pool->queue_count > 0 || pool->active_count > 0) {
        pthread_cond_wait(&pool->all_done_cond, &pool->queue_mutex);
    }

    pthread_mutex_unlock(&pool->queue_mutex);

    return THREAD_POOL_SUCCESS;
}

ThreadPoolStatus thread_pool_shutdown(ThreadPool *pool)
{
    if (pool == NULL || !pool->initialized) {
        return THREAD_POOL_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&pool->queue_mutex);

    /* Idempotent: if already shutting down, nothing more to do. */
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->queue_mutex);
        return THREAD_POOL_SUCCESS;
    }

    pool->shutdown = 1;

    /* Wake blocked workers so they can observe shutdown and exit. */
    pthread_cond_broadcast(&pool->work_available_cond);

    /* Wake blocked submitters so they receive a shutdown error. */
    pthread_cond_broadcast(&pool->queue_space_cond);

    pthread_mutex_unlock(&pool->queue_mutex);

    /* Join all successfully created worker threads. */
    for (size_t i = 0; i < pool->workers_created; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    return THREAD_POOL_SUCCESS;
}

void thread_pool_destroy(ThreadPool *pool)
{
    if (pool == NULL || !pool->initialized) {
        return;
    }

    /* Initiate shutdown if not already done, then join workers. */
    if (!pool->shutdown) {
        pool->shutdown = 1;
        pthread_mutex_lock(&pool->queue_mutex);
        pthread_cond_broadcast(&pool->work_available_cond);
        pthread_cond_broadcast(&pool->queue_space_cond);
        pthread_mutex_unlock(&pool->queue_mutex);

        for (size_t i = 0; i < pool->workers_created; i++) {
            pthread_join(pool->workers[i], NULL);
        }
    }

    /* Destroy synchronization primitives that were successfully initialized. */
    if (pool->all_done_cond_initialized) {
        pthread_cond_destroy(&pool->all_done_cond);
    }
    if (pool->queue_space_cond_initialized) {
        pthread_cond_destroy(&pool->queue_space_cond);
    }
    if (pool->work_available_cond_initialized) {
        pthread_cond_destroy(&pool->work_available_cond);
    }
    if (pool->mutex_initialized) {
        pthread_mutex_destroy(&pool->queue_mutex);
    }

    /* Release allocated memory. */
    free(pool->workers);
    free(pool->queue);
    pool->workers = NULL;
    pool->queue = NULL;

    pool->initialized = 0;
}
