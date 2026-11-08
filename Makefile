# Il progetto cresce a file interi compilati singolarmente; il motore
# unico e' il primo a stare in piu' unita'. I dieci file storici si
# compilano come sempre, uno per uno.
CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2

# il motore
motore_demo: motore.c demo_motore.c motore.h
	$(CC) $(CFLAGS) -o $@ motore.c demo_motore.c

# i capitoli storici (esempi: make database_ring)
%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f motore_demo motore_demo.exe *.db

.PHONY: clean
