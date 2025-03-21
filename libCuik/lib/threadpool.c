#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>

#ifndef _WIN32
#include <semaphore.h>

#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#endif

#include <cuik.h>

// HACK(NeGate): i wanna call tb_free_thread_resources on thread exit...
extern void tb_free_thread_resources(void);

// 1 << QEXP is the size of the queue per pool
#define QEXP 7

typedef _Atomic uint32_t atomic_uint32_t;
typedef void work_routine(void*);

typedef struct {
    work_routine* fn;
    char arg[56];
} work_t;

// Inspired by:
//   https://github.com/skeeto/scratch/blob/master/misc/queue.c
typedef struct {
    Cuik_IThreadpool super;

    atomic_bool running;

    // this skeeto guy is kinda sick wit it
    atomic_uint32_t queue;
    atomic_uint32_t jobs_done;

    int thread_count;
    work_t* work;
    thrd_t* threads;

    #ifdef _WIN32
    HANDLE sem;
    #else
    sem_t sem;
    #endif
} threadpool_t;

static work_t* ask_for_work(threadpool_t* threadpool, uint32_t* save) {
    uint32_t r = *save = threadpool->queue;
    uint32_t mask = (1u << QEXP) - 1;
    uint32_t head = r & mask;
    uint32_t tail = (r >> 16) & mask;

    return head != tail ? &threadpool->work[tail] : NULL;
}

static bool do_work(threadpool_t* threadpool) {
    uint32_t save;
    work_t* job = NULL;
    work_t tmp;

    do {
        job = ask_for_work(threadpool, &save);
        if (job == NULL) {
            // take a nap if we ain't find shit
            return true;
        }

        // copy out before we commit
        tmp = *job;
    } while (!atomic_compare_exchange_strong(&threadpool->queue, &save, save + 0x10000));

    tmp.fn(tmp.arg);
    threadpool->jobs_done -= 1;
    return false;
}

static int thread_func(void* arg) {
    threadpool_t* threadpool = arg;

    #ifdef CUIK_USE_CUIK
    cuikperf_thread_start();
    #endif

    while (threadpool->running) {
        if (do_work(threadpool)) {
            #ifdef _WIN32
            WaitForSingleObjectEx(threadpool->sem, -1, false); // wait for jobs
            #else
            sem_wait(&threadpool->sem);
            #endif
        }
    }

    #ifdef CUIK_USE_CUIK
    cuikperf_thread_stop();
    // tb_free_thread_resources();
    // cuik_free_thread_resources();
    #endif

    return 0;
}

void threadpool_submit(threadpool_t* threadpool, work_routine fn, size_t arg_size, void* arg) {
    ptrdiff_t i = 0;
    for (;;) {
        // might wanna change the memory order on this atomic op
        uint32_t r = threadpool->queue;

        uint32_t mask = (1u << QEXP) - 1;
        uint32_t head = r & mask;
        uint32_t tail = (r >> 16) & mask;
        uint32_t next = (head + 1u) & mask;
        if (r & 0x8000) { // avoid overflow on commit
            threadpool->queue &= ~0x8000;
        }

        // it don't fit...
        if (next != tail) {
            i = head;
            break;
        }
    }

    assert(arg_size <= sizeof(threadpool->work[i].arg));
    threadpool->work[i].fn = fn;
    memcpy(threadpool->work[i].arg, arg, arg_size);

    threadpool->jobs_done += 1;
    threadpool->queue += 1;

    #ifdef _WIN32
    ReleaseSemaphore(threadpool->sem, 1, 0);
    #else
    sem_post(&threadpool->sem);
    #endif
}

void threadpool_work_one_job(threadpool_t* threadpool) {
    do_work(threadpool);
}

void threadpool_work_while_wait(threadpool_t* threadpool) {
    while (threadpool->jobs_done > 0) {
        if (do_work(threadpool)) {
            thrd_yield();
        }
    }
}

void threadpool_wait(threadpool_t* threadpool) {
    while (threadpool->jobs_done > 0) {
        thrd_yield();
    }
}

void threadpool_free(threadpool_t* threadpool) {
}

int threadpool_get_thread_count(threadpool_t* threadpool) {
    return threadpool->thread_count;
}

static void threadpool__submit(void* user_data, Cuik_TaskFn fn, size_t arg_size, void* arg) {
    threadpool_submit(user_data, fn, arg_size, arg);
}

static void threadpool__work_one_job(void* user_data) {
    threadpool_work_one_job(user_data);
}

Cuik_IThreadpool* cuik_threadpool_create(int worker_count) {
    if (worker_count == 0) {
        return NULL;
    }

    size_t workqueue_size = 1u << QEXP;
    threadpool_t* tp = cuik_calloc(1, sizeof(threadpool_t));
    tp->super.submit = threadpool__submit;
    tp->super.work_one_job = threadpool__work_one_job;
    tp->work = cuik_malloc(workqueue_size * sizeof(work_t));
    tp->threads = cuik_malloc(worker_count * sizeof(thrd_t));
    tp->thread_count = worker_count;
    tp->running = true;

    #if _WIN32
    tp->sem = CreateSemaphoreExA(0, worker_count, worker_count, 0, 0, SEMAPHORE_ALL_ACCESS);
    #else
    if (sem_init(&tp->sem, 0 /* shared between threads */, worker_count) != 0) {
        fprintf(stderr, "error: could not create semaphore!\n");
        return NULL;
    }
    #endif

    for (int i = 0; i < worker_count; i++) {
        if (thrd_create(&tp->threads[i], thread_func, tp) != thrd_success) {
            fprintf(stderr, "error: could not create worker threads!\n");
            return NULL;
        }
    }

    return &tp->super;
}

void cuik_threadpool_destroy(Cuik_IThreadpool* thread_pool) {
    if (thread_pool == NULL) {
        return;
    }

    threadpool_t* tp = (threadpool_t*) thread_pool;
    tp->running = false;

    #ifdef _WIN32
    ReleaseSemaphore(tp->sem, tp->thread_count, 0);
    WaitForMultipleObjects(tp->thread_count, tp->threads, TRUE, INFINITE);

    for (int i = 0; i < tp->thread_count; i++) {
        thrd_join(tp->threads[i], NULL);
    }
    CloseHandle(tp->sem);
    #else
    // wake everyone
    for (size_t i = 0; i < tp->thread_count; i++) {
        sem_post(&tp->sem);
    }

    for (int i = 0; i < tp->thread_count; i++) {
        thrd_join(tp->threads[i], NULL);
    }

    sem_destroy(&tp->sem);
    #endif

    cuik_free(tp->threads);
    cuik_free(tp->work);
    cuik_free(tp);
}
