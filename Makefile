# Makefile -- ECEN 602 MP1: TCP echo server and client
#
#   make        build echos and echo
#   make test   build and run the automated harness
#   make clean  remove binaries and object files
#   make dist   clean, then produce the submission tarball

CC      := gcc
CFLAGS  := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS :=

COMMON_OBJ := echo_io.o util.o

SERVER_BIN := echos
SERVER_OBJ := echos.o $(COMMON_OBJ)

CLIENT_SRC := $(wildcard echo.c)
CLIENT_BIN := $(if $(CLIENT_SRC),echo,)

BINS := $(SERVER_BIN) $(CLIENT_BIN)

TEST_BIN := tests/test_echos

.PHONY: all clean test dist

all: $(BINS)

$(SERVER_BIN): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

ifneq ($(CLIENT_SRC),)
echo: echo.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
endif

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

echos.o   : echos.c   echo_io.h util.h
echo.o    : echo.c    echo_io.h util.h
echo_io.o : echo_io.c echo_io.h
util.o    : util.c    util.h

# -I. so the harness picks up the same ECHO_MAXLINE the server was built with.
$(TEST_BIN): tests/test_echos.c echo_io.h
	$(CC) $(CFLAGS) -I. -o $@ $<

test: $(SERVER_BIN) $(TEST_BIN)
	./$(TEST_BIN) --server ./$(SERVER_BIN)

clean:
	rm -f $(SERVER_BIN) echo *.o core core.* *~
	rm -f $(TEST_BIN)

dist: clean
	tar czf ../ecen602-mp1.tar.gz --exclude='*.tar.gz' --exclude='.git' .
	@echo "wrote ../ecen602-mp1.tar.gz"
