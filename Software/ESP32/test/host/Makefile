# Host unit tests for the ESP32 firmware modules that have no ESP-IDF
# dependencies.  Needs only a C compiler:  make
#
#   make        build and run the tests
#   make clean  remove the test binaries

CC     ?= cc
MAIN   := ../../main
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -I$(MAIN) -fsanitize=address,undefined

.PHONY: test clean

test: test_journal
	./test_journal

test_journal: test_journal.c $(MAIN)/journal.c $(MAIN)/journal.h
	$(CC) $(CFLAGS) -o $@ test_journal.c $(MAIN)/journal.c

clean:
	rm -rf test_journal test_journal.dSYM
