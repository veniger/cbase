# Platform detection
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

CC       = clang
CFLAGS   = -std=c99 -Wall -Wextra -Werror -fno-omit-frame-pointer

# Sanitizers (not available on Windows/MSVC)
ifneq ($(UNAME_S),Windows)
SANITIZE = -fsanitize=address -fsanitize=undefined -fsanitize=integer \
           -fsanitize=float-divide-by-zero
# -fsanitize=nullability is clang-only (Apple/LLVM), skip on Linux gcc-based clang
ifeq ($(UNAME_S),Darwin)
SANITIZE += -fsanitize=nullability
endif
CFLAGS  += $(SANITIZE)
endif

# Linker flags
LDFLAGS  = $(SANITIZE)
ifeq ($(UNAME_S),Linux)
LDFLAGS += -lpthread -lm
endif
ifeq ($(UNAME_S),Darwin)
LDFLAGS += -lpthread -lm
endif

SRC      = cbase_union.c
TEST_DIR = test_apps
OUT_DIR  = build

TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(OUT_DIR)/%,$(TEST_SRCS))

.PHONY: test-apps clean

test-apps: $(TEST_BINS)

$(OUT_DIR)/%: $(TEST_DIR)/%.c $(SRC) cbase.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LDFLAGS)

clean:
	rm -rf $(OUT_DIR)
