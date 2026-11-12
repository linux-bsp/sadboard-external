CC ?= cc
CFLAGS ?= -Wall -Wextra
DEBUG_FLAGS = -std=c11 -g3 -O0 -fno-omit-frame-pointer -fno-inline -fdebug-prefix-map=$(CURDIR)=/usr/src/sadboard
PROGRAMS = crash-bug crash-fixed cpu-bug cpu-fixed memory-bug memory-fixed lock-bug lock-fixed fd-bug fd-fixed check-result
PROGRAMS += case-suite watchdog-probe student-check
PROGRAMS += deadline-bug deadline-fixed backlog-bug backlog-fixed tcp-bug tcp-fixed
BOOT_PROGRAMS = boot-init libsadboard-init.so
INSTALL ?= install

.PHONY: all install clean
all: $(PROGRAMS) $(BOOT_PROGRAMS)

libsadboard-init.so: src/boot-init-library.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -shared -fPIC -Wl,-soname,libsadboard-init.so $< $(LDFLAGS) -o $@

boot-init: src/boot-init.c libsadboard-init.so
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< -L. -lsadboard-init -Wl,-rpath,/usr/lib $(LDFLAGS) -o $@

check-result: src/check-result.c src/lab.h src/basic-cases.def src/advanced-cases.def src/learning-cases.def
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

watchdog-probe: src/watchdog-probe.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -o $@

student-check: src/student-check.c src/lab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) $< $(LDFLAGS) -o $@

SUITE_SOURCES = src/suite-cma.c src/suite.c src/suite-fault.c src/suite-fd.c src/suite-memory.c src/suite-cpu.c src/suite-sync.c src/suite-network.c src/suite-performance.c src/suite-advanced-memory.c src/suite-advanced-network.c src/suite-cgroup.c src/suite-peer.c
SUITE_SOURCES += src/suite-assembly.c src/assembly-aarch64.S src/suite-kernel.c
case-suite: $(SUITE_SOURCES) src/suite.h src/suite-advanced.h src/lab.h src/basic-cases.def src/advanced-cases.def src/learning-cases.def
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEBUG_FLAGS) -fno-optimize-sibling-calls $(SUITE_SOURCES) $(LDFLAGS) -pthread -o $@

install: all
	$(INSTALL) -d $(DESTDIR)/usr/libexec/sadboard $(DESTDIR)/usr/src/sadboard/src
	for program in $(PROGRAMS); do $(INSTALL) -m 0755 $$program $(DESTDIR)/usr/libexec/sadboard/; done
	$(INSTALL) -D -m 0755 sadboard $(DESTDIR)/usr/bin/sadboard
	$(INSTALL) -D -m 0755 run-memory $(DESTDIR)/usr/libexec/sadboard/run-memory
	$(INSTALL) -D -m 0755 execute $(DESTDIR)/usr/libexec/sadboard/execute
	$(INSTALL) -m 0644 results.sh $(DESTDIR)/usr/libexec/sadboard/results.sh
	for script in run-cma run-kernel run-driver check-kernel run-network-suite cleanup-cgroups run-suite run-network run-tcp run-panic check-panic; do $(INSTALL) -m 0755 $$script $(DESTDIR)/usr/libexec/sadboard/; done
	$(INSTALL) -d $(DESTDIR)/usr/src/sadboard/kernel
	$(INSTALL) -m 0644 kernel/*.c kernel/Makefile $(DESTDIR)/usr/src/sadboard/kernel/
	$(INSTALL) -m 0644 src/*.c src/*.h src/*.def src/*.S $(DESTDIR)/usr/src/sadboard/src/

clean:
	rm -f $(PROGRAMS) $(BOOT_PROGRAMS)
