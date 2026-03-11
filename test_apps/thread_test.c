#include <stdio.h>
#include "../cbase.h"

cb_thread_result_t worker(void *arg)
{
    int *val = (int *)arg;
    printf("Thread got value: %d\n", *val);

    cb_thread_result_t res;
    res.info = CB_INFO_OK;
    res.result = NULL;
    return res;
}

int main(void)
{
    /* --- Thread test (NULL arena = malloc) --- */
    int val = 42;
    cb_thread_t t = cb_thread_create(NULL, worker, &val);
    if (t.info != CB_INFO_OK)
    {
        printf("Failed to create thread\n");
        return 1;
    }

    cb_thread_result_t result = cb_thread_join(&t);
    if (result.info != CB_INFO_OK)
    {
        printf("Failed to join thread\n");
        return 1;
    }

    printf("Thread joined successfully\n");

    /* --- Mutex test --- */
    cb_mutex_t m = cb_mutex_create();
    if (m.info != CB_INFO_OK) { printf("Mutex create failed\n"); return 1; }

    cb_mutex_lock(&m);
    printf("Mutex locked\n");
    cb_mutex_unlock(&m);
    printf("Mutex unlocked\n");
    cb_mutex_destroy(&m);

    /* --- Queue test (NULL arena) --- */
    cb_tsqueue_t q = cb_tsqueue_create(NULL, 8);
    if (q.info != CB_INFO_OK) { printf("Queue create failed\n"); return 1; }

    int data = 99;
    cb_tsqueue_push(&q, &data);
    printf("Pushed to queue, count: %u\n", cb_tsqueue_count(&q));

    cb_tsqueue_item_t item = cb_tsqueue_pop(&q);
    if (item.info == CB_INFO_OK)
    {
        printf("Popped from queue: %d\n", *(int *)item.data);
    }

    cb_tsqueue_destroy(NULL, &q);

    /* --- Arena test --- */
    cb_arena_t arena = cb_arena_create(1024, CB_ARENA_EXPONENTIAL);
    if (arena.info != CB_INFO_OK) { printf("Arena create failed\n"); return 1; }

    cb_arena_alloc_result_t a1 = cb_arena_alloc(&arena, 256, 16);
    if (a1.info != CB_INFO_OK) { printf("Arena alloc failed\n"); return 1; }
    printf("Arena alloc 256 bytes: OK\n");

    cb_arena_alloc_result_t a2 = cb_arena_alloc(&arena, 512, 8);
    if (a2.info != CB_INFO_OK) { printf("Arena alloc failed\n"); return 1; }
    printf("Arena alloc 512 bytes: OK\n");

    /* This should trigger a new block (exponential: 2048) */
    cb_arena_alloc_result_t a3 = cb_arena_alloc(&arena, 512, 8);
    if (a3.info != CB_INFO_OK) { printf("Arena alloc (new block) failed\n"); return 1; }
    printf("Arena alloc 512 bytes (new block): OK\n");

    cb_arena_reset(&arena);
    printf("Arena reset: OK\n");

    /* Thread with arena */
    cb_thread_t t2 = cb_thread_create(&arena, worker, &val);
    if (t2.info != CB_INFO_OK) { printf("Arena thread create failed\n"); return 1; }

    result = cb_thread_join(&t2);
    if (result.info != CB_INFO_OK) { printf("Arena thread join failed\n"); return 1; }
    printf("Arena thread joined successfully\n");

    /* Fixed arena out-of-memory test */
    cb_arena_t fixed = cb_arena_create(64, CB_ARENA_FIXED);
    if (fixed.info != CB_INFO_OK) { printf("Fixed arena create failed\n"); return 1; }

    cb_arena_alloc_result_t ok_alloc = cb_arena_alloc(&fixed, 32, 8);
    if (ok_alloc.info != CB_INFO_OK) { printf("Fixed arena first alloc failed\n"); return 1; }

    cb_arena_alloc_result_t oom = cb_arena_alloc(&fixed, 64, 8);
    if (oom.info == CB_INFO_ARENA_OUT_OF_MEMORY)
    {
        printf("Fixed arena OOM: correctly returned error\n");
    }
    else
    {
        printf("Fixed arena OOM: expected error, got OK\n");
        return 1;
    }

    /* --- Guard bytes health check --- */
    if (cb_arena_check_health(&arena))
    {
        printf("Arena health check: OK\n");
    }
    else
    {
        printf("Arena health check: CORRUPTED\n");
        return 1;
    }

    if (cb_arena_check_health(&fixed))
    {
        printf("Fixed arena health check: OK\n");
    }
    else
    {
        printf("Fixed arena health check: CORRUPTED\n");
        return 1;
    }

    cb_arena_destroy(&fixed);
    cb_arena_destroy(&arena);

    printf("All tests passed!\n");
    return 0;
}
