#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

/*
 * ThreadPool — a reusable POSIX thread pool with a bounded FIFO task queue.
 *
 * The pool is independent of the banking engine and makes no assumptions
 * about the nature of submitted tasks. The caller owns task argument memory
 * and must ensure it remains valid until the task finishes executing.
 */

typedef void (*ThreadTaskFunction)(void *arg);

typedef struct ThreadTask {
    ThreadTaskFunction function;
    void *arg;
} ThreadTask;

/*
 * ThreadPoolStatus — outcome of every public thread pool operation.
 */
typedef enum ThreadPoolStatus {
    THREAD_POOL_SUCCESS = 0,
    THREAD_POOL_ERR_INVALID_ARGUMENT = -1,
    THREAD_POOL_ERR_NOT_INITIALIZED = -2,
    THREAD_POOL_ERR_ALREADY_INITIALIZED = -3,
    THREAD_POOL_ERR_ALLOC_FAILED = -4,
    THREAD_POOL_ERR_MUTEX_INIT_FAILED = -5,
    THREAD_POOL_ERR_COND_INIT_FAILED = -6,
    THREAD_POOL_ERR_THREAD_CREATE_FAILED = -7,
    THREAD_POOL_ERR_SHUTDOWN = -8
} ThreadPoolStatus;

typedef struct ThreadPool {
    pthread_t *workers;
    size_t worker_count;
    size_t workers_created;

    ThreadTask *queue;
    size_t queue_capacity;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    size_t active_count;

    pthread_mutex_t queue_mutex;
    pthread_cond_t work_available_cond;
    pthread_cond_t queue_space_cond;
    pthread_cond_t all_done_cond;

    int mutex_initialized;
    int work_available_cond_initialized;
    int queue_space_cond_initialized;
    int all_done_cond_initialized;

    int shutdown;
    int initialized;
} ThreadPool;

/*
 * thread_pool_init — initialize a thread pool with the given number of
 * worker threads and bounded FIFO queue capacity.
 *
 * Returns THREAD_POOL_SUCCESS on success, or an error code on failure.
 * On failure, the pool is left in a clean, unusable state and all
 * successfully acquired resources are released.
 */
ThreadPoolStatus thread_pool_init(ThreadPool *pool,
                                  size_t worker_count,
                                  size_t queue_capacity);

/*
 * thread_pool_submit — submit a task to the pool's FIFO queue.
 *
 * Blocks when the queue is full until a worker consumes a task or shutdown
 * begins. Returns THREAD_POOL_ERR_SHUTDOWN if shutdown has started.
 *
 * The caller retains ownership of `arg` and must keep it valid until the
 * task function completes.
 */
ThreadPoolStatus thread_pool_submit(ThreadPool *pool,
                                    ThreadTaskFunction function,
                                    void *arg);

/*
 * thread_pool_wait_idle — block until all queued and in-progress tasks
 * have completed (queue_count == 0 && active_count == 0).
 *
 * Returns THREAD_POOL_SUCCESS on success, or an error code if the pool is
 * not initialized.
 */
ThreadPoolStatus thread_pool_wait_idle(ThreadPool *pool);

/*
 * thread_pool_shutdown — initiate graceful shutdown. Prevents new
 * submissions, wakes blocked submitters, and joins all worker threads after
 * they finish processing remaining queued tasks.
 *
 * Safe to call multiple times; subsequent calls return THREAD_POOL_SUCCESS
 * without side effects.
 */
ThreadPoolStatus thread_pool_shutdown(ThreadPool *pool);

/*
 * thread_pool_destroy — release all pool resources. If the pool has not
 * been shut down, shutdown is performed first. Safe to call on an
 * uninitialized or already-destroyed pool (no-op).
 */
void thread_pool_destroy(ThreadPool *pool);

#endif /* THREAD_POOL_H */
