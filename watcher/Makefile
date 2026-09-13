CC ?= cc
CFLAGS ?= -Os -Wall -Wextra -Werror -std=c11

.PHONY: all test clean
all: ../dist/nvfanwatch
../dist/nvfanwatch: nvfanwatch.c
	mkdir -p ../dist
	$(CC) $(CFLAGS) -s -o $@ $<
test: test_policy
	./test_policy
	../dist/nvfanwatch --help
	! ../dist/nvfanwatch --profile 4
test_policy: test_policy.c nvfanwatch.c
	$(CC) $(CFLAGS) -o $@ test_policy.c
clean:
	rm -f test_policy ../dist/nvfanwatch
