#ifndef CBASE_H_INCLUDED
#define CBASE_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>

/*
    This mini library is a C99 language code base for my projects to be ever expanded upon
    Use at your own risk!
*/

/* SEG Platform Detection */

#if defined(_WIN32) || defined(_WIN64)
    #define CB_PLATFORM_WINDOWS
#elif defined(__APPLE__) && defined(__MACH__)
    #define CB_PLATFORM_MACOS
#elif defined(__linux__)
    #define CB_PLATFORM_LINUX
#else
    #error "Unsupported platform"
#endif

#if defined(CB_PLATFORM_MACOS) || defined(CB_PLATFORM_LINUX)
    #define CB_PLATFORM_POSIX
#endif

/* SEG Info / Error Handling */

typedef enum
{
    /* General errors */
    CB_INFO_OK = 0,
    CB_INFO_GENERIC_ERROR,

    /* Threading errors */
    CB_INFO_THREAD_CREATE_FAILED,
    CB_INFO_THREAD_JOIN_FAILED,
    CB_INFO_THREAD_DETACH_FAILED,

    /* Mutex errors */
    CB_INFO_MUTEX_CREATE_FAILED,
    CB_INFO_MUTEX_DESTROY_FAILED,
    CB_INFO_MUTEX_LOCK_FAILED,
    CB_INFO_MUTEX_UNLOCK_FAILED,
    CB_INFO_MUTEX_TRYLOCK_FAILED,
    CB_INFO_MUTEX_BUSY,

    /* Condition variable errors */
    CB_INFO_COND_CREATE_FAILED,
    CB_INFO_COND_DESTROY_FAILED,
    CB_INFO_COND_WAIT_FAILED,
    CB_INFO_COND_SIGNAL_FAILED,

    /* Queue errors */
    CB_INFO_QUEUE_FULL,
    CB_INFO_QUEUE_EMPTY,
    CB_INFO_QUEUE_DESTROY_FAILED,

    /* Memory errors */
    CB_INFO_ALLOC_FAILED,

} cb_info_t;

/* SEG Math */

/* SEG System Stuff */

/* SEG Threading */

#ifdef CB_PLATFORM_POSIX
    #include <pthread.h>
#else
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

/* --- Thread --- */

typedef struct
{
    cb_info_t info;
    void *result;
} cb_thread_result_t;

typedef cb_thread_result_t (*cb_thread_function_t)(void *arg);

typedef struct
{
    cb_info_t info;
#ifdef CB_PLATFORM_POSIX
    pthread_t handle;
#else
    HANDLE handle;
    void *cb__internal; /* trampoline arg, do not touch */
#endif
} cb_thread_t;

cb_thread_t         cb_thread_create(cb_thread_function_t fn, void *arg);
cb_thread_result_t  cb_thread_join(cb_thread_t thread);
cb_info_t           cb_thread_detach(cb_thread_t thread);

/* --- Mutex --- */

typedef struct
{
    cb_info_t info;
#ifdef CB_PLATFORM_POSIX
    pthread_mutex_t handle;
#else
    CRITICAL_SECTION handle;
#endif
} cb_mutex_t;

cb_mutex_t  cb_mutex_create(void);
cb_info_t   cb_mutex_destroy(cb_mutex_t *mutex);

cb_info_t   cb_mutex_lock(cb_mutex_t *mutex);
cb_info_t   cb_mutex_unlock(cb_mutex_t *mutex);
cb_info_t   cb_mutex_trylock(cb_mutex_t *mutex);

/* --- Condition Variable --- */

typedef struct
{
    cb_info_t info;
#ifdef CB_PLATFORM_POSIX
    pthread_cond_t handle;
#else
    CONDITION_VARIABLE handle;
#endif
} cb_cond_t;

cb_cond_t   cb_cond_create(void);
cb_info_t   cb_cond_destroy(cb_cond_t *cond);

cb_info_t   cb_cond_wait(cb_cond_t *cond, cb_mutex_t *mutex);
cb_info_t   cb_cond_signal(cb_cond_t *cond);
cb_info_t   cb_cond_broadcast(cb_cond_t *cond);

/* --- Thread-Safe Queue --- */

typedef struct
{
    cb_info_t info;
    void **items;
    uint32_t capacity;
    uint32_t count;
    uint32_t head;
    uint32_t tail;
    cb_mutex_t mutex;
    cb_cond_t not_full;
    cb_cond_t not_empty;
} cb_tsqueue_t;

typedef struct
{
    cb_info_t info;
    void *data;
} cb_tsqueue_item_t;

cb_tsqueue_t        cb_tsqueue_create(uint32_t capacity);
cb_info_t           cb_tsqueue_destroy(cb_tsqueue_t *queue);

cb_info_t           cb_tsqueue_push(cb_tsqueue_t *queue, void *item);
cb_tsqueue_item_t   cb_tsqueue_pop(cb_tsqueue_t *queue);
cb_tsqueue_item_t   cb_tsqueue_try_pop(cb_tsqueue_t *queue);
uint32_t            cb_tsqueue_count(cb_tsqueue_t *queue);

/* SEG IO */

#endif
