CC = gcc
CLANG_FORMAT ?= clang-format
PYTHON ?= python3
CFLAGS  = -Wall -Wextra -g -fPIC
CPPFLAGS = -I$(INCLUDE_DIR)
LDFLAGS = -lpthread
RPATH_FLAGS = -Wl,-rpath,'$$ORIGIN/../lib:$$ORIGIN/..'

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = test
INSTALL_DIR = install
BIN_DIR = $(INSTALL_DIR)/bin
LIB_DIR = $(INSTALL_DIR)/lib
COMPAT_HEADERS = thread.h pool.h

# Source files
SRC = $(wildcard $(SRC_DIR)/*.c)
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
$(LIB): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -o $@ $^


# Build the preemption-enabled shared library (.so)
$(LIB_PREEM): $(SRC)
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
		$(CC) $(CPPFLAGS) $(CFLAGS) bin/$$t-pthread.o $(SRC) -o bin/$$t-pthread $(LDFLAGS); \
		rm -f bin/$$t-pthread.o; \
	done

# --- Installation ---

install: all pthreads
	@mkdir -p $(LIB_DIR) $(BIN_DIR)
	cp $(LIB) $(LIB_DIR)/
	cp $(LIB_PREEM) $(LIB_DIR)/
	cp bin/* $(BIN_DIR)/
	@echo "Installation terminée dans ./$(INSTALL_DIR)"

# --- Utilities ---

# Run all binaries in bin/ with Valgrind
valgrind: all
	@for t in $(wildcard bin/*); do \
		echo "--- Running valgrind on $$t ---"; \
		LD_LIBRARY_PATH=$(PWD)/$(INSTALL_DIR)/lib valgrind --leak-check=full --show-reachable=yes --track-origins=yes ./$$t; \
	done

clean:
	rm -rf *.o *.so $(SRC_DIR)/*.o bin $(INSTALL_DIR) bench $(COMPAT_HEADERS)

graphs: all pthreads
	$(PYTHON) scripts/benchmark_plot.py $(ARGS)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)

.PHONY: all tests preemptive-tests pthreads clang install valgrind clean graphs format clang-format bench-plot bench-plot-quick