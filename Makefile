CC = gcc
CFLAGS = -Wall -Wextra -Werror

all: thread.o

thread.o: thread.c thread.h
	$(CC) $(CFLAGS) -c thread.c -o thread.o

# Répertoire d'installation
INSTALL_DIR = install
BIN_DIR = $(INSTALL_DIR)/bin
LIB_DIR = $(INSTALL_DIR)/lib

# Fichiers à installer (à adapter selon vos binaires réels)
LIBS = libthread.a libthread.so
TESTS = 
PTHREAD_TESTS = 

install: all | $(BIN_DIR) $(LIB_DIR)
	cp $(LIBS) $(LIB_DIR) 2>/dev/null || true
	for t in $(TESTS); do cp $$t $(BIN_DIR) 2>/dev/null || true; done
	for t in $(PTHREAD_TESTS); do cp $$t $(BIN_DIR) 2>/dev/null || true; done

$(BIN_DIR):
	mkdir -p $@
$(LIB_DIR):
	mkdir -p $@

clean:
	rm -f *.o

clean:
	rm -f *.o
