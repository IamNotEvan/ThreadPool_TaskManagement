/**
 * threadpool.c
 *
 * A work-stealing, fork-join thread pool.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include "list.h"
#include "threadpool.h"
#include <strings.h>
#include <stdio.h>
#include <threads.h>

// #include <threadpool.h>
/*
 * Opaque forward declarations. The actual definitions of these
 * types will be local to your threadpool.c implementation.
 */

enum future_status
{
    START,
    PROGRESS,
    COMPLETED,
};

// The thread pool should keep track of a global submission queue,
// worker threads, and each worker thread's own queue in the work stealing approach.
struct thread_pool
{
    struct list global_q;
    struct worker_thread *threads;
    bool is_shutdown;
    int num_threads;

    pthread_cond_t global_queue_not_empty;
    pthread_mutex_t global_lock;
    pthread_t *thread_array;
    pthread_barrier_t readysetgo;
};

// A future should store a pointer to the function to be called,
// any data to be passed to that function, and the result.
struct future
{
    fork_join_task_t func;
    struct list_elem elem;
    struct thread_pool *pool;
    void *data;
    void *result;
    bool is_ready;
    pthread_cond_t task_ready;
    enum future_status status;
};

struct worker_thread
{
    pthread_mutex_t local_lock;
    struct list local_q;
    int local_q_size;
    int id;
    struct thread_pool *pool;
};

static void *worker_function(void *_arg);
static _Thread_local struct worker_thread *locality = NULL;

/* Create a new thread pool with no more than n threads. */
struct thread_pool *thread_pool_new(int nthreads)
{
    struct thread_pool *pool = malloc(sizeof(*pool));
    pool->num_threads = nthreads;
    pool->thread_array = malloc(nthreads * sizeof *pool->thread_array);
    pool->is_shutdown = false;
    pthread_barrier_init(&pool->readysetgo, NULL, nthreads);
    list_init(&pool->global_q);
    pool->threads = malloc(nthreads * sizeof *pool->threads);

    pthread_mutex_init(&pool->global_lock, NULL);
    pthread_cond_init(&pool->global_queue_not_empty, NULL);

    for (int i = 0; i < nthreads; i++)
    {
        pool->threads[i].pool = pool;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(i, &mask);
        pthread_create(&pool->thread_array[i], NULL, worker_function, &pool->threads[i]);
        pthread_setaffinity_np(pool->thread_array[i], sizeof(mask), &mask);
    }
    return pool;
}

/*
 * Shutdown this thread pool in an orderly fashion.
 * Tasks that have been submitted but not executed may or
 * may not be executed.
 *
 * Deallocate the thread pool object before returning.
 */
void thread_pool_shutdown_and_destroy(struct thread_pool *pool)
{

    // Wake up all threads to handle the shutdown
    pthread_mutex_lock(&pool->global_lock);
    pool->is_shutdown = true;

    pthread_cond_broadcast(&pool->global_queue_not_empty);
    pthread_mutex_unlock(&pool->global_lock);
    // Join all threads and free resources
    for (int i = 0; i < pool->num_threads; i++)
    {
        if (pthread_join(pool->thread_array[i], NULL) != 0)
        {
            perror("Failed to join");
        }
       
    }

    pthread_cond_destroy(&pool->global_queue_not_empty);
    pthread_mutex_destroy(&pool->global_lock);
    pthread_barrier_destroy(&pool->readysetgo);
    free(pool->thread_array);
    free(pool->threads);
    free(pool);
}

/*
 * Submit a fork join task to the thread pool and return a
 * future.  The returned future can be used in future_get()
 * to obtain the result.
 * 'pool' - the pool to which to submit
 * 'task' - the task to be submitted.
 * 'data' - data to be passed to the task's function
 *
 * Returns a future representing this computation.
 */
struct future *thread_pool_submit(
    struct thread_pool *pool,
    fork_join_task_t task,
    void *data)
{
    struct future *future_task = malloc(sizeof(*future_task));
    future_task->status = START;
    future_task->func = task;
    future_task->data = data;
    future_task->pool = pool;
    pthread_cond_init(&future_task->task_ready, NULL);

    if (locality == NULL)
    {
        pthread_mutex_lock(&pool->global_lock);
        list_push_back(&pool->global_q, &future_task->elem);
        pthread_cond_signal(&pool->global_queue_not_empty);
        pthread_mutex_unlock(&pool->global_lock);
    }
    else
    {
        pthread_mutex_lock(&locality->local_lock);
        list_push_back(&locality->local_q, &future_task->elem);
        pthread_cond_signal(&pool->global_queue_not_empty);
        pthread_mutex_unlock(&locality->local_lock);
    }

    return future_task;
}

/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * Returns the value returned by this task.
 */
void *future_get(struct future *task)
{
    // pthread_mutex_lock(&locality->local_lock);
    if (locality == NULL)
    {
        pthread_mutex_lock(&task->pool->global_lock);
        if (task->status == PROGRESS)
        {
            while (task->status != COMPLETED)
            {
                pthread_cond_wait(&task->task_ready, &task->pool->global_lock);
            }
        }
        else if (task->status == START)
        {

            list_remove(&task->elem);

            task->status = PROGRESS;

            pthread_mutex_unlock(&task->pool->global_lock);
            void *res = task->func(task->pool, task->data);
            pthread_mutex_lock(&task->pool->global_lock);

            task->result = res;
            task->status = COMPLETED;
            pthread_cond_signal(&task->task_ready);
        }
        pthread_mutex_unlock(&task->pool->global_lock);
        return task->result;
    }
    else
    {
        pthread_mutex_lock(&locality->local_lock);
        if (task->status == PROGRESS)
        {
            while (task->status != COMPLETED)
            {
                pthread_cond_wait(&task->task_ready, &locality->local_lock);
            }
        }
        else if (task->status == START)
        {

            list_remove(&task->elem);

            task->status = PROGRESS;

            pthread_mutex_unlock(&locality->local_lock);
            void *res = task->func(task->pool, task->data);
            pthread_mutex_lock(&locality->local_lock);

            task->result = res;
            task->status = COMPLETED;
            pthread_cond_signal(&task->task_ready);
        }
        pthread_mutex_unlock(&locality->local_lock);
        return task->result;    
    }
    return task->result;
}

/* Deallocate this future.  Must be called after future_get() */
void future_free(struct future *task)
{
    free(task);
}

static void *worker_function(void *_arg)
{
    struct worker_thread *worker = _arg;
    list_init(&worker->local_q);
    pthread_mutex_init(&worker->local_lock, NULL);
    locality = worker;

    pthread_barrier_wait(&worker->pool->readysetgo);
    while (!worker->pool->is_shutdown)
    {    
        pthread_mutex_lock(&worker->local_lock);
        if (!list_empty(&worker->local_q))
        {
            struct future *f = list_entry(list_pop_back(&worker->local_q), struct future, elem);

            f->status = PROGRESS;

            pthread_mutex_unlock(&worker->local_lock);
            void *res = f->func(f->pool, f->data);
            pthread_mutex_lock(&worker->local_lock);

            f->result = res;
            f->status = COMPLETED;
            pthread_cond_signal(&f->task_ready);
            pthread_mutex_unlock(&worker->local_lock);
        }
        else if (pthread_mutex_unlock(&worker->local_lock) == 0 && pthread_mutex_lock(&worker->pool->global_lock) == 0 && !list_empty(&worker->pool->global_q))
        {
            struct future *f = list_entry(list_pop_front(&worker->pool->global_q), struct future, elem);

            f->status = PROGRESS;

            pthread_mutex_unlock(&worker->pool->global_lock);
            void *res = f->func(f->pool, f->data);
            pthread_mutex_lock(&worker->pool->global_lock);

            f->result = res;
            f->status = COMPLETED;

            pthread_cond_signal(&f->task_ready);
            pthread_mutex_unlock(&worker->pool->global_lock);
        }
        else
        {
            pthread_mutex_unlock(&worker->pool->global_lock);
            for (int idx = 0; idx < worker->pool->num_threads; idx++)
            {
                if (&worker->pool->threads[idx] != worker)
                {
                    pthread_mutex_lock(&worker->pool->threads[idx].local_lock);
                    if (!list_empty(&worker->pool->threads[idx].local_q))
                    {
                        struct future *f = list_entry(list_pop_front(&worker->pool->threads[idx].local_q), struct future, elem);
                        f->status = PROGRESS;

                        pthread_mutex_unlock(&worker->pool->threads[idx].local_lock);
                        void *res = f->func(f->pool, f->data);
                        pthread_mutex_lock(&worker->pool->threads[idx].local_lock);
    
                        f->result = res;
                        f->status = COMPLETED;

                        pthread_cond_signal(&f->task_ready);
                        pthread_mutex_unlock(&worker->pool->threads[idx].local_lock);
                        continue;
                    }
                    pthread_mutex_unlock(&worker->pool->threads[idx].local_lock);
                }
            }
            pthread_mutex_lock(&worker->pool->global_lock);
            if (worker->pool->is_shutdown && list_empty(&worker->pool->global_q))
            {
                pthread_mutex_unlock(&worker->pool->global_lock);
                break;
            }
            else
            {
                pthread_cond_wait(&worker->pool->global_queue_not_empty, &worker->pool->global_lock);
                pthread_mutex_unlock(&worker->pool->global_lock);
            }
        }
    }
    return NULL;
}   