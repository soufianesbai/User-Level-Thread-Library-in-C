CC = gcc
CLANG_FORMAT ?= clang-format
PYTHON ?= python3
CFLAGS  = -Wall -Wextra -O3 -g -fPIC
LDFLAGS = -lpthread
RPATH_FLAGS = -Wl,-rpath,'$$ORIGIN/../lib:$$ORIGIN/..'
MULTICORE ?= 0

THREAD_SCHED_POLICY ?= THREAD_SCHED_FIFO
THREAD_SCHED_VALUE = $(THREAD_SCHED_POLICY)
ENABLE_SIGNAL ?= 0
THREAD_ENABLE_GUARD_PAGE ?= 1

CPPFLAGS = -I$(INCLUDE_DIR) -DTHREAD_SCHED_POLICY=$(THREAD_SCHED_VALUE) -DTHREAD_ENABLE_GUARD_PAGE=$(THREAD_ENABLE_GUARD_PAGE)
ifeq ($(MULTICORE),1)
CPPFLAGS += -DTHREAD_MULTICORE
CFLAGS += -pthread
endif
ifeq ($(ENABLE_SIGNAL),1)
CPPFLAGS += -DENABLE_SIGNAL
endif
# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = test
INSTALL_DIR = install
BIN_DIR = $(INSTALL_DIR)/bin
LIB_DIR = $(INSTALL_DIR)/lib
COMPAT_HEADERS = thread.h pool.h
REPORT_DIR = report
REPORT_TEX = $(REPORT_DIR)/report.tex
LATEXMK ?= /Library/TeX/texbin/latexmk

# Source files
SRC = $(wildcard $(SRC_DIR)/*.c)
ASM_SRC = $(wildcard $(SRC_DIR)/*.S)
OBJ = $(patsubst $(SRC_DIR)/%.c, %.o, $(SRC))
LIB = libthread.so
LIB_PREEM = libthread_preem.so


# List of tests (get all .c files in test/)
TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c, %, $(TEST_SRCS))
TEST_BINS_PTHREAD = $(patsubst %, %-pthread, $(TEST_BINS))
TEST_BINS_NORMAL = $(filter-out 71-preemption,$(TEST_BINS))
FORMAT_SRCS = $(wildcard $(SRC_DIR)/*.c) $(wildcard $(INCLUDE_DIR)/*.h) $(wildcard $(TEST_DIR)/*.c) $(wildcard $(TEST_DIR)/*.h)

# --- Main targets ---

all: compat-headers $(LIB) $(LIB_PREEM) tests preemptive-tests pthreads

compat-headers: $(COMPAT_HEADERS)

thread.h:
	ln -sf $(INCLUDE_DIR)/thread.h $@

pool.h:
	ln -sf $(INCLUDE_DIR)/pool.h $@

# Build the shared library (.so)
$(LIB): $(SRC) $(ASM_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -o $@ $^


# Build the preemption-enabled shared library (.so)
$(LIB_PREEM): $(SRC) $(ASM_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DENABLE_PREEMPTION -DPREEM_ENABLED -shared -o $@ $^


# Build tests with the standard thread library
tests: compat-headers $(LIB)
	@mkdir -p bin
	@for t in $(TEST_BINS_NORMAL); do \
		$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_DIR)/$$t.c -L. -lthread $(RPATH_FLAGS) -o bin/$$t $(LDFLAGS); \
	done


# Build 71-preemption with the preemption-enabled library
preemptive-tests: compat-headers $(LIB_PREEM)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_DIR)/71-preemption.c -L. -lthread_preem $(RPATH_FLAGS) -o bin/71-preemption $(LDFLAGS)


# Build tests with pthreads (-DUSE_PTHREAD)
pthreads: compat-headers
	@mkdir -p bin
	@for t in $(TEST_BINS); do \
		$(CC) $(CPPFLAGS) $(CFLAGS) -DUSE_PTHREAD -c $(TEST_DIR)/$$t.c -o bin/$$t-pthread.o; \
		$(CC) $(CPPFLAGS) $(CFLAGS) bin/$$t-pthread.o -o bin/$$t-pthread $(LDFLAGS); \
		rm -f bin/$$t-pthread.o; \
	done

# --- Installation ---

install: all pthreads
	@mkdir -p $(LIB_DIR) $(BIN_DIR)
	cp $(LIB) $(LIB_DIR)/
	cp $(LIB_PREEM) $(LIB_DIR)/
	cp bin/* $(BIN_DIR)/
	@echo "Installation terminée dans ./$(INSTALL_DIR)"

# --- Check ---

check: all
	@echo "=== Running tests ==="
	@echo "--- 01-main ---"         && bin/01-main
	@echo "--- 02-switch ---"       && bin/02-switch
	@echo "--- 03-equity ---"       && bin/03-equity
	@echo "--- 11-join ---"         && bin/11-join
	@echo "--- 12-join-main ---"    && bin/12-join-main
	@echo "--- 13-join-switch ---"  && bin/13-join-switch
	@echo "--- 21-create-many ---"          && bin/21-create-many 1000
	@echo "--- 22-create-many-recursive ---" && bin/22-create-many-recursive 500
	@echo "--- 23-create-many-once ---"      && bin/23-create-many-once 1000
	@echo "--- 31-switch-many ---"           && bin/31-switch-many 10 1000
	@echo "--- 32-switch-many-join ---"      && bin/32-switch-many-join 10 1000
	@echo "--- 33-switch-many-cascade ---"   && bin/33-switch-many-cascade 10 100
	@echo "--- 51-fibonacci ---"    && bin/51-fibonacci 16
	@echo "--- 61-mutex ---"        && bin/61-mutex 20
	@echo "--- 62-mutex ---"        && bin/62-mutex 20
	@echo "--- 63-mutex-equity ---" && bin/63-mutex-equity
	@echo "--- 64-mutex-join ---"   && bin/64-mutex-join
	@echo "--- 65-semaphore ---"    && bin/65-semaphore
	@echo "--- 66-cond ---"         && bin/66-cond
	@echo "--- 67-signal ---"       && bin/67-signal
	@echo "--- 71-preemption ---"   && bin/71-preemption 10
	@echo "--- 81-deadlock ---"     && bin/81-deadlock
	@echo "--- 82-deadlock-long ---" && bin/82-deadlock-long
	@echo "--- matrix_mul ---"         && bin/matrix_mul
	@echo "--- reduction ---"           && bin/reduction
	@echo "--- sort ---"                && bin/sort
	@echo "--- sum ---"                 && bin/sum
	@echo "--- yield_priority_test ---" && bin/yield_priority_test
	@echo "=== All tests passed ==="

# --- Utilities ---

report:
	$(LATEXMK) -synctex=1 -interaction=nonstopmode -file-line-error -pdf -outdir=$(REPORT_DIR) -auxdir=$(REPORT_DIR) $(REPORT_TEX)

# Run all tests under Valgrind with the same arguments as make check
VALGRIND = valgrind --leak-check=full --show-reachable=yes --track-origins=yes
valgrind: all
	@echo "--- 01-main ---"                  && $(VALGRIND) bin/01-main
	@echo "--- 02-switch ---"                && $(VALGRIND) bin/02-switch
	@echo "--- 03-equity ---"                && $(VALGRIND) bin/03-equity
	@echo "--- 11-join ---"                  && $(VALGRIND) bin/11-join
	@echo "--- 12-join-main ---"             && $(VALGRIND) bin/12-join-main
	@echo "--- 13-join-switch ---"           && $(VALGRIND) bin/13-join-switch
	@echo "--- 21-create-many ---"           && $(VALGRIND) bin/21-create-many 1000
	@echo "--- 22-create-many-recursive ---" && $(VALGRIND) bin/22-create-many-recursive 500
	@echo "--- 23-create-many-once ---"      && $(VALGRIND) bin/23-create-many-once 1000
	@echo "--- 31-switch-many ---"           && $(VALGRIND) bin/31-switch-many 10 1000
	@echo "--- 32-switch-many-join ---"      && $(VALGRIND) bin/32-switch-many-join 10 1000
	@echo "--- 33-switch-many-cascade ---"   && $(VALGRIND) bin/33-switch-many-cascade 10 100
	@echo "--- 51-fibonacci ---"             && $(VALGRIND) bin/51-fibonacci 16
	@echo "--- 61-mutex ---"                 && $(VALGRIND) bin/61-mutex 20
	@echo "--- 62-mutex ---"                 && $(VALGRIND) bin/62-mutex 20
	@echo "--- 63-mutex-equity ---"          && $(VALGRIND) bin/63-mutex-equity
	@echo "--- 64-mutex-join ---"            && $(VALGRIND) bin/64-mutex-join
	@echo "--- 65-semaphore ---"             && $(VALGRIND) bin/65-semaphore
	@echo "--- 66-cond ---"                  && $(VALGRIND) bin/66-cond
	@echo "--- 81-deadlock ---"              && $(VALGRIND) bin/81-deadlock
	@echo "--- 82-deadlock-long ---"         && $(VALGRIND) bin/82-deadlock-long
	@echo "--- matrix_mul ---"               && $(VALGRIND) bin/matrix_mul
	@echo "--- reduction ---"                && $(VALGRIND) bin/reduction
	@echo "--- sort ---"                     && $(VALGRIND) bin/sort
	@echo "--- sum ---"                      && $(VALGRIND) bin/sum
	@echo "--- yield_priority_test ---"      && $(VALGRIND) bin/yield_priority_test

clean:
	rm -rf *.o *.so $(SRC_DIR)/*.o bin $(INSTALL_DIR) bench $(COMPAT_HEADERS) \
		report/*.aux report/*.fls report/*.fdb_latexmk report/*.log report/*.out \
		report/*.pdf report/*.synctex.gz report/*.toc report/*.bbl report/*.blg

graphs: all pthreads
	$(PYTHON) graph_exec_time_comparison/benchmark_plot.py --custom-tests $(ARGS)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)

.PHONY: all tests preemptive-tests pthreads check clang install valgrind clean graphs format report clang-format bench-plot bench-plot-quick