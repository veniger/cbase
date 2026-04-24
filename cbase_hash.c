/*
 * cbase_hash.c - SHA-256 (FIPS 180-4).
 *
 * Streaming (init/update/final) + one-shot (cb_sha256) API. No allocations;
 * context lives on the caller's stack. Byte order is handled via explicit
 * shifts — no host casts, no reliance on endianness.
 *
 * Zero-length input is valid: cb_sha256(NULL, 0, out) and
 * cb_sha256("", 0, out) both produce the canonical empty-message digest.
 *
 * SHA-256 is an unkeyed hash — it is not a MAC. Build HMAC on top when we
 * need message authentication.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* SHA-256 is defined in terms of unsigned 32-bit addition and rotation, both
   of which intentionally wrap mod 2^32. That wrap is well-defined on unsigned
   types in C, but clang's `-fsanitize=integer` (unsigned-integer-overflow)
   flags it anyway. Narrow the opt-out to just the two hot helpers — same
   pattern as cbase_random.c. */
#if defined(__clang__)
    #define CB__HASH_NO_SANITIZE_INT __attribute__((no_sanitize("unsigned-integer-overflow")))
#else
    #define CB__HASH_NO_SANITIZE_INT
#endif

/* --- Round constants (FIPS 180-4 §4.2.2) --- */

static const uint32_t cb__sha256_k[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

/* --- Initial hash values H0..H7 (FIPS 180-4 §5.3.3) --- */

static const uint32_t cb__sha256_h0[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

/* --- Inner functions (FIPS 180-4 §4.1.2) --- */

static inline uint32_t cb__rotr32(uint32_t x, unsigned n)
{
    /* n is in [1, 31] at every call site. The `(32 - n)` form is safe
       because n != 0 and n != 32. Widen to uint64_t for the left shift so
       UBSan's shift check doesn't trap on values with the high bit set
       (same trick as the PCG32 XSH-RR rotate in cbase_random.c). */
    return (x >> n) | (uint32_t)(((uint64_t)x << (32u - n)) & 0xFFFFFFFFull);
}

#define CB__CH(x, y, z)    (((x) & (y)) ^ (~(x) & (z)))
#define CB__MAJ(x, y, z)   (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define CB__BSIG0(x)       (cb__rotr32((x),  2) ^ cb__rotr32((x), 13) ^ cb__rotr32((x), 22))
#define CB__BSIG1(x)       (cb__rotr32((x),  6) ^ cb__rotr32((x), 11) ^ cb__rotr32((x), 25))
#define CB__SSIG0(x)       (cb__rotr32((x),  7) ^ cb__rotr32((x), 18) ^ ((x) >>  3))
#define CB__SSIG1(x)       (cb__rotr32((x), 17) ^ cb__rotr32((x), 19) ^ ((x) >> 10))

/* --- Compress one 64-byte block into the state --- */

CB__HASH_NO_SANITIZE_INT
static void cb__sha256_compress(uint32_t state[8], const uint8_t block[CB_SHA256_BLOCK_LEN])
{
    uint32_t w[64];

    /* W[0..15] <- big-endian words from the block */
    for (int i = 0; i < 16; ++i) {
        const uint8_t *p = block + (i * 4);
        w[i] = ((uint32_t)p[0] << 24)
             | ((uint32_t)p[1] << 16)
             | ((uint32_t)p[2] <<  8)
             | ((uint32_t)p[3]      );
    }

    /* W[16..63] <- SSIG1(W[t-2]) + W[t-7] + SSIG0(W[t-15]) + W[t-16] */
    for (int i = 16; i < 64; ++i) {
        w[i] = CB__SSIG1(w[i - 2]) + w[i - 7] + CB__SSIG0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + CB__BSIG1(e) + CB__CH(e, f, g) + cb__sha256_k[i] + w[i];
        uint32_t t2 = CB__BSIG0(a) + CB__MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/* --- Public API --- */

void cb_sha256_init(cb_sha256_ctx_t *ctx)
{
    for (int i = 0; i < 8; ++i) ctx->state[i] = cb__sha256_h0[i];
    ctx->total_bits = 0;
    ctx->buffer_len = 0;
    /* ctx->buffer intentionally left untouched — only the first buffer_len
       bytes are ever read back. */
}

void cb_sha256_update(cb_sha256_ctx_t *ctx, const void *data, size_t len)
{
    if (len == 0) return;
    const uint8_t *p = (const uint8_t *)data;

    /* Bit count update. 2^64 bits is well past anything we'd ever hash;
       wrap is defined on unsigned types anyway. */
    ctx->total_bits += (uint64_t)len * 8u;

    /* Fill partial block from buffer if present. */
    if (ctx->buffer_len > 0) {
        size_t need = CB_SHA256_BLOCK_LEN - ctx->buffer_len;
        if (len < need) {
            memcpy(ctx->buffer + ctx->buffer_len, p, len);
            ctx->buffer_len += len;
            return;
        }
        memcpy(ctx->buffer + ctx->buffer_len, p, need);
        cb__sha256_compress(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
        p   += need;
        len -= need;
    }

    /* Process full 64-byte blocks straight from input. */
    while (len >= CB_SHA256_BLOCK_LEN) {
        cb__sha256_compress(ctx->state, p);
        p   += CB_SHA256_BLOCK_LEN;
        len -= CB_SHA256_BLOCK_LEN;
    }

    /* Stash the remaining tail. */
    if (len > 0) {
        memcpy(ctx->buffer, p, len);
        ctx->buffer_len = len;
    }
}

void cb_sha256_final(cb_sha256_ctx_t *ctx, uint8_t out[CB_SHA256_DIGEST_LEN])
{
    uint64_t bits = ctx->total_bits;

    /* Append 0x80. Guaranteed to fit: buffer_len < 64. */
    ctx->buffer[ctx->buffer_len++] = 0x80u;

    /* If there isn't room for the 8-byte length field, pad this block with
       zeros, compress, and start a fresh block. */
    if (ctx->buffer_len > CB_SHA256_BLOCK_LEN - 8) {
        while (ctx->buffer_len < CB_SHA256_BLOCK_LEN) {
            ctx->buffer[ctx->buffer_len++] = 0x00u;
        }
        cb__sha256_compress(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
    }

    /* Zero-pad up to the length-field position (byte 56). */
    while (ctx->buffer_len < CB_SHA256_BLOCK_LEN - 8) {
        ctx->buffer[ctx->buffer_len++] = 0x00u;
    }

    /* Append the 64-bit big-endian bit length. */
    ctx->buffer[56] = (uint8_t)((bits >> 56) & 0xFFu);
    ctx->buffer[57] = (uint8_t)((bits >> 48) & 0xFFu);
    ctx->buffer[58] = (uint8_t)((bits >> 40) & 0xFFu);
    ctx->buffer[59] = (uint8_t)((bits >> 32) & 0xFFu);
    ctx->buffer[60] = (uint8_t)((bits >> 24) & 0xFFu);
    ctx->buffer[61] = (uint8_t)((bits >> 16) & 0xFFu);
    ctx->buffer[62] = (uint8_t)((bits >>  8) & 0xFFu);
    ctx->buffer[63] = (uint8_t)( bits        & 0xFFu);

    cb__sha256_compress(ctx->state, ctx->buffer);

    /* Emit the digest: big-endian words. */
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = (uint8_t)((ctx->state[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = (uint8_t)((ctx->state[i] >>  8) & 0xFFu);
        out[i * 4 + 3] = (uint8_t)( ctx->state[i]        & 0xFFu);
    }

    /* Ctx is "spent" after final. Callers should re-init before reuse.
       Uncomment the next line to defensively wipe state/buffer if that ever
       matters for keyed constructions (HMAC keys etc.). */
    /* memset(ctx, 0, sizeof(*ctx)); */
}

void cb_sha256(const void *input, size_t len, uint8_t out[CB_SHA256_DIGEST_LEN])
{
    cb_sha256_ctx_t ctx;
    cb_sha256_init(&ctx);
    cb_sha256_update(&ctx, input, len);
    cb_sha256_final(&ctx, out);
}

/* --- HMAC-SHA256 (RFC 2104 / FIPS 198-1) --- */

#define CB__HMAC_IPAD 0x36u
#define CB__HMAC_OPAD 0x5Cu

/* Derive a block-sized (64-byte) key per RFC 2104: if key_len > block size,
 * hash first; otherwise copy and zero-pad. */
static void cb__hmac_derive_block_key(const void *key, size_t key_len,
                                      uint8_t out_block[CB_SHA256_BLOCK_LEN])
{
    memset(out_block, 0, CB_SHA256_BLOCK_LEN);
    if (key_len > CB_SHA256_BLOCK_LEN)
    {
        cb_sha256(key, key_len, out_block);
        /* Remaining CB_SHA256_BLOCK_LEN - CB_SHA256_DIGEST_LEN bytes stay 0. */
    }
    else if (key_len > 0)
    {
        memcpy(out_block, key, key_len);
    }
}

void cb_hmac_sha256_init(cb_hmac_sha256_ctx_t *ctx, const void *key, size_t key_len)
{
    uint8_t block_key[CB_SHA256_BLOCK_LEN];
    uint8_t ipad[CB_SHA256_BLOCK_LEN];

    cb__hmac_derive_block_key(key, key_len, block_key);

    /* ipad = block_key XOR 0x36^64, opad = block_key XOR 0x5c^64. */
    for (size_t i = 0; i < CB_SHA256_BLOCK_LEN; ++i)
    {
        ipad[i]                 = (uint8_t)(block_key[i] ^ CB__HMAC_IPAD);
        ctx->outer_key_pad[i]   = (uint8_t)(block_key[i] ^ CB__HMAC_OPAD);
    }

    cb_sha256_init(&ctx->inner);
    cb_sha256_update(&ctx->inner, ipad, CB_SHA256_BLOCK_LEN);

    /* Wipe the derived block key — it is only key material, not secret
     * output, but no reason to keep it on the stack after this point. */
    memset(block_key, 0, sizeof(block_key));
    memset(ipad,      0, sizeof(ipad));
}

void cb_hmac_sha256_update(cb_hmac_sha256_ctx_t *ctx, const void *data, size_t len)
{
    cb_sha256_update(&ctx->inner, data, len);
}

void cb_hmac_sha256_final(cb_hmac_sha256_ctx_t *ctx,
                          uint8_t out[CB_HMAC_SHA256_DIGEST_LEN])
{
    uint8_t inner_digest[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx->inner, inner_digest);

    cb_sha256_ctx_t outer;
    cb_sha256_init(&outer);
    cb_sha256_update(&outer, ctx->outer_key_pad, CB_SHA256_BLOCK_LEN);
    cb_sha256_update(&outer, inner_digest,       CB_SHA256_DIGEST_LEN);
    cb_sha256_final(&outer, out);

    /* Scrub per-run key material. The inner sha ctx is already spent, but
     * the prepared opad is still sitting in ctx — clear the whole struct so
     * re-init is the only supported way to reuse. */
    memset(inner_digest, 0, sizeof(inner_digest));
    memset(ctx,          0, sizeof(*ctx));
}

void cb_hmac_sha256(const void *key, size_t key_len,
                    const void *msg, size_t msg_len,
                    uint8_t out[CB_HMAC_SHA256_DIGEST_LEN])
{
    cb_hmac_sha256_ctx_t ctx;
    cb_hmac_sha256_init(&ctx, key, key_len);
    cb_hmac_sha256_update(&ctx, msg, msg_len);
    cb_hmac_sha256_final(&ctx, out);
}
