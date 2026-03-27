CC      = gcc
CFLAGS  = -Wall -Wextra -g -fPIC
LDFLAGS = -L. -lthread -lpthread

# Répertoires
TEST_DIR = test
INSTALL_DIR = install
BIN_DIR = $(INSTALL_DIR)/bin
LIB_DIR = $(INSTALL_DIR)/lib

# Fichiers sources
SRC = thread.c
OBJ = thread.o
LIB = libthread.so

# Liste des tests (on récupère tous les .c dans test/)
TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c, %, $(TEST_SRCS))
TEST_BINS_PTHREAD = $(patsubst %, %-pthread, $(TEST_BINS))

# --- Cibles principales ---

all: $(LIB) tests

# Compilation de la bibliothèque partagée (.so)
$(LIB): $(SRC)
	$(CC) $(CFLAGS) -shared -o $@ $^

# Compilation des tests version "maison"
tests: $(LIB)
	@mkdir -p bin
	@for t in $(TEST_BINS); do \
		$(CC) $(CFLAGS) $(TEST_DIR)/$$t.c -o bin/$$t $(LDFLAGS); \
	done

# Compilation des tests version "pthreads" (-DUSE_PTHREAD)
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

# --- Utilitaires ---

# Exécute tous les binaires dans bin/ sous Valgrind
valgrind: all
	@for t in $(wildcard bin/*); do \
		echo "--- Running valgrind on $$t ---"; \
		LD_LIBRARY_PATH=. valgrind --leak-check=full --show-reachable=yes --track-origins=yes ./$$t; \
	done

clean:
	rm -rf *.o *.so bin $(INSTALL_DIR)

.PHONY: all tests pthreads install valgrind clean