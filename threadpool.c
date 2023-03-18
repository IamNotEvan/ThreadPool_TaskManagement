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
    struct list waiting_q;
    struct worker_thread *threads;
    //int global_q_size;
    bool is_shutdown;
    int num_threads;
    int count;
    pthread_cond_t global_queue_not_empty;
    // pthread_cond_t global_queue_not_full;
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
    pthread_mutex_t task_lock;
    pthread_cond_t task_ready;
    enum future_status status;
};

struct worker_thread
{
    pthread_mutex_t local_lock;
    struct list_elem elem;
    struct list local_q;
    int local_q_size;
    int id;

    struct thread_pool *pool;
    bool is_stopped;
};

// Important Notes
// A flag should denote when the thread pool is shutting down.
//  A static function should be created to perform the core work
//  of each worker thread. The thread pool submit() function should
//  allocate a new future and submit it to the pool. The get()
//  function may require the calling thread to help complete the
//  future being joined. The shutdown and destroy() function should
// shut down the thread pool, join all worker threads, and deallocate
// memory allocated on behalf of the worker threads. The free()
//  function should be used by the client to free memory for a
// future instance allocated in thread pool submit(). Atomic
//       updates should be used to ensure the state of a future is
//        updated correctly and to avoid conflicts between worker
//         threads when stealing or helping with a future.

static pthread_barrier_t readysetgo;
/*
struct thread_pool *thread_pool_new(int nthreads);
void thread_pool_shutdown_and_destroy(struct thread_pool *pool);
typedef void *(*fork_join_task_t)(struct thread_pool *pool, void *data);
struct future *thread_pool_submit(struct thread_pool *pool, fork_join_task_t task, void *data);
void *future_get(struct future *);
void future_free(struct future *);
*/
static void *worker_function(void *_arg);

/* Create a new thread pool with no more than n threads. */
struct thread_pool *thread_pool_new(int nthreads)
{
    struct thread_pool *pool = malloc(sizeof(*pool));
    pool->num_threads = nthreads;
    pool->threads = malloc(nthreads * sizeof(struct worker_thread));
    pool->thread_array = malloc(nthreads * sizeof *pool->thread_array);
    pool->is_shutdown = false;
    pool->count = 0;
    //pool->global_q_size = 0;
    list_init(&pool->global_q);
    list_init(&pool->waiting_q);

    pthread_mutex_init(&pool->global_lock, NULL);
    pthread_cond_init(&pool->global_queue_not_empty, NULL);
    pthread_cond_init(&pool->global_queue_not_empty, NULL);

    pthread_barrier_init(&readysetgo, NULL, nthreads);

    for (int i = 0; i < nthreads; i++)
    {
        printf("created thread \n");
        pthread_create(&pool->thread_array[i], NULL, worker_function, pool);
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
    printf("here\n");

    pthread_barrier_destroy(&readysetgo);
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
    pthread_mutex_init(&future_task->task_lock, NULL);

    pthread_mutex_lock(&pool->global_lock);
    list_push_back(&pool->global_q, &future_task->elem);
    //pool->global_q_size++;
    pthread_cond_signal(&pool->global_queue_not_empty);
    pthread_mutex_unlock(&pool->global_lock);
    return future_task;
}

/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * Returns the value returned by this task.
 */
void *future_get(struct future *task)
{
    if (task->status == PROGRESS)
    {
        pthread_cond_wait(&task->task_ready, &task->task_lock);
    }
    else if (task->status == START)
    {
        pthread_mutex_lock(&task->pool->global_lock);
        if (!list_empty(&task->pool->global_q))
        {
            
            list_pop_front(&task->pool->global_q);
            //task->pool->global_q_size--;
           
        }
        pthread_mutex_unlock(&task->pool->global_lock);
        task->status = PROGRESS;

        void *res = task->func(task->pool, task->data);

        //pthread_mutex_lock(&task->pool->global_lock);
        task->result = res;
        task->status = COMPLETED;
        pthread_cond_signal(&task->task_ready);

        //pthread_mutex_unlock(&task->pool->global_lock);
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
    // Initialize worker thread values
    struct worker_thread *worker = malloc(sizeof(*worker));
    worker->pool = _arg;
    worker->id = worker->pool->count;
    worker->is_stopped = false;
    worker->local_q_size = 0;
    pthread_mutex_init(&worker->local_lock, NULL);

    list_init(&worker->local_q);

    pthread_mutex_lock(&worker->pool->global_lock);
    //(worker->pool->threads[worker->pool->count]) = *worker;

    while (!worker->pool->is_shutdown)
    {
        if (list_empty(&worker->pool->global_q))
        {
            pthread_cond_wait(&worker->pool->global_queue_not_empty, &worker->pool->global_lock);
        }
        else
        {
            struct future *f = list_entry(list_pop_front(&worker->pool->global_q), struct future, elem);
            //worker->pool->global_q_size--;

            f->status = PROGRESS;
            pthread_mutex_unlock(&worker->pool->global_lock);
            void *res = f->func(f->pool, f->data);
            pthread_mutex_lock(&worker->pool->global_lock);

            f->result = res;
            f->status = COMPLETED;
            pthread_cond_signal(&f->task_ready);
        }
    }

    pthread_mutex_unlock(&worker->pool->global_lock);

    return NULL;
}
