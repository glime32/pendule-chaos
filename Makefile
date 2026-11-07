CC ?= cc
CFLAGS ?= -O3 -march=native -Wall -Wextra -std=c11

pendule: pendule.c
	$(CC) $(CFLAGS) -o $@ $< -lm -lpthread

clean:
	rm -f pendule

.PHONY: clean
