#include <stdlib.h>
#include <string.h>

/* --- Arena block (internal) --- */

struct cb__arena_block_t
{
    uint8_t *data;
    size_t size;
    size_t used;
    cb__arena_block_t *next;
};

static cb__arena_block_t *cb__arena_block_create(size_t size)
{
    cb__arena_block_t *block = (cb__arena_block_t *)malloc(sizeof(cb__arena_block_t));
    if (!block) return NULL;

    block->data = (uint8_t *)malloc(size);
    if (!block->data)
    {
        free(block);
        return NULL;
    }

    block->size = size;
    block->used = 0;
    block->next = NULL;
    return block;
}

static void cb__arena_block_destroy(cb__arena_block_t *block)
{
    while (block)
    {
        cb__arena_block_t *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
}

/* --- Arena --- */

cb_arena_t cb_arena_create(size_t size, cb_arena_strategy_t strategy)
{
    cb_arena_t arena;
    arena.info = CB_INFO_OK;
    arena.strategy = strategy;
    arena.initial_size = size;

    cb__arena_block_t *block = cb__arena_block_create(size);
    if (!block)
    {
        arena.info = CB_INFO_ALLOC_FAILED;
        arena.head = NULL;
        arena.current = NULL;
        return arena;
    }

    arena.head = block;
    arena.current = block;
    return arena;
}

void cb_arena_destroy(cb_arena_t *arena)
{
    cb__arena_block_destroy(arena->head);
    arena->head = NULL;
    arena->current = NULL;
}

static size_t cb__align_forward(size_t offset, size_t align)
{
    size_t mask = align - 1;
    return (offset + mask) & ~mask;
}

cb_arena_alloc_result_t cb_arena_alloc(cb_arena_t *arena, size_t size, size_t align)
{
    cb_arena_alloc_result_t result;
    result.info = CB_INFO_OK;
    result.ptr = NULL;

    if (align == 0) align = sizeof(void *);

    /* Try to fit in current block */
    cb__arena_block_t *block = arena->current;
    size_t aligned_offset = cb__align_forward(block->used, align);

    if (aligned_offset + size <= block->size)
    {
        result.ptr = block->data + aligned_offset;
        block->used = aligned_offset + size;
        return result;
    }

    /* Check if there's a next block already (from a previous reset) */
    if (block->next)
    {
        block = block->next;
        aligned_offset = cb__align_forward(0, align);
        if (aligned_offset + size <= block->size)
        {
            arena->current = block;
            result.ptr = block->data + aligned_offset;
            block->used = aligned_offset + size;
            return result;
        }
    }

    /* Need a new block */
    size_t new_block_size;

    switch (arena->strategy)
    {
        case CB_ARENA_FIXED:
            result.info = CB_INFO_ARENA_OUT_OF_MEMORY;
            return result;

        case CB_ARENA_LINEAR:
            new_block_size = arena->initial_size;
            break;

        case CB_ARENA_EXPONENTIAL:
            new_block_size = block->size * 2;
            break;

        default:
            result.info = CB_INFO_GENERIC_ERROR;
            return result;
    }

    /* Ensure the new block is large enough for this allocation */
    if (new_block_size < size + align)
    {
        new_block_size = size + align;
    }

    cb__arena_block_t *new_block = cb__arena_block_create(new_block_size);
    if (!new_block)
    {
        result.info = CB_INFO_ALLOC_FAILED;
        return result;
    }

    /* Append to chain */
    arena->current->next = new_block;
    arena->current = new_block;

    aligned_offset = cb__align_forward(0, align);
    result.ptr = new_block->data + aligned_offset;
    new_block->used = aligned_offset + size;

    return result;
}

void cb_arena_reset(cb_arena_t *arena)
{
    cb__arena_block_t *block = arena->head;
    while (block)
    {
        block->used = 0;
        block = block->next;
    }
    arena->current = arena->head;
}

/* --- Internal alloc/free helpers --- */

void *cb__alloc(cb_arena_t *arena, size_t size, size_t align)
{
    if (!arena)
    {
        return malloc(size);
    }

    cb_arena_alloc_result_t r = cb_arena_alloc(arena, size, align);
    if (r.info != CB_INFO_OK) return NULL;
    return r.ptr;
}

void cb__free(cb_arena_t *arena, void *ptr)
{
    if (!arena)
    {
        free(ptr);
    }
    /* Arena memory is freed in bulk via destroy/reset */
}
