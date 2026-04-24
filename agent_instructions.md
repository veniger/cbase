# cbase - Agent Instructions

## Project Overview
cbase is a cross-platform (Windows, Linux, macOS) single-file C99 utility library meant to be included in other C projects. It provides memory management, threading, math, IO, and other common utilities.

## Architecture

### Single Translation Unit Build
- `cbase_union.c` is the single translation unit that `#include`s all other `.c` files
- `cbase.h` is the public header with all type definitions and function declarations
- Each module is a separate `.c` file (e.g., `cbase_threading.c`) included by `cbase_union.c`
- Test apps in `test_apps/` each compile as a standalone program linking against `cbase_union.c`

### Adding a New Module
1. Create `cbase_<module>.c` with the implementation
2. Add declarations and types to `cbase.h` under the appropriate `/* SEG <Name> */` comment
3. Add `#include "cbase_<module>.c"` to `cbase_union.c`
4. The arena module (`cbase_arena.c`) must be included BEFORE any module that allocates

### Conventions
- **C99 standard** - no C11+ features
- **Compiler**: clang (LLVM) with `-Wall -Wextra -Werror` and all available `-fsanitize` options
- **Prefix**: all public symbols use the `cb_` prefix, internal symbols use `cb__` (double underscore)
- **Info pattern**: all structs that are returned by value have a `cb_info_t info` member as the FIRST field. `CB_INFO_OK` (value 0) means success
- **Pass-by-value for small structs**: functions that work with small structs (vectors, results, etc.) accept and return struct copies. Example: `vec3f_t a = vec3f_add(b, c);`
- **Pass-by-pointer for large/mutable structs**: structs containing OS handles, heap allocations, or internal state (mutex, queue, etc.) are passed by pointer for mutation
- **Error enum**: all error codes go in `cb_info_t` enum in `cbase.h`, grouped by module with comments
- **Segments**: sections of `cbase.h` are marked with `/* SEG <Name> */` comments for organization
- **Allocator convention**: all functions that allocate take `cb_arena_t *arena` as their first parameter. Passing `NULL` falls back to `malloc`/`free`. Internally use `cb__alloc(arena, size, align)` and `cb__free(arena, ptr)`

### Cross-Platform
- Target platforms: **Windows**, **Linux**, **macOS**
- Platform detection macros defined in `cbase.h`: `CB_PLATFORM_WINDOWS`, `CB_PLATFORM_LINUX`, `CB_PLATFORM_MACOS`, `CB_PLATFORM_POSIX` (Linux + macOS)
- Platform-specific code uses `#ifdef CB_PLATFORM_*` blocks
- POSIX platforms share code where possible (pthreads, etc.)
- Windows uses Win32 API: `CreateThread`, `CRITICAL_SECTION`, `CONDITION_VARIABLE` (Vista+)
- Struct types may contain platform-specific handle fields guarded by `#ifdef`; internal bookkeeping fields are prefixed `cb__`
- When adding platform-dependent features, implement all three platforms (or at minimum POSIX + Windows)
- The Makefile auto-detects the platform; sanitizers are enabled on POSIX only

### Build System
- `make test-apps` - compiles all `.c` files in `test_apps/` into `build/`
- `make clean` - removes `build/`
- Each test app is a standalone program that includes `cbase.h` and links with `cbase_union.c`
- On Windows, use clang or MSVC from a developer command prompt; the Makefile handles POSIX platforms

### Current Modules
- **Arena Allocator** (`cbase_arena.c`): bump allocator with three growth strategies
  - `CB_ARENA_FIXED` - single block, returns `CB_INFO_ARENA_OUT_OF_MEMORY` when full
  - `CB_ARENA_LINEAR` - allocates new blocks of the original size
  - `CB_ARENA_EXPONENTIAL` - each new block is 2x the previous
  - `cb_arena_reset()` reuses existing blocks without freeing
- **Threading** (`cbase_threading.c`): threads, mutexes, condition variables, thread-safe queue
  - POSIX: pthreads
  - Windows: Win32 threads, CRITICAL_SECTION, CONDITION_VARIABLE
  - Thread create/join use a heap-allocated trampoline arg (via `cb__alloc`) that holds fn, arg, and result; freed on join. On detach, the trampoline leaks (reclaimed by arena destroy if using an arena)
- **Random** (`cbase_random.c`): deterministic seeded PRNG for the sim. Algorithm is PCG32 (O'Neill) with canonical pcg-c "srandom" init so the reference vector for (seed=42, stream=54) matches the published demo output. Stream selector lets you run independent streams per zone/module without draw reordering desyncing another.
  - API covers raw u32/u64, unbiased bounded ints (Lemire's nearly-divisionless), i32 range, fx16 unit/range, BRAD, bool, chance(p in Q16.16), and in-place Fisher-Yates shuffle (stride cap = `CB_RNG_SHUFFLE_MAX_STRIDE` = 256, `CB_INFO_RNG_BAD_STRIDE` on overflow).
  - No floats, no libm. No global state. `cb_rng_seed` always succeeds.
  - UBSan traps to watch: the LCG step wraps mod-2^64 by design (`cb__pcg_step` is marked `no_sanitize("unsigned-integer-overflow")`); the XSH-RR rotate widens the left shift to u64 before narrowing; `i32_range` computes span in signed int64 to avoid wrap traps on `(uint32_t)hi - (uint32_t)lo`.
  - Section 7 of `test_apps/random_test.c` is the cross-platform determinism canary, same pattern as the fixed-point canary (pinned to `0x5655FF65` on macOS arm64). If it fails on a new arch, suspect the PCG32 step or the Lemire threshold first.
- **Fixed-Point Math** (`cbase_fixed.c`): deterministic math primitives for game simulation. Three types: `cb_fx16_t` (Q16.16 in int32), `cb_fx32_t` (Q32.32 in int64), `cb_brad_t` (binary radians, uint16, wraps at 65536 = full turn).
  - No floats in the sim path. `cb_fx*_from_float` / `_to_float` / `_to_double` are tooling-only.
  - No libm, no `<math.h>`. sin/cos use a 1025-entry quarter-turn LUT with linear interpolation; atan2 uses a 257-entry octant LUT; sqrt is digit-by-digit integer (bit-exact).
  - Saturating arithmetic on overflow (sign-correct to MIN/MAX). Division by zero returns MAX or MIN per dividend sign; `div(0,0) = 0`. `abs(MIN) = MAX`. sqrt of negative = 0. atan2(0,0) = 0.
  - Q32.32 `mul`/`div`/`sqrt` use `__int128` on GCC/Clang, `_mul128`/`_udiv128` on MSVC. 64-bit targets only on Windows.
  - Section 6 of `test_apps/fixed_test.c` is a pinned 10000-iteration accumulator that acts as a cross-platform determinism canary. If it fails on a new arch, investigate before anything else (most likely suspect: `cb_fx32_mul` rounding direction on the MSVC path).
- **Bytes** (`cbase_bytes.c`): bounded byte-buffer reader/writer + u16 length-prefix frame helper. Little-endian on all hosts (explicit byte shifts, not host-order casts), no allocations (caller provides the buffer).
  - Sticky info: once `info` goes non-OK on a writer or reader, every subsequent op short-circuits and keeps returning that code without advancing `pos`. Callers may chain writes/reads and check once at the end.
  - Frame helpers: `begin_frame_u16` writes two placeholder bytes and hands back a mark, `end_frame_u16` patches the mark with the body length (errors `CB_INFO_BYTES_FRAME_TOO_LARGE` if > UINT16_MAX), `read_frame_u16` decodes a length-prefixed body as a bounded sub-reader.
- **Network** (`cbase_network.c`): cross-platform UDP + TCP transport. Raw byte transport only — no framing, no tag, no reliability. Callers layer `[tag][struct bytes]` or whatever they need on top.
  - POSIX: BSD sockets + `poll()`; `cb_net_init` installs `SIG_IGN` for `SIGPIPE`
  - Windows: Winsock2 + `WSAPoll` (Vista+); `cb_net_init` calls `WSAStartup(2.2)`
  - All sockets are non-blocking after creation. I/O functions return `CB_INFO_NET_WOULD_BLOCK` when no data is ready, `CB_INFO_NET_CLOSED` when the peer hangs up (TCP)
  - `cb_net_addr_t` stores IP and port in **host byte order** for ergonomic comparisons; `htons`/`htonl` conversion happens at the BSD socket boundary only
  - `cb_tcp_connect` is blocking; `cb_tcp_accept` is non-blocking (listener is set non-blocking at listen time)
  - `cb_net_poll` wraps `poll` / `WSAPoll` over an array of `cb_net_pollable_t`
  - On Windows, link `-lws2_32` (the Makefile adds this automatically)

### File Layout
```
cbase.h              - Public header (all types + declarations)
cbase_union.c        - Single translation unit (includes all .c files)
cbase_arena.c        - Arena allocator implementation
cbase_threading.c    - Threading implementation (POSIX + Win32)
cbase_network.c      - UDP/TCP transport (POSIX BSD sockets + Win32 Winsock2)
cbase_fixed.c        - Deterministic fixed-point math (Q16.16, Q32.32, BRAD)
cbase_random.c       - PCG32 deterministic PRNG (u32/u64, bounded, fx16, BRAD, shuffle)
cbase_bytes.c        - Bounded byte-buffer reader/writer + u16 length-prefix frame helper
test_apps/           - Test programs (one .c file each)
build/               - Compiled test binaries (gitignored)
Makefile             - Build system (cross-platform)
agent_instructions.md - This file
```
