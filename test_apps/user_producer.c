#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../cbase.h"

#define NUM_WORKERS    5
#define QUEUE_CAPACITY 64
#define MAX_JOBS       128

/* ---------------------------------------------------------------- */
/*  Shared state                                                     */
/* ---------------------------------------------------------------- */

typedef struct
{
    int      id;
    int      job_seconds;       /* 0 = idle */
    int      job_remaining;     /* seconds left */
    int      jobs_completed;
    bool     shutdown;
} worker_state_t;

typedef struct
{
    cb_tsqueue_t     queue;
    worker_state_t   workers[NUM_WORKERS];
    cb_mutex_t       display_mutex;
    volatile bool    running;
    int              total_jobs_submitted;
    int              total_jobs_completed;
} app_state_t;

/* A job is just an int (seconds) malloc'd and pushed into the queue.
   A NULL job is the poison pill that tells a worker to exit. */

/* ---------------------------------------------------------------- */
/*  TUI rendering                                                    */
/* ---------------------------------------------------------------- */

#define BOX_W 56 /* inner width between the two border columns */
#define QVIS_COLS 32 /* slots per visualization row */
#define QVIS_ROWS ((QUEUE_CAPACITY + QVIS_COLS - 1) / QVIS_COLS)

/*
    TUI lines (1-indexed rows):
     1  top border
     2  title
     3  mid border
     4  queue info
     5  submitted/completed
     6  queue viz label
     7  queue viz row 0
     8  queue viz row 1
     9  mid border
    10  worker 0
    11  worker 1
    12  worker 2
    13  worker 3
    14  worker 4
    15  mid border
    16  instructions
    17  bottom border
    18  "> " prompt  <-- cursor lives here
*/
#define PROMPT_ROW (8 + QVIS_ROWS + NUM_WORKERS + 4)

/* Write content into a fixed-width line: "| content...padded |" */
static void print_line(const char *left, const char *content, const char *right)
{
    char buf[256];
    int len;

    len = snprintf(buf, sizeof(buf), "%s", content);
    if (len < 0) len = 0;
    if (len > BOX_W) len = BOX_W;

    printf("%s", left);
    printf("%s", buf);

    /* pad remaining columns with spaces */
    {
        int pad = BOX_W - len;
        int p;
        for (p = 0; p < pad; p++) putchar(' ');
    }

    printf("%s", right);
}

static void print_border(const char *left, const char *fill, const char *right)
{
    int i;
    printf("%s", left);
    for (i = 0; i < BOX_W; i++) printf("%s", fill);
    printf("%s", right);
}

static void render(app_state_t *state, bool initial)
{
    int i;
    char line[256];

    cb_mutex_lock(&state->display_mutex);

    if (initial)
    {
        /* Clear screen once, then all subsequent renders just overwrite in place */
        printf("\033[2J");
    }

    /* Save cursor position, jump to top-left */
    printf("\033[s\033[1;1H");

    /* Hide cursor during redraw to avoid flicker */
    printf("\033[?25l");

    print_border("\xe2\x95\x94", "\xe2\x95\x90", "\xe2\x95\x97");
    printf("\033[K\n");

    print_line("\xe2\x95\x91", "          Producer / Consumer Demo (cbase)          ", "\xe2\x95\x91");
    printf("\033[K\n");

    print_border("\xe2\x95\xa0", "\xe2\x95\x90", "\xe2\x95\xa3");
    printf("\033[K\n");

    snprintf(line, sizeof(line), "  Queue: %2u jobs pending", cb_tsqueue_count(&state->queue));
    print_line("\xe2\x95\x91", line, "\xe2\x95\x91");
    printf("\033[K\n");

    snprintf(line, sizeof(line), "  Submitted: %-4d  Completed: %-4d",
             state->total_jobs_submitted, state->total_jobs_completed);
    print_line("\xe2\x95\x91", line, "\xe2\x95\x91");
    printf("\033[K\n");

    /* Queue visualization */
    {
        uint32_t q_head, q_tail, q_count, q_cap;
        char slots[QUEUE_CAPACITY];
        int row;

        cb_mutex_lock(&state->queue.mutex);
        q_head  = state->queue.head;
        q_tail  = state->queue.tail;
        q_count = state->queue.count;
        q_cap   = state->queue.capacity;
        cb_mutex_unlock(&state->queue.mutex);

        /* Build slot characters */
        for (i = 0; i < (int)q_cap; i++)
        {
            /* Is this slot occupied? Walk from head forward count slots */
            bool occupied = false;
            if (q_count > 0)
            {
                uint32_t start = q_head;
                uint32_t end   = (q_head + q_count) % q_cap;
                if (start < end)
                    occupied = ((uint32_t)i >= start && (uint32_t)i < end);
                else
                    occupied = ((uint32_t)i >= start || (uint32_t)i < end);
            }

            bool is_head = ((uint32_t)i == q_head);
            bool is_tail = ((uint32_t)i == q_tail);

            if (is_head && is_tail)
            {
                /* Same position: show > when empty, @ when full */
                slots[i] = (q_count == q_cap) ? '@' : '>';
            }
            else if (is_head)
            {
                slots[i] = '>';
            }
            else if (is_tail)
            {
                slots[i] = '@';
            }
            else if (occupied)
            {
                slots[i] = '#';
            }
            else
            {
                slots[i] = ' ';
            }
        }

        snprintf(line, sizeof(line), "  @=producer  >=consumer  #=job");
        print_line("\xe2\x95\x91", line, "\xe2\x95\x91");
        printf("\033[K\n");

        for (row = 0; row < QVIS_ROWS; row++)
        {
            int start_idx = row * QVIS_COLS;
            int end_idx   = start_idx + QVIS_COLS;
            char vis[128];
            int pos = 0;
            int s;

            if (end_idx > (int)q_cap) end_idx = (int)q_cap;

            /* "  [" prefix */
            vis[pos++] = ' ';
            vis[pos++] = ' ';
            vis[pos++] = '[';

            for (s = start_idx; s < end_idx; s++)
            {
                vis[pos++] = slots[s];
            }

            /* Pad if last row is shorter */
            while (s < start_idx + QVIS_COLS)
            {
                vis[pos++] = ' ';
                s++;
            }

            vis[pos++] = ']';
            vis[pos] = '\0';

            print_line("\xe2\x95\x91", vis, "\xe2\x95\x91");
            printf("\033[K\n");
        }
    }

    print_border("\xe2\x95\xa0", "\xe2\x95\x90", "\xe2\x95\xa3");
    printf("\033[K\n");

    for (i = 0; i < NUM_WORKERS; i++)
    {
        worker_state_t *w = &state->workers[i];
        char bar[21];
        memset(bar, ' ', 20);
        bar[20] = '\0';

        if (w->job_seconds > 0 && w->job_remaining > 0)
        {
            int filled = 20 - (w->job_remaining * 20 / w->job_seconds);
            int j;
            if (filled > 20) filled = 20;
            for (j = 0; j < filled; j++) bar[j] = '#';

            snprintf(line, sizeof(line), "  Worker %d: [%s] %2ds/%-2ds (done: %d)",
                     i, bar, w->job_seconds - w->job_remaining,
                     w->job_seconds, w->jobs_completed);
        }
        else
        {
            snprintf(line, sizeof(line), "  Worker %d: [                    ] idle    (done: %d)",
                     i, w->jobs_completed);
        }

        print_line("\xe2\x95\x91", line, "\xe2\x95\x91");
        printf("\033[K\n");
    }

    print_border("\xe2\x95\xa0", "\xe2\x95\x90", "\xe2\x95\xa3");
    printf("\033[K\n");

    print_line("\xe2\x95\x91", "  Type a number (seconds) for a job, 'q' to quit  ", "\xe2\x95\x91");
    printf("\033[K\n");

    print_border("\xe2\x95\x9a", "\xe2\x95\x90", "\xe2\x95\x9d");
    printf("\033[K\n");

    if (initial)
    {
        /* First render: place prompt and save cursor there */
        printf("> ");
        printf("\033[?25h");
        /* Save position at the prompt so future restores land here */
        printf("\033[s");
    }
    else
    {
        /* Show cursor, restore saved cursor position (at prompt line) */
        printf("\033[?25h\033[u");
    }

    fflush(stdout);

    cb_mutex_unlock(&state->display_mutex);
}

/* ---------------------------------------------------------------- */
/*  Display refresh thread                                           */
/* ---------------------------------------------------------------- */

static cb_thread_result_t display_thread_fn(void *arg)
{
    app_state_t *state = (app_state_t *)arg;
    cb_thread_result_t res;
    res.info = CB_INFO_OK;
    res.result = NULL;

    while (state->running)
    {
        render(state, false);
        usleep(250000); /* 250ms refresh */
    }

    return res;
}

/* ---------------------------------------------------------------- */
/*  Worker thread                                                    */
/* ---------------------------------------------------------------- */

typedef struct
{
    app_state_t    *state;
    int             worker_id;
} worker_arg_t;

static cb_thread_result_t worker_wrapper(void *arg)
{
    worker_arg_t *wa = (worker_arg_t *)arg;
    app_state_t *state = wa->state;
    int id = wa->worker_id;
    worker_state_t *ws = &state->workers[id];

    cb_thread_result_t res;
    res.info = CB_INFO_OK;
    res.result = NULL;

    while (1)
    {
        cb_tsqueue_item_t item = cb_tsqueue_pop(&state->queue);
        if (item.info != CB_INFO_OK || item.data == NULL)
        {
            break; /* poison pill or error */
        }

        int seconds = *(int *)item.data;
        free(item.data);

        ws->job_seconds = seconds;
        ws->job_remaining = seconds;

        while (ws->job_remaining > 0)
        {
            sleep(1);
            ws->job_remaining--;
        }

        ws->jobs_completed++;
        ws->job_seconds = 0;
        ws->job_remaining = 0;

        /* Update global counter (protected by display_mutex for simplicity) */
        cb_mutex_lock(&state->display_mutex);
        state->total_jobs_completed++;
        cb_mutex_unlock(&state->display_mutex);
    }

    ws->shutdown = true;
    return res;
}

/* ---------------------------------------------------------------- */
/*  Input thread (reads stdin without blocking the display)          */
/* ---------------------------------------------------------------- */

typedef struct
{
    app_state_t *state;
    bool         quit_requested;
} input_arg_t;

static cb_thread_result_t input_thread_fn(void *arg)
{
    input_arg_t *ia = (input_arg_t *)arg;
    app_state_t *state = ia->state;
    cb_thread_result_t res;
    res.info = CB_INFO_OK;
    res.result = NULL;

    char buf[64];

    while (state->running)
    {
        if (fgets(buf, sizeof(buf), stdin) == NULL)
        {
            break; /* EOF */
        }

        /* Strip newline */
        buf[strcspn(buf, "\n")] = '\0';

        if (buf[0] == 'q' || buf[0] == 'Q')
        {
            ia->quit_requested = true;
            state->running = false;
            break;
        }

        int seconds = atoi(buf);
        if (seconds <= 0)
        {
            continue; /* ignore invalid input */
        }

        int *job = (int *)malloc(sizeof(int));
        if (!job) continue;
        *job = seconds;

        cb_tsqueue_push(&state->queue, job);

        cb_mutex_lock(&state->display_mutex);
        state->total_jobs_submitted++;
        /* Move cursor to prompt row, clear it, reprint prompt, save position */
        printf("\033[%d;1H\033[K> \033[s", PROMPT_ROW);
        fflush(stdout);
        cb_mutex_unlock(&state->display_mutex);
    }

    return res;
}

/* ---------------------------------------------------------------- */
/*  Main                                                             */
/* ---------------------------------------------------------------- */

int main(void)
{
    int i;
    app_state_t state;
    memset(&state, 0, sizeof(state));

    state.queue = cb_tsqueue_create(NULL, QUEUE_CAPACITY);
    if (state.queue.info != CB_INFO_OK)
    {
        fprintf(stderr, "Failed to create queue\n");
        return 1;
    }

    state.display_mutex = cb_mutex_create();
    state.running = true;

    for (i = 0; i < NUM_WORKERS; i++)
    {
        state.workers[i].id = i;
        state.workers[i].shutdown = false;
    }

    /* Launch workers */
    cb_thread_t worker_threads[NUM_WORKERS];
    worker_arg_t worker_args[NUM_WORKERS];

    for (i = 0; i < NUM_WORKERS; i++)
    {
        worker_args[i].state = &state;
        worker_args[i].worker_id = i;
        worker_threads[i] = cb_thread_create(NULL, worker_wrapper, &worker_args[i]);
        if (worker_threads[i].info != CB_INFO_OK)
        {
            fprintf(stderr, "Failed to create worker %d\n", i);
            return 1;
        }
    }

    /* Launch display thread */
    cb_thread_t display_thread = cb_thread_create(NULL, display_thread_fn, &state);

    /* Initial render (clears screen and places prompt) */
    render(&state, true);

    /* Launch input thread */
    input_arg_t input_arg;
    input_arg.state = &state;
    input_arg.quit_requested = false;
    cb_thread_t input_thread = cb_thread_create(NULL, input_thread_fn, &input_arg);

    /* Wait for input thread to finish (user typed 'q') */
    cb_thread_join(&input_thread);

    /* Send poison pills to all workers */
    for (i = 0; i < NUM_WORKERS; i++)
    {
        cb_tsqueue_push(&state.queue, NULL);
    }

    /* Wait for workers to finish current jobs and exit */
    for (i = 0; i < NUM_WORKERS; i++)
    {
        cb_thread_join(&worker_threads[i]);
    }

    /* Stop display */
    state.running = false;
    cb_thread_join(&display_thread);

    /* Final render */
    render(&state, false);
    printf("\033[%d;1H\033[K", PROMPT_ROW);
    printf("Shutdown complete. %d jobs processed.\n", state.total_jobs_completed);

    /* Cleanup */
    cb_tsqueue_destroy(NULL, &state.queue);
    cb_mutex_destroy(&state.display_mutex);

    return 0;
}
