CC      = gcc
CFLAGS  = -Wall -Wextra -g -fPIC
LDFLAGS = -L. -lthread -lpthread -Wl,-rpath,'$$ORIGIN/../lib'

# Directories
TEST_DIR = test
INSTALL_DIR = install
BIN_DIR = $(INSTALL_DIR)/bin
LIB_DIR = $(INSTALL_DIR)/lib

# Source files
SRC = thread.c
OBJ = thread.o
LIB = libthread.so

# List of tests (get all .c files in test/)
TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c, %, $(TEST_SRCS))
TEST_BINS_PTHREAD = $(patsubst %, %-pthread, $(TEST_BINS))

# --- Main targets ---

all: $(LIB) tests

# Build the shared library (.so)
$(LIB): $(SRC)
	$(CC) $(CFLAGS) -shared -o $@ $^

# Build tests with custom thread library
tests: $(LIB)
	@mkdir -p bin
	@for t in $(TEST_BINS); do \
		$(CC) $(CFLAGS) $(TEST_DIR)/$$t.c -o bin/$$t -L. -lthread -lpthread -Wl,-rpath='$$ORIGIN/../lib'; \
	done

# Build tests with pthreads (-DUSE_PTHREAD)
pthreads:
	@mkdir -p bin
	@for t in $(TEST_BINS); do \
		$(CC) $(CFLAGS) -DUSE_PTHREAD $(TEST_DIR)/$$t.c -o bin/$$t-pthread -lpthread; \
	done

# --- Installation ---

install: all pthreads
	@mkdir -p $(LIB_DIR) $(BIN_DIR)
	cp $(LIB) $(LIB_DIR)/
	cp bin/* $(BIN_DIR)/
	@echo "Installation terminée dans ./$(INSTALL_DIR)"

# --- Utilities ---

# Run all binaries in bin/ with Valgrind
valgrind: all
	@for t in $(wildcard bin/*); do \
		echo "--- Running valgrind on $$t ---"; \
		LD_LIBRARY_PATH=. valgrind --leak-check=full --show-reachable=yes --track-origins=yes ./$$t; \
	done

check: install
	@for t in $(BIN_DIR)/*; do \
		echo "Running $$t"; \
		$$t || exit 1; \
	done

clean:
	rm -rf *.o *.so bin $(INSTALL_DIR)

.PHONY: all tests pthreads install valgrind clean