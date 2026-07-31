# --- System Detection ---
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# --- Compiler and Flags ---
LDFLAGS  := -lm
PTHREAD_LDFLAGS := -lpthread

# macOS (Apple Silicon) specific paths and flags[cite: 1]
ifeq ($(UNAME_S),Darwin)
    CC := clang
    CFLAGS   := -Wall -Wextra -O3 -std=c99
    ifeq ($(UNAME_M),arm64)
        CFLAGS += -arch arm64
    endif
    BREW_PREFIX := /opt/homebrew
    SDR_CFLAGS  := -I$(BREW_PREFIX)/include -I$(BREW_PREFIX)/include/rtl-sdr
    SDR_LDFLAGS := -L$(BREW_PREFIX)/lib -lrtlsdr
    TUI_LDFLAGS := -L$(BREW_PREFIX)/opt/ncurses/lib -lncurses -lm
    TUI_CFLAGS  := -I$(BREW_PREFIX)/opt/ncurses/include
else
    # Standard Linux paths
    CC := gcc
    CFLAGS   := -Wall -Wextra -O3 -std=gnu99
    SDR_CFLAGS  := -I/usr/include -I/usr/include/rtl-sdr
    SDR_LDFLAGS := -lrtlsdr
    TUI_LDFLAGS := -lncurses -lm
    TUI_CFLAGS  := -I/usr/include
endif

# --- Targets and Sources ---
IRADIO_TARGET := iRadio
IRADIO_SRC    := iRadio.c

# C11 overrides
IRADIO_STD   := -std=c11

# --- Default Build Rule ---
.PHONY: all
all: $(IRADIO_TARGET)

# --- Targets ---
$(IRADIO_TARGET): $(IRADIO_SRC)
	@$(CC) $(CFLAGS) $(IRADIO_STD) $(SDR_CFLAGS) $(IRADIO_SRC) -o $(IRADIO_TARGET) $(LDFLAGS) $(SDR_LDFLAGS) $(PTHREAD_LDFLAGS)


