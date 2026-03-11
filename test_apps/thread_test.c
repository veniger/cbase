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
    int val = 42;
    cb_thread_t t = cb_thread_create(worker, &val);
    if (t.info != CB_INFO_OK)
    {
        printf("Failed to create thread\n");
        return 1;
    }

    cb_thread_result_t result = cb_thread_join(t);
    if (result.info != CB_INFO_OK)
    {
        printf("Failed to join thread\n");
        return 1;
    }

    printf("Thread joined successfully\n");

    /* Quick mutex test */
    cb_mutex_t m = cb_mutex_create();
    if (m.info != CB_INFO_OK) { printf("Mutex create failed\n"); return 1; }

    cb_mutex_lock(&m);
    printf("Mutex locked\n");
    cb_mutex_unlock(&m);
    printf("Mutex unlocked\n");
    cb_mutex_destroy(&m);

    /* Quick queue test */
    cb_tsqueue_t q = cb_tsqueue_create(8);
    if (q.info != CB_INFO_OK) { printf("Queue create failed\n"); return 1; }

    int data = 99;
    cb_tsqueue_push(&q, &data);
    printf("Pushed to queue, count: %u\n", cb_tsqueue_count(&q));

    cb_tsqueue_item_t item = cb_tsqueue_pop(&q);
    if (item.info == CB_INFO_OK)
    {
        printf("Popped from queue: %d\n", *(int *)item.data);
    }

    cb_tsqueue_destroy(&q);

    printf("All tests passed!\n");
    return 0;
}
