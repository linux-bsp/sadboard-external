CC ?= cc
CFLAGS ?= -Wall -Wextra
DEBUG_FLAGS = -std=c11 -g3 -O0 -fno-omit-frame-pointer -fno-inline -fdebug-prefix-map=$(CURDIR)=/usr/src/sadboard

.PHONY: all clean
all: crash-bug crash-fixed

crash-bug: src/crash.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -o $@

crash-fixed: src/crash.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -DSADBOARD_FIXED $< $(LDFLAGS) -o $@

clean:
	rm -f crash-bug crash-fixed
