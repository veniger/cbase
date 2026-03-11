
#ifdef CB_PLATFORM_POSIX
    #include <errno.h>
    #include <pthread.h>
#endif

/* --- Internal thread wrapper (shared layout for both platforms) --- */

typedef struct
{
    cb_thread_function_t fn;
    void *arg;
    cb_thread_result_t result;
    cb_arena_t *arena;
} cb__thread_trampoline_arg_t;

/* ================================================================ */
/*  POSIX (Linux + macOS) implementation                            */
/* ================================================================ */

#ifdef CB_PLATFORM_POSIX

static void *cb__thread_trampoline(void *raw_arg)
{
    cb__thread_trampoline_arg_t *targ = (cb__thread_trampoline_arg_t *)raw_arg;
    targ->result = targ->fn(targ->arg);
    return NULL;
}

/* --- Thread --- */

cb_thread_t cb_thread_create(cb_arena_t *arena, cb_thread_function_t fn, void *arg)
{
    cb_thread_t t;
    t.info = CB_INFO_OK;
    t.cb__internal = NULL;

    cb__thread_trampoline_arg_t *targ = (cb__thread_trampoline_arg_t *)cb__alloc(
        arena, sizeof(cb__thread_trampoline_arg_t), sizeof(void *));
    if (!targ)
    {
        t.info = CB_INFO_ALLOC_FAILED;
        return t;
    }

    targ->fn = fn;
    targ->arg = arg;
    targ->result.info = CB_INFO_OK;
    targ->result.result = NULL;
    targ->arena = arena;

    int rc = pthread_create(&t.handle, NULL, cb__thread_trampoline, targ);
    if (rc != 0)
    {
        cb__free(arena, targ);
        t.info = CB_INFO_THREAD_CREATE_FAILED;
        return t;
    }

    t.cb__internal = targ;
    return t;
}

cb_thread_result_t cb_thread_join(cb_thread_t *thread)
{
    cb_thread_result_t result;
    result.info = CB_INFO_OK;
    result.result = NULL;

    int rc = pthread_join(thread->handle, NULL);
    if (rc != 0)
    {
        result.info = CB_INFO_THREAD_JOIN_FAILED;
        return result;
    }

    if (thread->cb__internal)
    {
        cb__thread_trampoline_arg_t *targ = (cb__thread_trampoline_arg_t *)thread->cb__internal;
        result = targ->result;
        cb__free(targ->arena, targ);
        thread->cb__internal = NULL;
    }

    return result;
}

cb_info_t cb_thread_detach(cb_thread_t *thread)
{
    int rc = pthread_detach(thread->handle);
    /* NOTE: targ is leaked on detach because the thread may still be running.
       If using an arena, it will be reclaimed on arena destroy/reset. */
    return (rc == 0) ? CB_INFO_OK : CB_INFO_THREAD_DETACH_FAILED;
}

#endif /* CB_PLATFORM_POSIX */

/* ================================================================ */
/*  Windows implementation                                          */
/* ================================================================ */

#ifdef CB_PLATFORM_WINDOWS

static DWORD WINAPI cb__thread_trampoline(LPVOID raw_arg)
{
    cb__thread_trampoline_arg_t *targ = (cb__thread_trampoline_arg_t *)raw_arg;
    targ->result = targ->fn(targ->arg);
    return 0;
}

/* --- Thread --- */

cb_thread_t cb_thread_create(cb_arena_t *arena, cb_thread_function_t fn, void *arg)
{
    cb_thread_t t;
    t.info = CB_INFO_OK;
    t.cb__internal = NULL;

    cb__thread_trampoline_arg_t *targ = (cb__thread_trampoline_arg_t *)cb__alloc(
        arena, sizeof(cb__thread_trampoline_arg_t), sizeof(void *));
    if (!targ)
    {
        t.info = CB_INFO_ALLOC_FAILED;
        t.handle = NULL;
        return t;
    }

    targ->fn = fn;
    targ->arg = arg;
    targ->result.info = CB_INFO_OK;
    targ->result.result = NULL;
    targ->arena = arena;

    t.handle = CreateThread(NULL, 0, cb__thread_trampoline, targ, 0, NULL);
    if (!t.handle)
    {
        cb__free(arena, targ);
        t.info = CB_INFO_THREAD_CREATE_FAILED;
        return t;
    }

    t.cb__internal = targ;
    return t;
}

cb_thread_result_t cb_thread_join(cb_thread_t *thread)
{
    cb_thread_result_t result;
    result.info = CB_INFO_OK;
    result.result = NULL;

    DWORD wait = WaitForSingleObject(thread->handle, INFINITE);
    if (wait != WAIT_OBJECT_0)
    {
        result.info = CB_INFO_THREAD_JOIN_FAILED;
        return result;
    }

    CloseHandle(thread->handle);

    if (thread->cb__internal)
    {
        cb__thread_trampoline_arg_t *targ = (cb__thread_trampoline_arg_t *)thread->cb__internal;
        result = targ->result;
        cb__free(targ->arena, targ);
        thread->cb__internal = NULL;
    }

    return result;
}

cb_info_t cb_thread_detach(cb_thread_t *thread)
{
    BOOL ok = CloseHandle(thread->handle);
    return ok ? CB_INFO_OK : CB_INFO_THREAD_DETACH_FAILED;
}

/* --- Mutex (CRITICAL_SECTION) --- */

cb_mutex_t cb_mutex_create(void)
{
    cb_mutex_t m;
    m.info = CB_INFO_OK;
    InitializeCriticalSection(&m.handle);
    return m;
}

cb_info_t cb_mutex_destroy(cb_mutex_t *mutex)
{
    DeleteCriticalSection(&mutex->handle);
    return CB_INFO_OK;
}

cb_info_t cb_mutex_lock(cb_mutex_t *mutex)
{
    EnterCriticalSection(&mutex->handle);
    return CB_INFO_OK;
}

cb_info_t cb_mutex_unlock(cb_mutex_t *mutex)
{
    LeaveCriticalSection(&mutex->handle);
    return CB_INFO_OK;
}

cb_info_t cb_mutex_trylock(cb_mutex_t *mutex)
{
    BOOL ok = TryEnterCriticalSection(&mutex->handle);
    return ok ? CB_INFO_OK : CB_INFO_MUTEX_BUSY;
}

/* --- Condition Variable (CONDITION_VARIABLE, Vista+) --- */

cb_cond_t cb_cond_create(void)
{
    cb_cond_t c;
    c.info = CB_INFO_OK;
    InitializeConditionVariable(&c.handle);
    return c;
}

cb_info_t cb_cond_destroy(cb_cond_t *cond)
{
    (void)cond;
    return CB_INFO_OK;
}

cb_info_t cb_cond_wait(cb_cond_t *cond, cb_mutex_t *mutex)
{
    BOOL ok = SleepConditionVariableCS(&cond->handle, &mutex->handle, INFINITE);
    return ok ? CB_INFO_OK : CB_INFO_COND_WAIT_FAILED;
}

cb_info_t cb_cond_signal(cb_cond_t *cond)
{
    WakeConditionVariable(&cond->handle);
    return CB_INFO_OK;
}

cb_info_t cb_cond_broadcast(cb_cond_t *cond)
{
    WakeAllConditionVariable(&cond->handle);
    return CB_INFO_OK;
}

#endif /* CB_PLATFORM_WINDOWS */

/* ================================================================ */
/*  Shared: Mutex / Cond (POSIX)                                    */
/* ================================================================ */

#ifdef CB_PLATFORM_POSIX

cb_mutex_t cb_mutex_create(void)
{
    cb_mutex_t m;
    m.info = CB_INFO_OK;

    int rc = pthread_mutex_init(&m.handle, NULL);
    if (rc != 0)
    {
        m.info = CB_INFO_MUTEX_CREATE_FAILED;
    }

    return m;
}

cb_info_t cb_mutex_destroy(cb_mutex_t *mutex)
{
    int rc = pthread_mutex_destroy(&mutex->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_MUTEX_DESTROY_FAILED;
}

cb_info_t cb_mutex_lock(cb_mutex_t *mutex)
{
    int rc = pthread_mutex_lock(&mutex->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_MUTEX_LOCK_FAILED;
}

cb_info_t cb_mutex_unlock(cb_mutex_t *mutex)
{
    int rc = pthread_mutex_unlock(&mutex->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_MUTEX_UNLOCK_FAILED;
}

cb_info_t cb_mutex_trylock(cb_mutex_t *mutex)
{
    int rc = pthread_mutex_trylock(&mutex->handle);
    if (rc == 0) return CB_INFO_OK;
    if (rc == EBUSY) return CB_INFO_MUTEX_BUSY;
    return CB_INFO_MUTEX_TRYLOCK_FAILED;
}

cb_cond_t cb_cond_create(void)
{
    cb_cond_t c;
    c.info = CB_INFO_OK;

    int rc = pthread_cond_init(&c.handle, NULL);
    if (rc != 0)
    {
        c.info = CB_INFO_COND_CREATE_FAILED;
    }

    return c;
}

cb_info_t cb_cond_destroy(cb_cond_t *cond)
{
    int rc = pthread_cond_destroy(&cond->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_COND_DESTROY_FAILED;
}

cb_info_t cb_cond_wait(cb_cond_t *cond, cb_mutex_t *mutex)
{
    int rc = pthread_cond_wait(&cond->handle, &mutex->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_COND_WAIT_FAILED;
}

cb_info_t cb_cond_signal(cb_cond_t *cond)
{
    int rc = pthread_cond_signal(&cond->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_COND_SIGNAL_FAILED;
}

cb_info_t cb_cond_broadcast(cb_cond_t *cond)
{
    int rc = pthread_cond_broadcast(&cond->handle);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_COND_SIGNAL_FAILED;
}

#endif /* CB_PLATFORM_POSIX */

/* ================================================================ */
/*  Platform-independent: Thread-Safe Queue                         */
/* ================================================================ */

cb_tsqueue_t cb_tsqueue_create(cb_arena_t *arena, uint32_t capacity)
{
    cb_tsqueue_t q;
    q.info = CB_INFO_OK;
    q.capacity = capacity;
    q.count = 0;
    q.head = 0;
    q.tail = 0;

    q.items = (void **)cb__alloc(arena, sizeof(void *) * capacity, sizeof(void *));
    if (!q.items)
    {
        q.info = CB_INFO_ALLOC_FAILED;
        return q;
    }

    q.mutex = cb_mutex_create();
    if (q.mutex.info != CB_INFO_OK)
    {
        cb__free(arena, q.items);
        q.items = NULL;
        q.info = CB_INFO_ALLOC_FAILED;
        return q;
    }

    q.not_full = cb_cond_create();
    if (q.not_full.info != CB_INFO_OK)
    {
        cb__free(arena, q.items);
        q.items = NULL;
        cb_mutex_destroy(&q.mutex);
        q.info = CB_INFO_ALLOC_FAILED;
        return q;
    }

    q.not_empty = cb_cond_create();
    if (q.not_empty.info != CB_INFO_OK)
    {
        cb__free(arena, q.items);
        q.items = NULL;
        cb_mutex_destroy(&q.mutex);
        cb_cond_destroy(&q.not_full);
        q.info = CB_INFO_ALLOC_FAILED;
        return q;
    }

    return q;
}

cb_info_t cb_tsqueue_destroy(cb_arena_t *arena, cb_tsqueue_t *queue)
{
    cb_info_t result = CB_INFO_OK;

    if (cb_mutex_destroy(&queue->mutex) != CB_INFO_OK) result = CB_INFO_QUEUE_DESTROY_FAILED;
    if (cb_cond_destroy(&queue->not_full) != CB_INFO_OK) result = CB_INFO_QUEUE_DESTROY_FAILED;
    if (cb_cond_destroy(&queue->not_empty) != CB_INFO_OK) result = CB_INFO_QUEUE_DESTROY_FAILED;

    cb__free(arena, queue->items);
    queue->items = NULL;
    queue->count = 0;

    return result;
}

cb_info_t cb_tsqueue_push(cb_tsqueue_t *queue, void *item)
{
    cb_info_t rc;

    rc = cb_mutex_lock(&queue->mutex);
    if (rc != CB_INFO_OK) return rc;

    while (queue->count == queue->capacity)
    {
        rc = cb_cond_wait(&queue->not_full, &queue->mutex);
        if (rc != CB_INFO_OK)
        {
            cb_mutex_unlock(&queue->mutex);
            return rc;
        }
    }

    queue->items[queue->tail] = item;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    cb_cond_signal(&queue->not_empty);
    cb_mutex_unlock(&queue->mutex);

    return CB_INFO_OK;
}

cb_tsqueue_item_t cb_tsqueue_pop(cb_tsqueue_t *queue)
{
    cb_tsqueue_item_t result;
    result.info = CB_INFO_OK;
    result.data = NULL;

    cb_info_t rc = cb_mutex_lock(&queue->mutex);
    if (rc != CB_INFO_OK)
    {
        result.info = rc;
        return result;
    }

    while (queue->count == 0)
    {
        rc = cb_cond_wait(&queue->not_empty, &queue->mutex);
        if (rc != CB_INFO_OK)
        {
            cb_mutex_unlock(&queue->mutex);
            result.info = rc;
            return result;
        }
    }

    result.data = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    cb_cond_signal(&queue->not_full);
    cb_mutex_unlock(&queue->mutex);

    return result;
}

cb_tsqueue_item_t cb_tsqueue_try_pop(cb_tsqueue_t *queue)
{
    cb_tsqueue_item_t result;
    result.info = CB_INFO_OK;
    result.data = NULL;

    cb_info_t rc = cb_mutex_lock(&queue->mutex);
    if (rc != CB_INFO_OK)
    {
        result.info = rc;
        return result;
    }

    if (queue->count == 0)
    {
        cb_mutex_unlock(&queue->mutex);
        result.info = CB_INFO_QUEUE_EMPTY;
        return result;
    }

    result.data = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    cb_cond_signal(&queue->not_full);
    cb_mutex_unlock(&queue->mutex);

    return result;
}

uint32_t cb_tsqueue_count(cb_tsqueue_t *queue)
{
    cb_mutex_lock(&queue->mutex);
    uint32_t c = queue->count;
    cb_mutex_unlock(&queue->mutex);
    return c;
}
