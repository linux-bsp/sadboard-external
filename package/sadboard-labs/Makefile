CC ?= cc
CFLAGS ?= -Wall -Wextra
DEBUG_FLAGS = -std=c11 -g3 -O0 -fno-omit-frame-pointer -fno-inline -fdebug-prefix-map=$(CURDIR)=/usr/src/sadboard
PROGRAMS = crash-bug crash-fixed cpu-bug cpu-fixed memory-bug memory-fixed lock-bug lock-fixed
INSTALL ?= install

.PHONY: all install clean
all: $(PROGRAMS)

crash-bug: src/crash.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -o $@

crash-fixed: src/crash.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -DSADBOARD_FIXED $< $(LDFLAGS) -o $@

cpu-bug: src/cpu.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -O2 -Wno-unused-function $< $(LDFLAGS) -o $@

cpu-fixed: src/cpu.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -O2 -Wno-unused-function -DSADBOARD_FIXED $< $(LDFLAGS) -o $@

memory-bug lock-bug: %-bug: src/%.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -pthread -o $@

memory-fixed lock-fixed: %-fixed: src/%.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -DSADBOARD_FIXED $< $(LDFLAGS) -pthread -o $@

install: all
	$(INSTALL) -d $(DESTDIR)/usr/libexec/sadboard $(DESTDIR)/usr/src/sadboard/src
	for program in $(PROGRAMS); do $(INSTALL) -m 0755 $$program $(DESTDIR)/usr/libexec/sadboard/; done
	$(INSTALL) -D -m 0755 sadboard $(DESTDIR)/usr/bin/sadboard
	$(INSTALL) -D -m 0755 run-memory $(DESTDIR)/usr/libexec/sadboard/run-memory
	$(INSTALL) -D -m 0755 execute $(DESTDIR)/usr/libexec/sadboard/execute
	for script in run-network run-panic check-panic; do $(INSTALL) -m 0755 $$script $(DESTDIR)/usr/libexec/sadboard/; done
	$(INSTALL) -m 0644 src/*.c src/*.h $(DESTDIR)/usr/src/sadboard/src/

clean:
	rm -f $(PROGRAMS)
