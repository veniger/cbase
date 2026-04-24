/*
 * cbase_bytes.c - Bounded byte-buffer reader/writer + u16 length-prefix
 * frame helper.
 *
 * No allocations. Caller provides the buffer. All multi-byte integers are
 * encoded and decoded little-endian via explicit byte shifts (host byte
 * order independent).
 *
 * Sticky info semantics: once a writer or reader's info is non-OK, every
 * subsequent op short-circuits without mutating pos and returns that info.
 * On error, info is assigned AND returned; pos is never advanced on a
 * failed op.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* --- Writer --- */

cb_bytes_writer_t cb_bytes_writer_init(uint8_t *buf, size_t cap)
{
    cb_bytes_writer_t w;
    w.info = CB_INFO_OK;
    w.data = buf;
    w.cap  = cap;
    w.pos  = 0;
    return w;
}

static inline cb_info_t cb__bytes_w_check(cb_bytes_writer_t *w, size_t n)
{
    if (w->info != CB_INFO_OK) return w->info;
    if (n > w->cap || w->pos > w->cap - n) {
        w->info = CB_INFO_BYTES_OUT_OF_BOUNDS;
        return CB_INFO_BYTES_OUT_OF_BOUNDS;
    }
    return CB_INFO_OK;
}

cb_info_t cb_bytes_write_u8(cb_bytes_writer_t *w, uint8_t v)
{
    cb_info_t info = cb__bytes_w_check(w, 1);
    if (info != CB_INFO_OK) return info;
    w->data[w->pos] = v;
    w->pos += 1;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_write_u16_le(cb_bytes_writer_t *w, uint16_t v)
{
    cb_info_t info = cb__bytes_w_check(w, 2);
    if (info != CB_INFO_OK) return info;
    w->data[w->pos + 0] = (uint8_t)( v        & 0xFFu);
    w->data[w->pos + 1] = (uint8_t)((v >> 8)  & 0xFFu);
    w->pos += 2;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_write_u32_le(cb_bytes_writer_t *w, uint32_t v)
{
    cb_info_t info = cb__bytes_w_check(w, 4);
    if (info != CB_INFO_OK) return info;
    w->data[w->pos + 0] = (uint8_t)( v        & 0xFFu);
    w->data[w->pos + 1] = (uint8_t)((v >> 8)  & 0xFFu);
    w->data[w->pos + 2] = (uint8_t)((v >> 16) & 0xFFu);
    w->data[w->pos + 3] = (uint8_t)((v >> 24) & 0xFFu);
    w->pos += 4;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_write_u64_le(cb_bytes_writer_t *w, uint64_t v)
{
    cb_info_t info = cb__bytes_w_check(w, 8);
    if (info != CB_INFO_OK) return info;
    w->data[w->pos + 0] = (uint8_t)( v        & 0xFFu);
    w->data[w->pos + 1] = (uint8_t)((v >> 8)  & 0xFFu);
    w->data[w->pos + 2] = (uint8_t)((v >> 16) & 0xFFu);
    w->data[w->pos + 3] = (uint8_t)((v >> 24) & 0xFFu);
    w->data[w->pos + 4] = (uint8_t)((v >> 32) & 0xFFu);
    w->data[w->pos + 5] = (uint8_t)((v >> 40) & 0xFFu);
    w->data[w->pos + 6] = (uint8_t)((v >> 48) & 0xFFu);
    w->data[w->pos + 7] = (uint8_t)((v >> 56) & 0xFFu);
    w->pos += 8;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_write_i8    (cb_bytes_writer_t *w, int8_t  v) { return cb_bytes_write_u8    (w, (uint8_t) v); }
cb_info_t cb_bytes_write_i16_le(cb_bytes_writer_t *w, int16_t v) { return cb_bytes_write_u16_le(w, (uint16_t)v); }
cb_info_t cb_bytes_write_i32_le(cb_bytes_writer_t *w, int32_t v) { return cb_bytes_write_u32_le(w, (uint32_t)v); }
cb_info_t cb_bytes_write_i64_le(cb_bytes_writer_t *w, int64_t v) { return cb_bytes_write_u64_le(w, (uint64_t)v); }

cb_info_t cb_bytes_write_bytes(cb_bytes_writer_t *w, const void *src, size_t n)
{
    cb_info_t info = cb__bytes_w_check(w, n);
    if (info != CB_INFO_OK) return info;
    if (n > 0) memcpy(w->data + w->pos, src, n);
    w->pos += n;
    return CB_INFO_OK;
}

size_t cb_bytes_writer_len(const cb_bytes_writer_t *w)
{
    return w->pos;
}

/* --- Reader --- */

cb_bytes_reader_t cb_bytes_reader_init(const uint8_t *buf, size_t len)
{
    cb_bytes_reader_t r;
    r.info = CB_INFO_OK;
    r.data = buf;
    r.len  = len;
    r.pos  = 0;
    return r;
}

static inline cb_info_t cb__bytes_r_check(cb_bytes_reader_t *r, size_t n)
{
    if (r->info != CB_INFO_OK) return r->info;
    if (n > r->len || r->pos > r->len - n) {
        r->info = CB_INFO_BYTES_OUT_OF_BOUNDS;
        return CB_INFO_BYTES_OUT_OF_BOUNDS;
    }
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_u8(cb_bytes_reader_t *r, uint8_t *out)
{
    cb_info_t info = cb__bytes_r_check(r, 1);
    if (info != CB_INFO_OK) return info;
    *out = r->data[r->pos];
    r->pos += 1;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_u16_le(cb_bytes_reader_t *r, uint16_t *out)
{
    cb_info_t info = cb__bytes_r_check(r, 2);
    if (info != CB_INFO_OK) return info;
    uint16_t v = (uint16_t)r->data[r->pos + 0]
               | (uint16_t)((uint16_t)r->data[r->pos + 1] << 8);
    *out = v;
    r->pos += 2;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_u32_le(cb_bytes_reader_t *r, uint32_t *out)
{
    cb_info_t info = cb__bytes_r_check(r, 4);
    if (info != CB_INFO_OK) return info;
    uint32_t v = (uint32_t)r->data[r->pos + 0]
               | ((uint32_t)r->data[r->pos + 1] << 8)
               | ((uint32_t)r->data[r->pos + 2] << 16)
               | ((uint32_t)r->data[r->pos + 3] << 24);
    *out = v;
    r->pos += 4;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_u64_le(cb_bytes_reader_t *r, uint64_t *out)
{
    cb_info_t info = cb__bytes_r_check(r, 8);
    if (info != CB_INFO_OK) return info;
    uint64_t v = (uint64_t)r->data[r->pos + 0]
               | ((uint64_t)r->data[r->pos + 1] << 8)
               | ((uint64_t)r->data[r->pos + 2] << 16)
               | ((uint64_t)r->data[r->pos + 3] << 24)
               | ((uint64_t)r->data[r->pos + 4] << 32)
               | ((uint64_t)r->data[r->pos + 5] << 40)
               | ((uint64_t)r->data[r->pos + 6] << 48)
               | ((uint64_t)r->data[r->pos + 7] << 56);
    *out = v;
    r->pos += 8;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_i8(cb_bytes_reader_t *r, int8_t *out)
{
    uint8_t u;
    cb_info_t info = cb_bytes_read_u8(r, &u);
    if (info != CB_INFO_OK) return info;
    *out = (int8_t)u;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_i16_le(cb_bytes_reader_t *r, int16_t *out)
{
    uint16_t u;
    cb_info_t info = cb_bytes_read_u16_le(r, &u);
    if (info != CB_INFO_OK) return info;
    *out = (int16_t)u;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_i32_le(cb_bytes_reader_t *r, int32_t *out)
{
    uint32_t u;
    cb_info_t info = cb_bytes_read_u32_le(r, &u);
    if (info != CB_INFO_OK) return info;
    *out = (int32_t)u;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_i64_le(cb_bytes_reader_t *r, int64_t *out)
{
    uint64_t u;
    cb_info_t info = cb_bytes_read_u64_le(r, &u);
    if (info != CB_INFO_OK) return info;
    *out = (int64_t)u;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_bytes(cb_bytes_reader_t *r, void *out, size_t n)
{
    cb_info_t info = cb__bytes_r_check(r, n);
    if (info != CB_INFO_OK) return info;
    if (n > 0) memcpy(out, r->data + r->pos, n);
    r->pos += n;
    return CB_INFO_OK;
}

size_t cb_bytes_reader_remaining(const cb_bytes_reader_t *r)
{
    return r->len - r->pos;
}

/* --- Length-prefix frame helpers (u16, little-endian) --- */

cb_info_t cb_bytes_begin_frame_u16(cb_bytes_writer_t *w, size_t *out_mark)
{
    if (w->info != CB_INFO_OK) return w->info;
    size_t mark = w->pos;
    cb_info_t info = cb_bytes_write_u16_le(w, 0);
    if (info != CB_INFO_OK) return info;
    *out_mark = mark;
    return CB_INFO_OK;
}

cb_info_t cb_bytes_end_frame_u16(cb_bytes_writer_t *w, size_t mark)
{
    if (w->info != CB_INFO_OK) return w->info;
    /* mark + 2 <= pos is guaranteed by a well-formed begin/end pair and the
       fact that every intervening write either advanced pos or set info
       non-OK (in which case we short-circuited above). */
    size_t body = w->pos - mark - 2;
    if (body > (size_t)UINT16_MAX) {
        w->info = CB_INFO_BYTES_FRAME_TOO_LARGE;
        return CB_INFO_BYTES_FRAME_TOO_LARGE;
    }
    uint16_t len = (uint16_t)body;
    w->data[mark + 0] = (uint8_t)( len        & 0xFFu);
    w->data[mark + 1] = (uint8_t)((len >> 8)  & 0xFFu);
    return CB_INFO_OK;
}

cb_info_t cb_bytes_read_frame_u16(cb_bytes_reader_t *r, cb_bytes_reader_t *out_sub)
{
    if (r->info != CB_INFO_OK) return r->info;
    uint16_t body_len;
    cb_info_t info = cb_bytes_read_u16_le(r, &body_len);
    if (info != CB_INFO_OK) return info;
    info = cb__bytes_r_check(r, (size_t)body_len);
    if (info != CB_INFO_OK) return info;
    *out_sub = cb_bytes_reader_init(r->data + r->pos, (size_t)body_len);
    r->pos += (size_t)body_len;
    return CB_INFO_OK;
}
