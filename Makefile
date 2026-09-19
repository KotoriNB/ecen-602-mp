# Makefile -- ECEN 602 Machine Problem 1: TCP echo server and client
#
#   make            build everything that is present
#   make echos      build the server only
#   make test       build the server and run the automated test harness
#   make clean      remove all binaries and object files (do this before
#                   submitting -- see Submission Guideline 6)
#   make dist       clean, then produce the submission tarball

CC      := gcc
CFLAGS  := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS :=

# Code shared by both halves of the assignment.
COMMON_OBJ := echo_io.o util.o

SERVER_BIN := echos
SERVER_OBJ := echos.o $(COMMON_OBJ)

# The client (echo.c) is the other team member's half.  Build it only when
# the file is actually present, so the server always compiles on its own
# while the two halves are being developed in parallel.
CLIENT_SRC := $(wildcard echo.c)
CLIENT_BIN := $(if $(CLIENT_SRC),echo,)

BINS := $(SERVER_BIN) $(CLIENT_BIN)

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

# Header dependencies (kept explicit rather than generated, so the Makefile
# stays readable).
echos.o   : echos.c   echo_io.h util.h
echo.o    : echo.c    echo_io.h util.h
echo_io.o : echo_io.c echo_io.h
util.o    : util.c    util.h

test: $(SERVER_BIN)
	./tests/run_tests.py --server ./$(SERVER_BIN)

clean:
	rm -f $(SERVER_BIN) echo *.o core core.* *~
	rm -rf tests/__pycache__

dist: clean
	tar czf ../ecen602-mp1.tar.gz --exclude='*.tar.gz' --exclude='.git' .
	@echo "wrote ../ecen602-mp1.tar.gz"
