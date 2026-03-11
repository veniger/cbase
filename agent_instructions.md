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

### File Layout
```
cbase.h              - Public header (all types + declarations)
cbase_union.c        - Single translation unit (includes all .c files)
cbase_arena.c        - Arena allocator implementation
cbase_threading.c    - Threading implementation (POSIX + Win32)
test_apps/           - Test programs (one .c file each)
build/               - Compiled test binaries (gitignored)
Makefile             - Build system (cross-platform)
agent_instructions.md - This file
```
