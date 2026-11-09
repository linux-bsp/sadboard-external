CC ?= cc
CFLAGS ?= -Wall -Wextra
DEBUG_FLAGS = -std=c11 -g3 -O0 -fno-omit-frame-pointer -fno-inline -fdebug-prefix-map=$(CURDIR)=/usr/src/sadboard
PROGRAMS = crash-bug crash-fixed cpu-bug cpu-fixed memory-bug memory-fixed lock-bug lock-fixed fd-bug fd-fixed check-result
PROGRAMS += case-suite
PROGRAMS += deadline-bug deadline-fixed backlog-bug backlog-fixed tcp-bug tcp-fixed
INSTALL ?= install

.PHONY: all install clean
all: $(PROGRAMS)

check-result: src/check-result.c src/lab.h src/basic-cases.def
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -o $@

crash-bug: src/crash.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -o $@

crash-fixed: src/crash.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -DSADBOARD_FIXED $< $(LDFLAGS) -o $@

cpu-bug: src/cpu.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -O2 -Wno-unused-function $< $(LDFLAGS) -o $@

cpu-fixed: src/cpu.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -O2 -Wno-unused-function -DSADBOARD_FIXED $< $(LDFLAGS) -o $@

memory-bug lock-bug fd-bug: %-bug: src/%.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -pthread -o $@

memory-fixed lock-fixed fd-fixed: %-fixed: src/%.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -DSADBOARD_FIXED $< $(LDFLAGS) -pthread -o $@

deadline-bug backlog-bug tcp-bug: %-bug: src/%.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -O2 $< $(LDFLAGS) -pthread -o $@

deadline-fixed backlog-fixed tcp-fixed: %-fixed: src/%.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -O2 -DSADBOARD_FIXED $< $(LDFLAGS) -pthread -o $@

SUITE_SOURCES = src/suite.c src/suite-fault.c src/suite-fd.c src/suite-memory.c src/suite-cpu.c src/suite-sync.c src/suite-network.c
case-suite: $(SUITE_SOURCES) src/suite.h src/lab.h src/basic-cases.def
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -fno-optimize-sibling-calls $(SUITE_SOURCES) $(LDFLAGS) -pthread -o $@

install: all
	$(INSTALL) -d $(DESTDIR)/usr/libexec/sadboard $(DESTDIR)/usr/src/sadboard/src
	for program in $(PROGRAMS); do $(INSTALL) -m 0755 $$program $(DESTDIR)/usr/libexec/sadboard/; done
	$(INSTALL) -D -m 0755 sadboard $(DESTDIR)/usr/bin/sadboard
	$(INSTALL) -D -m 0755 run-memory $(DESTDIR)/usr/libexec/sadboard/run-memory
	$(INSTALL) -D -m 0755 execute $(DESTDIR)/usr/libexec/sadboard/execute
	$(INSTALL) -m 0644 results.sh $(DESTDIR)/usr/libexec/sadboard/results.sh
	for script in run-suite run-network run-tcp run-panic check-panic; do $(INSTALL) -m 0755 $$script $(DESTDIR)/usr/libexec/sadboard/; done
	$(INSTALL) -m 0644 src/*.c src/*.h src/*.def $(DESTDIR)/usr/src/sadboard/src/

clean:
	rm -f $(PROGRAMS)
