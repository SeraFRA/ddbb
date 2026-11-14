# Il progetto cresce a file interi compilati singolarmente; il motore
# unico e' il primo a stare in piu' unita'. I dieci file storici si
# compilano come sempre, uno per uno.
CC      = gcc
# _POSIX_C_SOURCE: con -std=c11 stretto glibc non dichiara clock_gettime
# (e la struct timespec resta pratica ideale); il define lo riporta alla
# luce su Linux senza cambiare niente su MinGW, dove l'orologio passa
# da windows.h.
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -D_POSIX_C_SOURCE=199309L
# LDLIBS: su Linux pow/exp vivono in libm, che MinGW piega dentro il
# runtime (per questo in locale linkava anche senza: la scoperta della
# CI al primo giro). -lm su MinGW e' un no-op innocuo.
LDLIBS  = -lm

# il motore
motore_demo: motore.c demo_motore.c motore.h
	$(CC) $(CFLAGS) -o $@ motore.c demo_motore.c $(LDLIBS)

# il layer MCP (capitolo 11): nucleo + guscio stdio + banco di prova
mcp_server: mcp_stdio.c mcp_server.c json.c motore.c prefetch.c mcp_server.h json.h motore.h prefetch.h
	$(CC) $(CFLAGS) -o $@ mcp_stdio.c mcp_server.c json.c motore.c prefetch.c $(LDLIBS)

demo_mcp: demo_mcp.c mcp_server.c json.c motore.c prefetch.c mcp_server.h json.h motore.h prefetch.h
	$(CC) $(CFLAGS) -o $@ demo_mcp.c mcp_server.c json.c motore.c prefetch.c $(LDLIBS)

# il prefetcher speculativo (capitolo 12): l'hook pre_agent + il banco
prefetch_hook: prefetch_hook.c prefetch.c json.c motore.c prefetch.h json.h motore.h
	$(CC) $(CFLAGS) -o $@ prefetch_hook.c prefetch.c json.c motore.c $(LDLIBS)

demo_prefetch: demo_prefetch.c prefetch.c json.c motore.c mcp_server.c prefetch.h json.h motore.h mcp_server.h
	$(CC) $(CFLAGS) -o $@ demo_prefetch.c prefetch.c json.c motore.c mcp_server.c $(LDLIBS)

# i capitoli storici (esempi: make database_ring)
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f *.exe *.db *.prefetch motore_demo mcp_server demo_mcp \
	      demo_prefetch prefetch_hook database_debug database_indexed \
	      database_compact database_persist database_learned \
	      database_semantic database_ring database_quotient \
	      database_persist_semantic

.PHONY: clean
